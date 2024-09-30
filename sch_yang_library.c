/**
 * @file sch_yang-library.c
 *
 * Copyright 2023, Allied Telesis Labs New Zealand, Ltd
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <glib-unix.h>
#include <ctype.h>
#include <inttypes.h>
#include <syslog.h>
#include <assert.h>
#include <apteryx.h>
#include <apteryx-xml.h>
#include <sys/inotify.h>

/* Container holding the entire YANG library of this server. */
#define YANG_LIBRARY_PATH "/yang-library"
/* An arbitrary name of the module set. */
#define YANG_LIBRARY_MODULE_SET_NAME "name"
/* An entry in this list represents a module implemented by the server, as per Section 5.6.5 of RFC 7950, with a particular set of supported features and deviations. */
#define YANG_LIBRARY_MODULE_SET_MODULE_PATH "module"
/* The YANG module or submodule name. */
#define YANG_LIBRARY_MODULE_SET_MODULE_NAME "name"
/* The YANG module or submodule revision date.  If no revision statement is present in the YANG module or submodule, this leaf is not instantiated. */
#define YANG_LIBRARY_MODULE_SET_MODULE_REVISION "revision"
/* List of all YANG feature names from this module that are supported by the server, regardless whether they are defined in the module or any included submodule. */
#define YANG_LIBRARY_MODULE_SET_MODULE_FEATURE "feature"
/* List of all YANG deviation modules used by this server to modify the conformance of the module associated with this entry.  Note that the same module can be used for deviations for multiple modules, so the same entry MAY appear within multiple 'module' entries.  This reference MUST NOT (directly or indirectly) refer to the module being deviated.  Robust clients may want to make sure that they handle a situation where a module deviates itself (directly or indirectly) gracefully. */
#define YANG_LIBRARY_MODULE_SET_MODULE_DEVIATION "deviation"
/* The YANG module or submodule name. */
#define MODULES_STATE_MODULE_NAME "name"
/* The YANG module or submodule revision date. A zero-length string is used if no revision statement is present in the YANG module or submodule. */
#define MODULES_STATE_MODULE_REVISION "revision"
/* The XML namespace identifier for this module. */
#define MODULES_STATE_MODULE_NAMESPACE "namespace"
/* An arbitrary name of the schema. */
#define YANG_LIBRARY_SCHEMA_NAME "name"
/* A set of module-sets that are included in this schema. If a non-import-only module appears in multiple module sets, then the module revision and the associated features and deviations must be identical. */
#define YANG_LIBRARY_SCHEMA_MODULE_SET "module-set"
/* The identity of the datastore. */
#define YANG_LIBRARY_DATASTORE_NAME "name"
/* A reference to the schema supported by this datastore. All non-import-only modules of the schema are implemented with their associated features and deviations. */
#define YANG_LIBRARY_DATASTORE_SCHEMA "schema"


/* Name for the set of modules */
#define MODULES_STR "modules"
#define SCHEMA_STR "schema"
#define DATASTORE_STR "datastore"
#define COMMON_STR "common"

static yang_library_callback callback = NULL;

/*
 * A wrapper for APTERYX_LEAF.
 * @param root - The node the leaf will be added to
 * @param node_name - Name of the new node
 * @param value - Value to set the node to, or NULL to delete the leaf
 */
static GNode *
add_leaf (GNode *root, char *node_name, char *value)
{
    GNode *child_node = NULL;

    assert (root);

    if (value == NULL)
    {
        value = g_strdup ("");
    }

    child_node = APTERYX_LEAF (root, (gpointer) node_name, (gpointer) value);

    return child_node;
}

/**
 * Add a leaf to a node
 *
 * @param root - The node the leaf will be added to
 * @param node_name - Name of the new node
 * @param value - Value to set the node to, or NULL to delete the leaf
 *
 * @return a pointer to the leaf,  or NULL for failure
 */
static GNode *
add_leaf_strdup (GNode *root, const char *node_name, const char *value)
{
    if (root && node_name)
    {
        return add_leaf (root, g_strdup (node_name), g_strdup (value));
    }

    return NULL;
}

/**
 * Set the state of yang-library-control. This is a state machine used to
 * coordinate multiple sources updating yang-library
 *
 * @param state - the state to set
 */
void
yang_library_control_set_state (int state)
{
    char *state_str = NULL;

    switch (state)
    {
    case YANG_LIBRARY_S_CREATED:
        state_str = "created";
        break;
    case YANG_LIBRARY_S_LOADING:
        state_str = "loading";
        break;
    case YANG_LIBRARY_S_READY:
        state_str = "ready";
        break;
    default:
        return;
    }
    apteryx_set (YANG_LIBRARY_CONTROL_STATE, state_str);
}

/**
 * Get the current state of yang-library-control
 *
 * @return volatile int the current state YANG_LIBRARY_S_xxx
 */
volatile int
yang_library_control_get_state (void)
{
    char *state_str = apteryx_get_string (YANG_LIBRARY_CONTROL_STATE, NULL);
    int state = YANG_LIBRARY_S_NONE;

    if (!state_str)
        state = YANG_LIBRARY_S_NONE;
    else if (g_strcmp0 (state_str, "created") == 0)
        state = YANG_LIBRARY_S_CREATED;
    else if (g_strcmp0 (state_str, "loading") == 0)
        state = YANG_LIBRARY_S_LOADING;
    else if (g_strcmp0 (state_str, "ready") == 0)
        state = YANG_LIBRARY_S_READY;

    g_free (state_str);
    return state;
}

/**
 * Remove an entry from the apteryx database for the specified model
 *
 * @param loaded - Structure with the relevant model information
 */
void
yang_library_remove_model_information (sch_loaded_model *loaded)
{
    char *path = g_strdup_printf ("%s/%s", YANG_LIBRARY_MOD_SET_COMMON_MOD, loaded->model);
    apteryx_prune (path);
    g_free (path);
}

/**
 * Update the features leaf-list using the updated models features
 *
 * @param loaded A control structure for a loaded model
 */
void
yang_library_update_feature_information (sch_loaded_model *loaded)
{
    GNode *root;
    GNode *modules;
    GNode *gnode;

    char *path = g_strdup_printf ("%s/%s/feature", YANG_LIBRARY_MOD_SET_COMMON_MOD, loaded->model);
    apteryx_prune (path);
    g_free (path);

    root = APTERYX_NODE (NULL, g_strdup (YANG_LIBRARY_PATH));
    modules = add_leaf_strdup (root, YANG_LIBRARY_SCHEMA_MODULE_SET, COMMON_STR);
    add_leaf_strdup (modules, YANG_LIBRARY_MODULE_SET_NAME, COMMON_STR);
    gnode = add_leaf_strdup (modules, YANG_LIBRARY_MODULE_SET_MODULE_PATH,
                             loaded->model);
    if (loaded->features)
    {
        gchar **split;
        int count;
        int i;

        split = g_strsplit (loaded->features, ",", 0);
        count = g_strv_length (split);
        for (i = 0; i < count; i++)
        {
            char *feature_path;
            feature_path = g_strdup_printf ("%s/%s",
                                            YANG_LIBRARY_MODULE_SET_MODULE_FEATURE,
                                            split[i]);
            add_leaf_strdup (gnode, feature_path, split[i]);
            g_free (feature_path);
        }
        g_strfreev (split);
    }

    apteryx_set_tree (root);
    apteryx_free_tree (root);
}

/**
 * Add an entry to the apteryx database for the specified model
 *
 * @param loaded - Structure with the relevant model information
 */
void
yang_library_add_model_information (sch_loaded_model *loaded)
{
    GNode *root;
    GNode *modules;
    GNode *gnode;

    root = APTERYX_NODE (NULL, g_strdup (YANG_LIBRARY_PATH));
    modules = add_leaf_strdup (root, YANG_LIBRARY_SCHEMA_MODULE_SET, COMMON_STR);
    add_leaf_strdup (modules, YANG_LIBRARY_MODULE_SET_NAME, COMMON_STR);
    if (loaded->model && loaded->model[0] != '\0')
    {
        gnode = add_leaf_strdup (modules, YANG_LIBRARY_MODULE_SET_MODULE_PATH,
                                 loaded->model);

        add_leaf_strdup (gnode, MODULES_STATE_MODULE_NAME, loaded->model);
        if (loaded->version)
        {
            add_leaf_strdup (gnode, MODULES_STATE_MODULE_REVISION,
                             loaded->version);
        }
        if (loaded->ns_href)
        {
            add_leaf_strdup (gnode, MODULES_STATE_MODULE_NAMESPACE, loaded->ns_href);
        }
        if (loaded->features)
        {
            gchar **split;
            int count;
            int i;

            split = g_strsplit (loaded->features, ",", 0);
            count = g_strv_length (split);
            for (i = 0; i < count; i++)
            {
                char *feature_path;
                feature_path = g_strdup_printf ("%s/%s",
                                                YANG_LIBRARY_MODULE_SET_MODULE_FEATURE,
                                                split[i]);
                add_leaf_strdup (gnode, feature_path, split[i]);
                g_free (feature_path);
            }
            g_strfreev (split);
        }
        if (loaded->deviations)
        {
            gchar **split;
            int count;
            int i;

            split = g_strsplit (loaded->deviations, ",", 0);
            count = g_strv_length (split);
            for (i = 0; i < count; i++)
            {
                char *deviation_path;
                deviation_path = g_strdup_printf ("%s/%s",
                                                  YANG_LIBRARY_MODULE_SET_MODULE_DEVIATION,
                                                  split[i]);
                add_leaf_strdup (gnode, deviation_path, split[i]);
                g_free (deviation_path);
            }
            g_strfreev (split);
        }
    }

    apteryx_set_tree (root);
    apteryx_free_tree (root);
}

/**
 * Update the yang-library content-id tag. This changes when any of the models in the schema
 * change
 *
 * @return true - when its done
 */
bool
yang_library_update_content_id (void)
{
    time_t now = time (NULL);
    uint64_t now_64 = (uint64_t) now;
    char set_id[24];

    snprintf (set_id, sizeof (set_id), "%" PRIx64 "", now_64);
    while (1)
    {
        uint64_t ts = apteryx_timestamp (YANG_LIBRARY_CONTENT_ID);
        bool success = apteryx_cas_wait (YANG_LIBRARY_CONTENT_ID, set_id, ts);
        if (success || errno != -EBUSY)
        {
            // If success is true here, watches have completed
            return success;
        }
    }
}

/**
 * Given a schema create the Apteryx data for the ietf-yang-library model required
 * by restconf.
 *
 * @param g_schema - The root schema xml node
 */
void
yang_library_create (sch_instance *schema)
{
    GNode *root;
    GNode *modules;
    GNode *datastore;
    GNode *tmp;
    GNode *sch_tmp;

    root = APTERYX_NODE (NULL, g_strdup (YANG_LIBRARY_PATH));
    modules = add_leaf_strdup (root, YANG_LIBRARY_SCHEMA_MODULE_SET, COMMON_STR);
    add_leaf_strdup (modules, YANG_LIBRARY_MODULE_SET_NAME, COMMON_STR);
    // schema_set_model_information (schema, modules);

    tmp = add_leaf_strdup (root, SCHEMA_STR, SCHEMA_STR);
    add_leaf_strdup (tmp, YANG_LIBRARY_SCHEMA_NAME, COMMON_STR);
    sch_tmp = add_leaf_strdup (tmp, YANG_LIBRARY_SCHEMA_MODULE_SET, COMMON_STR);
    add_leaf_strdup (sch_tmp, COMMON_STR, COMMON_STR);


    datastore = add_leaf_strdup (root, DATASTORE_STR, DATASTORE_STR);
    add_leaf_strdup (datastore, YANG_LIBRARY_DATASTORE_NAME, "ietf-datastores:running");
    add_leaf_strdup (datastore, YANG_LIBRARY_DATASTORE_SCHEMA, COMMON_STR);

    apteryx_set_tree (root);
    apteryx_free_tree (root);

    yang_library_control_set_state (YANG_LIBRARY_S_CREATED);

    yang_library_update_content_id ();
}

/**
 * Watch for changes in the yang-library-control data
 *
 * @param tree - the passed in updated tree
 * @return true - if successful
 * @return false - if an error occurs
 */
bool
yang_library_watch_handler (GNode *tree)
{
    GNode *node = tree;
    char *model = NULL;
    char *action = NULL;
    char *features = NULL;
    int flags = 0;

    if (!node)
        goto exit;

    node = node->children;
    if (!node)
        goto exit;

    if (g_strcmp0 (APTERYX_NAME (node), "yang-library-control") != 0)
        goto exit;

    node = node->children;
    if (!node || g_strcmp0 (APTERYX_NAME (node), "model") != 0)
        goto exit;

    node = node->children;
    if (!node)
        goto exit;

    model = APTERYX_NAME (node);
    for (GNode *iter = node->children; iter; iter = iter->next)
    {
        if (g_strcmp0 (APTERYX_NAME (iter), "name") == 0)
        {
            if (!iter->children || g_strcmp0 (APTERYX_VALUE (iter), model) != 0)
                goto exit;
        }
        else if (g_strcmp0 (APTERYX_NAME (iter), "action") == 0)
        {
            if (iter->children)
            {
                action = APTERYX_VALUE (iter);
                if (g_strcmp0 (action, "load") == 0)
                    flags |= YANG_LIBRARY_F_LOAD;
                else if (g_strcmp0 (action, "unload") == 0)
                    flags |= YANG_LIBRARY_F_UNLOAD;
                else
                {
                    syslog (LOG_ERR, "Syntax error in yang-library-control - invalid action - %s\n", action);
                    goto exit;
                }
            }
        }
        else if (g_strcmp0 (APTERYX_NAME (iter), "features-add") == 0)
        {
            if (iter->children)
            {
                features = APTERYX_VALUE (iter);
                flags |= YANG_LIBRARY_F_ADD_FEATURES;
            }
        }
        else if (g_strcmp0 (APTERYX_NAME (iter), "features-remove") == 0)
        {
            if (iter->children)
            {
                features = APTERYX_VALUE (iter);
                flags |= YANG_LIBRARY_F_REMOVE_FEATURES;
            }
        }
    }

    if (model && flags && callback)
        callback (model, flags, features);

exit:
    apteryx_free_tree (tree);
    return true;
}

/**
 * Shutdown the yang-library. Remove the apteryx watch.
 */
void
yang_library_shutdown (void)
{
    char *watch_path = g_strdup_printf ("%s/*", YANG_LIBRARY_CONTROL_MODEL);
    apteryx_unwatch_tree (watch_path, yang_library_watch_handler);
    g_free (watch_path);

    callback = NULL;
}

/**
 * Initialize the yang-library. Record the callback routine and set the apteryx watch.
 *
 * @param cb - Callback routine to handle new information in the yang-library-control tree
 */
int
yang_library_init (yang_library_callback cb)
{
    char *watch_path = g_strdup_printf ("%s/*", YANG_LIBRARY_CONTROL_MODEL);
    apteryx_watch_tree_full (watch_path, yang_library_watch_handler, 0, 0);
    g_free (watch_path);

    callback = cb;

    return 0;
}
