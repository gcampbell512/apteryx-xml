/**
 * @file schema.c
 * Utilities for validating paths against the XML schema.
 *
 * Copyright 2016, Allied Telesis Labs New Zealand, Ltd
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this library. If not, see <http://www.gnu.org/licenses/>
 */
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include <glib.h>
#include <dirent.h>
#include <fnmatch.h>
#include <syslog.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <jansson.h>
#include <regex.h>
#include <apteryx.h>
#include "apteryx-xml.h"

/* Error handling and debug */
static __thread sch_err tl_error = 0;
static __thread char tl_errmsg[BUFSIZ] = {0};

#define DEBUG(flags, fmt, args...) \
    if (flags & SCH_F_DEBUG) \
    { \
        syslog (LOG_DEBUG, fmt, ## args); \
        printf (fmt, ## args); \
    }

#define ERROR(flags, error, fmt, args...) \
    { \
        tl_error = error; \
        snprintf (tl_errmsg, BUFSIZ - 1, fmt, ## args); \
        DEBUG (flags, fmt"\n", ## args); \
    }

#define READ_BUF_SIZE 512

typedef struct _sch_instance
{
    xmlDoc *doc;
    GList *models_list;
    GHashTable *map_hash_table;
    GHashTable *model_hash_table;
    GList *regexes;
} sch_instance;

typedef enum
{
    ITEM_STATE_INIT,
    ITEM_STATE_PENDING,
    ITEM_STATE_DONE
} item_state;

typedef struct _sch_load_item
{
    char *filename;
    char *d_name;
    xmlDoc *doc_new;
    GList *dependencies;
    char *default_href;
    item_state state;
} sch_load_item;

/* Retrieve the last error code */
sch_err
sch_last_err (void)
{
    return tl_error;
}

/* Retrieve the last error message */
const char *
sch_last_errmsg (void)
{
    return tl_errmsg;
}

static void
list_doc_ns_dependencies (GList *files, sch_load_item *item)
{
    xmlNode *node = xmlDocGetRootElement (item->doc_new);
    xmlNs *ns = node->nsDef;
    GList *iter;
    sch_load_item *list_item;

    while (ns)
    {
        if (ns->href && strstr ((char *) ns->href, "www.w3.org/2001/XMLSchema-instance") == NULL &&
            g_strcmp0 ((char *) ns->href, item->default_href) != 0)
        {
            /* Find the namespace from the loaded models default name spaces */
            for (iter = g_list_first (files); iter; iter = g_list_next (iter))
            {
                list_item = iter->data;
                if (list_item->default_href &&
                    g_strcmp0 (list_item->default_href, (char *) ns->href) == 0)
                {
                    item->dependencies = g_list_append (item->dependencies, list_item);
                    break;
                }
            }
        }
        ns = ns->next;
    }
}

static void
resolve_model_dependencies (GList *list, GList **sorted)
{
    GList *iter;
    sch_load_item *item;

    for (iter = g_list_first (list); iter; iter = g_list_next (iter))
    {
        item = iter->data;
        if (item->state != ITEM_STATE_DONE)
        {
            if (item->state != ITEM_STATE_PENDING)
                item->state = ITEM_STATE_PENDING;
            else
            {
                /* Circular dependency - break loop */
                return;
            }

            if (item->dependencies)
            {
                resolve_model_dependencies (item->dependencies, sorted);
            }
            item->state = ITEM_STATE_DONE;
            *sorted = g_list_append (*sorted, item);
        }
    }
}

static gint
sort_schema_files (gconstpointer a, gconstpointer b)
{
    const sch_load_item *item1 = a;
    const sch_load_item *item2 = b;

    return strcmp (item1->d_name, item2->d_name);
}

/* List full paths for all schema files in the search path */
static void
load_schema_files (GList ** files, const char *path)
{
    DIR *dp;
    struct dirent *ep;
    char *saveptr = NULL;
    char *cpath;
    char *dpath;
    sch_load_item *new_item;
    sch_load_item *item;
    GList *iter;
    GList *sorted = NULL;
    xmlNode *node;
    xmlNs *ns;

    cpath = g_strdup (path);
    dpath = strtok_r (cpath, ":", &saveptr);
    while (dpath != NULL)
    {
        dp = opendir (dpath);
        if (dp != NULL)
        {
            while ((ep = readdir (dp)))
            {
                if ((fnmatch ("*.xml", ep->d_name, FNM_PATHNAME) != 0) &&
                    (fnmatch ("*.xml.gz", ep->d_name, FNM_PATHNAME) != 0) &&
                    (fnmatch ("*.map", ep->d_name, FNM_PATHNAME) != 0))
                {
                    continue;
                }
                new_item = g_malloc0 (sizeof (sch_load_item));
                if (dpath[strlen (dpath) - 1] == '/')
                    new_item->filename = g_strdup_printf ("%s%s", dpath, ep->d_name);
                else
                    new_item->filename = g_strdup_printf ("%s/%s", dpath, ep->d_name);
                new_item->d_name = g_strdup (ep->d_name);
                if (fnmatch ("*.map", ep->d_name, FNM_PATHNAME) != 0)
                {
                    new_item->doc_new = xmlParseFile (new_item->filename);
                    if (new_item->doc_new == NULL)
                    {
                        syslog (LOG_ERR, "XML: failed to parse \"%s\"", new_item->filename);
                        g_free (new_item->filename);
                        g_free (new_item);
                        continue;
                    }
                }
                *files = g_list_append (*files, new_item);
            }
            (void) closedir (dp);
        }
        dpath = strtok_r (NULL, ":", &saveptr);
    }
    free (cpath);
    *files = g_list_sort (*files, sort_schema_files);

    /* Get the default href for the models */
    for (iter = g_list_first (*files); iter; iter = g_list_next (iter))
    {
        item = iter->data;
        if (item->doc_new)
        {
            node = xmlDocGetRootElement (item->doc_new);
            ns = xmlSearchNs (item->doc_new, node, NULL);
            if (ns)
                item->default_href = (char *) ns->href;
        }
    }

    /* Record any model dependencies */
    for (iter = g_list_first (*files); iter; iter = g_list_next (iter))
    {
        item = iter->data;
        if (item->default_href)
            list_doc_ns_dependencies (*files, item);
    }

    resolve_model_dependencies (*files, &sorted);
    g_list_free (*files);
    *files = sorted;

    return;
}

static bool
sch_ns_node_equal (xmlNode * a, xmlNode * b)
{
    char *a_name = NULL;
    char *b_name = NULL;
    bool ret = false;

    /* Must have matching names */
    a_name = (char *) xmlGetProp (a, (xmlChar *) "name");
    b_name = (char *) xmlGetProp (b, (xmlChar *) "name");
    if (g_strcmp0 (a_name, b_name) != 0)
    {
        goto exit;
    }

    /* Must have matching namespaces */
    if ((a->ns == b->ns) ||
        (a->ns && b->ns && g_strcmp0 ((const char *)a->ns->href, (const char *)b->ns->href) == 0))
    {
        ret = true;
    }

exit:
    free (a_name);
    free (b_name);
    return ret;
}

static void
insert_in_order (xmlNs *ns, xmlNode *parent, xmlNode *child)
{
    xmlNode *sibling = NULL;
    /* Add nodes for the current model before any augmentations */
    if (ns && child->ns && g_strcmp0 ((char *) ns->href, (char *) child->ns->href) == 0)
    {
        sibling = parent->children;
        while (sibling)
        {
            if (g_strcmp0 ((char *) ns->href, (char *) sibling->ns->href) != 0)
                break;
            sibling = sibling->next;
        }
    }
    if (sibling)
    {
        if (child->parent)
            xmlUnlinkNode (child);
        // printf ("Adding %s after %s\n", xmlGetProp (child, (xmlChar *)"name"), xmlGetProp (sibling, (xmlChar *)"name"));
        xmlAddNextSibling (sibling, child);
    }
    else if (!child->parent)
    {
        // printf ("Adding %s to end\n", xmlGetProp (child, (xmlChar *)"name"));
        xmlAddChild (parent, child);
    }
}

/* Merge nodes from a new tree to the original tree */
static void
merge_nodes (xmlNs * ns, xmlNode * parent, xmlNode * orig, xmlNode * new, int depth)
{
    xmlNode *n;
    xmlNode *o;

    for (n = new; n; n = n->next)
    {
        xmlAttr* attribute = n->properties;

        /* Check if this node is already in the existing tree */
        for (o = orig; o; o = o->next)
        {
            if (sch_ns_node_equal (n, o))
            {
                /* Check to see if the model names match */
                xmlChar *mod_n = xmlGetProp (n, (xmlChar *)"model");
                if (mod_n)
                {
                    xmlChar *mod_o = xmlGetProp (o, (xmlChar *)"model");
                    if (mod_o)
                    {
                        if (g_strcmp0 ((char *) mod_o, (char *) mod_n) != 0)
                        {
                            xmlChar *name = xmlGetProp (n, (xmlChar *)"name");
                            syslog (LOG_ERR, "XML: Conflicting model names in same namespace \"%s:%s\" \"%s:%s\"",
                                (char *) mod_o, (char *) name, (char *) mod_n, (char *) name);
                            xmlFree (name);
                        }
                        xmlFree (mod_o);
                    }
                    xmlFree (mod_n);
                }

                /* Merge into the original node any new attributes from the new node */
                while (attribute && attribute->name && attribute->children)
                {
                    if (!xmlHasProp (o, attribute->name))
                    {
                        xmlChar* value = xmlNodeListGetString (n->doc, attribute->children, 1);
                        xmlSetProp (o, attribute->name, value);
                        xmlFree (value);
                    }
                    attribute = attribute->next;
                }
                break;
            }
        }
        if (o)
        {
            /* Already exists - merge in the children */
            merge_nodes (ns, o, o->children, n->children, depth + 1);
            if (depth > 0)
                insert_in_order (ns, parent, o);
        }
        else
        {
            /* New node */
            o = xmlCopyNode (n, 1);
            if (depth > 0)
                insert_in_order (ns, parent, o);
            else
                xmlAddChild (parent, o);
        }
    }
}

/* Remove unwanted nodes and attributes from a parsed tree */
static void
cleanup_nodes (xmlNode * node)
{
    xmlNode *n, *next;

    n = node;
    while (n)
    {
        next = n->next;
        if (n->type == XML_ELEMENT_NODE)
        {
            cleanup_nodes (n->children);
        }
        else
        {
            xmlUnlinkNode (n);
            xmlFreeNode (n);
        }
        n = next;
    }
}

/* Add module organisation and revision to the first child(ren) that matches the namespace */
static void
add_module_info_to_children (xmlNode *node, xmlNsPtr ns, xmlChar *mod, xmlChar *org,
                             xmlChar *ver, xmlChar *feat, xmlChar *devi)
{
    xmlNode *n = node;
    xmlNode *s = node;
    while (n)
    {
        if (n->ns && g_strcmp0 ((char *)n->ns->href, (char *)ns->href) == 0)
        {
            if (!xmlHasProp (n, (const xmlChar *)"model"))
            {
                xmlNewProp (n, (const xmlChar *)"model", mod);
                xmlNewProp (n, (const xmlChar *)"organization", org);
                xmlNewProp (n, (const xmlChar *)"version", ver);
                xmlNewProp (n, (const xmlChar *)"features", feat);
                xmlNewProp (n, (const xmlChar *)"deviations", devi);
                s = sch_node_next_sibling ((sch_node *) n);
                while (s)
                {
                    if (s->ns && g_strcmp0 ((char *)s->ns->href, (char *)ns->href) == 0)
                    {
                        if (!xmlHasProp (s, (const xmlChar *)"model"))
                        {
                            xmlNewProp (s, (const xmlChar *)"model", mod);
                            xmlNewProp (s, (const xmlChar *)"organization", org);
                            xmlNewProp (s, (const xmlChar *)"version", ver);
                            xmlNewProp (s, (const xmlChar *)"features", feat);
                            xmlNewProp (s, (const xmlChar *)"deviations", devi);
                        }
                    }
                    s = sch_node_next_sibling ((sch_node *) s);
                }
            }
        }
        else
        {
            add_module_info_to_children (n->children, ns, mod, org, ver, feat, devi);
        }
        n = n->next;
    }
}

static void
add_module_info_to_child (sch_instance *instance, xmlNode *module)
{
    xmlChar *mod = xmlGetProp (module, (xmlChar *) "model");

    if (mod)
    {
        xmlChar *org = xmlGetProp (module, (xmlChar *) "organization");
        xmlChar *ver = xmlGetProp (module, (xmlChar *) "version");
        xmlChar *feat = xmlGetProp (module, (xmlChar *) "features");
        xmlChar *devi = xmlGetProp (module, (xmlChar *) "deviations");
        xmlNsPtr def = xmlSearchNs (module->doc, module, NULL);
        add_module_info_to_children (module->children, def, mod, org, ver, feat, devi);
        xmlFree (mod);
        if (org)
            xmlFree (org);
        if (ver)
            xmlFree (ver);
        if (feat)
            xmlFree (feat);
        if (devi)
            xmlFree (devi);
    }
}

static bool
save_module_info (sch_instance *instance, xmlNode *module)
{
    sch_loaded_model *loaded;
    bool add = true;
    xmlChar *mod = xmlGetProp (module, (xmlChar *) "model");
    xmlChar *org = xmlGetProp (module, (xmlChar *) "organization");
    xmlChar *ver = xmlGetProp (module, (xmlChar *) "version");
    xmlChar *feat = xmlGetProp (module, (xmlChar *) "features");
    xmlChar *devi = xmlGetProp (module, (xmlChar *) "deviations");

    if (instance->model_hash_table)
    {
        if (!mod || strlen ((char *) mod ) == 0 ||
            !g_hash_table_lookup (instance->model_hash_table, (const char *) mod))
        {
            if (mod)
                xmlFree (mod);
            if (org)
                xmlFree (org);
            if (ver)
                xmlFree (ver);
            if (feat)
                xmlFree (feat);
            if (devi)
                xmlFree (devi);
            return false;
        }
    }

    if (mod)
    {
        /* Check for duplicate model information being saved into the list of models */
        for (GList *iter = g_list_first (instance->models_list); iter;
             iter = g_list_next (iter))
        {
            loaded = iter->data;
            if (g_strcmp0 ((char *) mod, loaded->model) == 0)
            {
                /* We have a duplicate model */
                add = false;
            }
        }
    }

    if (add)
    {
        loaded = g_malloc0 (sizeof (sch_loaded_model));
        if (loaded)
        {
            if (module->ns)
            {
                if (module->ns->href)
                {
                    loaded->ns_href = g_strdup ((char *) module->ns->href);
                }
                if (module->ns->prefix)
                {
                    loaded->ns_prefix = g_strdup ((char *) module->ns->prefix);
                }
            }
            else
            {
                xmlChar *nsp = xmlGetProp (module, (xmlChar *) "namespace");
                xmlChar *pre = xmlGetProp (module, (xmlChar *) "prefix");

                if (nsp)
                {
                    loaded->ns_href = (char *) nsp;
                }
                if (pre)
                {
                    loaded->ns_prefix = (char *) pre;
                }
            }
            if (mod)
            {
                loaded->model = g_strdup ((char *) mod);
            }
            if (org)
            {
                loaded->organization = g_strdup ((char *) org);
            }
            if (ver)
            {
                loaded->version = g_strdup ((char *) ver);
            }
            if (feat)
            {
                loaded->features = g_strdup ((char *) feat);
            }
            if (devi)
            {
                loaded->deviations = g_strdup ((char *) devi);
            }
            instance->models_list = (void *) g_list_append (instance->models_list, loaded);
        }
    }

    if (mod)
        xmlFree (mod);
    if (org)
        xmlFree (org);
    if (ver)
        xmlFree (ver);
    if (feat)
        xmlFree (feat);
    if (devi)
        xmlFree (devi);

    return true;
}

static void
copy_nsdef_to_root (xmlDoc *doc, xmlNode *node)
{
    xmlNode *n = node;
    while (n)
    {
        /* Copy any NS defintions to the new root */
        xmlNsPtr ns = n->nsDef;
        while (ns)
        {
            if (ns->href && !xmlSearchNsByHref (doc, xmlDocGetRootElement (doc), ns->href))
            {
                char *prefix = (char *) ns->prefix;
                if (!prefix)
                    prefix = (char *) xmlGetProp (n, (xmlChar *)"prefix");
                if (prefix)
                    xmlNewNs (xmlDocGetRootElement (doc), ns->href, (xmlChar *)prefix);
                if (!ns->prefix)
                    free (prefix);
            }
            ns = ns->next;
        }

        /* Recurse */
        copy_nsdef_to_root (doc, n->children);
        n = n->next;
    }
}

void
assign_ns_to_root (xmlDoc *doc, xmlNode *node)
{
    xmlNode *n = node;
    while (n)
    {
        /* Assign this nodes ns to the new root if needed */
        if (n->ns)
            n->ns = xmlSearchNsByHref (doc, xmlDocGetRootElement (doc), n->ns->href);

        /* Recurse */
        assign_ns_to_root (doc, n->children);

        /* Chuck away the local NS */
        if (n->nsDef) {
            xmlFreeNsList (n->nsDef);
            n->nsDef = NULL;
        }

        n = n->next;
     }
 }

static void
sch_load_namespace_mappings (sch_instance *instance, const char *filename)
{
    FILE *fp = NULL;
    gchar **ns_names;
    char *buf;

    if (!instance)
        return;

    buf = g_malloc0 (READ_BUF_SIZE);
    fp = fopen (filename, "r");
    if (fp && buf)
    {
        if (!instance->map_hash_table)
            instance->map_hash_table = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

        while (fgets (buf, READ_BUF_SIZE, fp) != NULL)
        {
            /* Skip comment lines */
            if (buf[0] == '#')
                continue;

            /* Remove any trailing LF */
            buf[strcspn(buf, "\n")] = '\0';

            ns_names = g_strsplit (buf, " ", 2);
            if (ns_names[0] && ns_names[1])
            {
                /* Insert will take care of duplicates automatically. */
                g_hash_table_insert (instance->map_hash_table, g_strdup (ns_names[0]),
                                     g_strdup (ns_names[1]));
            }
            g_strfreev (ns_names);
        }
        fclose (fp);
    }
    g_free (buf);
}

static void
sch_load_model_list (sch_instance *instance, const char *path, const char *model_list_filename)
{
    FILE *fp = NULL;
    char *name;
    char *buf;

    if (!instance)
        return;

    buf = g_malloc0 (READ_BUF_SIZE);
    name = g_strdup_printf ("%s/%s", path, model_list_filename);
    fp = fopen (name, "r");
    if (fp && buf)
    {
        if (!instance->model_hash_table)
            instance->model_hash_table = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

        while (fgets (buf, READ_BUF_SIZE, fp) != NULL)
        {
            /* Skip comment lines */
            if (buf[0] == '#')
                continue;

            /* Remove any trailing LF */
            buf[strcspn(buf, "\n")] = '\0';
            if (strlen (buf) > 0)
            {
                void *old_key;
                void *old_value;

                /* Look up this node name to check for duplicates. */
                if (g_hash_table_lookup_extended (instance->model_hash_table, buf,
                                                  &old_key, &old_value))
                {
                    g_free (old_key);
                    g_free (old_value);
                }
                else
                {
                    g_hash_table_insert (instance->model_hash_table, g_strdup (buf),
                                         g_strdup (buf));
                }
            }
        }
        fclose (fp);
    }
    g_free (name);
    g_free (buf);
}

void
sch_load_item_free (void *data)
{
    sch_load_item *item = data;
    g_free (item->filename);
    g_free (item->d_name);
    g_list_free (item->dependencies);
    g_free (item);
}

/* Parse all XML files in the search path and merge trees */
static sch_instance *
_sch_load (const char *path, const char *model_list_filename)
{
    sch_instance *instance;
    xmlNode *module;
    xmlNs *ns;
    GList *files = NULL;
    GList *iter;

    /* New instance */
    instance = g_malloc0 (sizeof (sch_instance));

    /* Create a new doc and root node for the merged MODULE */
    instance->doc = xmlNewDoc ((xmlChar *) "1.0");
    module = xmlNewNode (NULL, (xmlChar *) "MODULE");
    ns = xmlNewNs (module, (const xmlChar *) "https://github.com/alliedtelesis/apteryx", NULL);
    xmlSetNs (module, ns);
    xmlNewNs (module, (const xmlChar *) "http://www.w3.org/2001/XMLSchema-instance", (const xmlChar *) "xsi");
    xmlNewProp (module, (const xmlChar *) "xsi:schemaLocation",
        (const xmlChar *) "https://github.com/alliedtelesis/apteryx-xml https://github.com/alliedtelesis/apteryx-xml/releases/download/v1.2/apteryx.xsd");
    xmlDocSetRootElement (instance->doc, module);

    if (model_list_filename)
        sch_load_model_list (instance, path, model_list_filename);

    load_schema_files (&files, path);
    for (iter = files; iter; iter = g_list_next (iter))
    {
        sch_load_item *item;
        char *filename;
        char *ext;

        item = iter->data;
        filename = item->filename;
        ext = strrchr(filename, '.');
        if (g_strcmp0 (ext, ".map") == 0)
        {
            sch_load_namespace_mappings (instance, filename);
            continue;
        }

        xmlNode *module_new = xmlDocGetRootElement (item->doc_new);
        cleanup_nodes (module_new);
        /* Sanity check for empty modules */
        if (!module_new || (module_new->children && (module_new->children->name[0] != 'N' && module_new->children->name[0] != 'S')))
        {
            syslog (LOG_ERR, "XML: ignoring empty schema \"%s\"", filename);
            continue;
        }
        copy_nsdef_to_root (instance->doc, module_new);
        if (save_module_info (instance, module_new))
        {
            add_module_info_to_child (instance, module_new);
            merge_nodes (module_new->ns, module, module->children, module_new->children, 0);
            xmlFreeDoc (item->doc_new);
            item->doc_new = NULL;
            assign_ns_to_root (instance->doc, module->children);
        }
        else
        {
            xmlFreeDoc (item->doc_new);
            item->doc_new = NULL;
        }
    }
    g_list_free_full (files, sch_load_item_free);

    /* Store a link back to the instance in the xmlDoc stucture */
    instance->doc->_private = (void *) instance;

    return instance;
}

sch_instance *
sch_load (const char *path)
{
    return _sch_load (path, NULL);
}

/**
 * Only load XML models that are specified in the model list file. If the model list
 * filename is NULL, all models are loaded.
 */
sch_instance *
sch_load_with_model_list_filename (const char *path, const char *model_list_filename)
{
    return _sch_load (path, model_list_filename);
}

static void
sch_free_loaded_models (GList *loaded_models)
{
    GList *list;
    sch_loaded_model *loaded;

    if (loaded_models)
    {
        for (list = g_list_first (loaded_models); list; list = g_list_next (list))
        {
            loaded = list->data;
            if (loaded->ns_href)
            {
                g_free (loaded->ns_href);
            }
            if (loaded->model)
            {
                g_free (loaded->model);
            }
            if (loaded->organization)
            {
                g_free (loaded->organization);
            }
            if (loaded->ns_prefix)
            {
                g_free (loaded->ns_prefix);
            }
            if (loaded->version)
            {
                g_free (loaded->version);
            }
            if (loaded->features)
            {
                g_free (loaded->features);
            }
            if (loaded->deviations)
            {
                g_free (loaded->deviations);
            }
            g_free (loaded);
        }
        g_list_free (loaded_models);
        loaded_models = NULL;
    }
}

static void
free_regex (regex_t *regex_obj)
{
    regfree (regex_obj);
    g_free (regex_obj);
}

void
sch_free (sch_instance * instance)
{
    if (instance)
    {
        if (instance->models_list)
            sch_free_loaded_models (instance->models_list);
        if (instance->doc)
            xmlFreeDoc (instance->doc);
        if (instance->map_hash_table)
            g_hash_table_destroy (instance->map_hash_table);
        if (instance->model_hash_table)
            g_hash_table_destroy (instance->model_hash_table);
        if (instance->regexes)
            g_list_free_full (instance->regexes, (GDestroyNotify) free_regex);

        g_free (instance);
    }
}

GList *
sch_get_loaded_models (sch_instance * instance)
{
    return instance->models_list;
}

gboolean
sch_match_name (const char *s1, const char *s2)
{
    char c1, c2;
    do
    {
        c1 = *s1;
        c2 = *s2;
        if (c1 == '\0' && c2 == '\0')
            return true;
        if (c1 == '-')
            c1 = '_';
        if (c2 == '-')
            c2 = '_';
        s1++;
        s2++;
    }
    while (c1 == c2);
    return false;
}

static bool
_sch_ns_native (sch_instance *instance, xmlNs *ns)
{
    /* No namespace means native */
    if (!ns)
        return true;
    /* Root namespace is considered native */
    if (instance && ns == xmlDocGetRootElement (instance->doc)->ns)
        return true;
    /* Check if namespace is in the table of non-native namespaces */
    if (instance && instance->map_hash_table)
    {
        if (g_hash_table_lookup (instance->map_hash_table, (const char *) ns->href))
            return false;
    }
    return true;
}

bool
sch_ns_native (sch_instance *instance, sch_ns *ns)
{
    return _sch_ns_native (instance, (xmlNs *)ns);
}

static bool
remove_hidden_children (xmlNode *node)
{
    xmlNode *child;

    if (node == NULL)
        return false;

    /* Keep all the value nodes */
    if (node->name[0] == 'V')
        return true;

    /* Throw away any non schema nodes */
    if (node->name[0] != 'M' && node->name[0] != 'N')
        return false;

    if (sch_is_hidden (node))
        return false;

    child = node->children;
    while (child)
    {
        if (!remove_hidden_children (child))
        {
            xmlNode *dchild = child;
            child = child->next;
            xmlUnlinkNode (dchild);
            xmlFreeNode (dchild);
        }
        else
        {
            child = child->next;
        }
    }

    return true;
}

static void
format_api_namespaces (sch_instance * instance, xmlNs *ns, xmlNode *node, int depth)
{
    xmlNode *child;

    if (node == NULL)
        return;

    child = node->children;
    while (child)
    {
        if (depth == 0 && child->ns && child->ns->prefix && !_sch_ns_native (instance, child->ns))
        {
            /* Replace top-level nodes of non-native models with the namespace prefixed name */
            char *old = sch_name (child);
            char *name = g_strdup_printf ("%s:%s",child->ns->prefix, old);
            xmlSetProp (child, (const xmlChar *)"name", (const xmlChar *)name);
            free (name);
            free (old);
        }
        format_api_namespaces (instance, ns, child, depth + 1);
        /* Everything in the apteryx namespace */
        child->ns = ns;
        child = child->next;
    }

    /* Get rid of all non default namespace definitions on the root node */
    if (depth == 0)
    {
        xmlNs *cur = node->nsDef;
        while (cur != NULL)
        {
            xmlNs *next = cur->next;
            if (cur != ns)
                xmlFreeNs (cur);
            cur = next;
        }
        node->nsDef = ns;
        ns->next = NULL;
        xmlNewNs (node, (const xmlChar *) "http://www.w3.org/2001/XMLSchema-instance", (const xmlChar *) "xsi");
    }

    return;
}

static gint
xmlNodeCmp (gconstpointer a, gconstpointer b)
{
    char *aname = (char *) xmlGetProp ((xmlNode *) a, (xmlChar *) "name");
    char *bname = (char *) xmlGetProp ((xmlNode *) b, (xmlChar *) "name");
    gint result = g_strcmp0 (aname, bname);
    free (aname);
    free (bname);
    return result;
}

static void
sort_root_nodes (xmlNode *module)
{
    GList *nodes = NULL;
    xmlNode *child = module->children;
    while (child)
    {
        xmlNode *next = child->next;
        nodes = g_list_prepend (nodes, child);
        xmlUnlinkNode (child);
        child = next;
    }
    nodes = g_list_sort (nodes, xmlNodeCmp);
    xmlNode *prev = NULL;
    for (GList *iter = nodes; iter; iter = iter->next)
    {
        child = (xmlNode *) iter->data;
        if (prev)
            xmlAddNextSibling (prev, child);
        else
            xmlAddChild (module, child);
        prev = child;
    }
    g_list_free (nodes);
}

char *
sch_dump_xml (sch_instance * instance)
{
    xmlNode *xml = xmlDocGetRootElement (instance->doc);
    xmlChar *xmlbuf = NULL;
    int bufsize;

    xmlDoc *copy = xmlCopyDoc (xml->doc, 1);
    remove_hidden_children (xmlDocGetRootElement (copy));
    format_api_namespaces (instance, xmlDocGetRootElement (copy)->ns, xmlDocGetRootElement (copy), 0);
    sort_root_nodes (xmlDocGetRootElement (copy));
    xmlDocDumpFormatMemory (copy, &xmlbuf, &bufsize, 1);
    xmlFreeDoc (copy);
    return (char *) xmlbuf;
}

static bool
_sch_ns_match (xmlNode *node, xmlNs *ns)
{
    sch_instance *instance = node->doc->_private;

    /* Actually the same namespace (object) */
    if (node->ns == ns)
        return true;

    /* NULL == the global namespace */
    if (!ns && node->ns == xmlDocGetRootElement (instance->doc)->ns)
        return true;

    /* Check if both namespaces are part of the global namespace */
    if (_sch_ns_native (instance, ns) && _sch_ns_native (instance, node->ns))
        return true;

    /* Search up the tree until the root<MODULE> for an exact namespace match */
    while (ns && node && node->type == XML_ELEMENT_NODE && node->name[0] == 'N')
    {
        if (node->ns && node->ns->href &&
            g_strcmp0 ((const char *) node->ns->href, (const char *) ns->href) == 0)
            return true;
        node = node->parent;
    }

    /* No match */
    return false;
}

bool
sch_ns_match (sch_node *node, sch_ns *ns)
{
    return _sch_ns_match (node, ns);
}

sch_ns *
sch_node_ns (sch_node * node)
{
    xmlNode *xml = (xmlNode *) node;
    return xml ? (sch_ns *) xml->ns : NULL;
}

const char *
sch_ns_prefix (sch_instance *instance, sch_ns *ns)
{
    return ns ? (const char *) ((xmlNs *)ns)->prefix : NULL;
}

const char *
sch_ns_href (sch_instance *instance, sch_ns *ns)
{
    return ns ? (const char *) ((xmlNs *)ns)->href : NULL;
}

sch_node *
sch_get_root_schema (sch_instance * instance)
{
    return (instance ? xmlDocGetRootElement (instance->doc) : NULL);
}

static xmlNs *
_sch_lookup_ns (sch_instance * instance, xmlNode *schema, const char *name, int flags, bool href)
{
    xmlNs *ns = NULL;
    xmlNode *xml;

    if (!schema)
        schema = xmlDocGetRootElement (instance->doc);

    xml = sch_node_child_first (schema);
    while (xml && xml->type == XML_ELEMENT_NODE)
    {
        if (flags & SCH_F_NS_MODEL_NAME)
        {
            char *model;

            model = (char *) xmlGetProp (xml, (xmlChar *) "model");
            if (model)
            {
                if (g_strcmp0 (model, name) == 0)
                {
                    free (model);
                    ns = xml->ns;
                    break;
                }
                free (model);
            }
        }

        if (xml->ns &&
            ((href && xml->ns->href && g_strcmp0 ((char *) xml->ns->href, name) == 0) ||
             (!href && xml->ns->prefix && g_strcmp0 ((char *) xml->ns->prefix, name) == 0)))
        {
            ns = xml->ns;
            break;
        }
        xml = xml->next;
    }

    return ns;
}

sch_ns *
sch_lookup_ns (sch_instance * instance, sch_node *schema, const char *name, int flags, bool href)
{
    return (sch_ns *) _sch_lookup_ns (instance, (xmlNode *)schema, name, flags, href);
}

static xmlNode *
lookup_node (sch_instance *instance, xmlNs *ns, xmlNode *node, const char *path)
{
    xmlNode *n;
    char *name, *mode;
    char *key = NULL;
    char *lk = NULL;
    char *colon;
    int len;

    if (!node)
    {
        return NULL;
    }

    if (path[0] == '/')
    {
        path++;
    }
    key = strchr (path, '/');
    if (key)
    {
        len = key - path;
        key = g_strndup (path, len);
        path += len;
    }
    else
    {
        key = g_strdup (path);
        path = NULL;
    }

    colon = strchr (key, ':');
    if (colon)
    {
        colon[0] = '\0';
        xmlNs *nns = _sch_lookup_ns (instance, node, key, 0/*flags*/, false);
        if (!nns)
        {
            /* No namespace found assume the node is supposed to have a colon in it */
            colon[0] = ':';
        }
        else
        {
            /* We found a namespace. Remove the prefix */
            char *_key = key;
            key = g_strdup (colon + 1);
            free (_key);
            ns = nns;
        }
    }

    for (n = node->children; n; n = n->next)
    {
        if (n->type != XML_ELEMENT_NODE)
        {
            continue;
        }
        name = (char *) xmlGetProp (n, (xmlChar *) "name");
        if (name && name[0] == '*')
        {
            lk = strchr (key, '=');
            if (lk)
                key[lk - key] = '\0';
        }
        if (name && (name[0] == '*' || sch_match_name (name, key)) && _sch_ns_match (n, ns))
        {
            free (key);
            if (path)
            {
                mode = (char *) xmlGetProp (n, (xmlChar *) "mode");
                if (mode && strchr (mode, 'p') != NULL)
                {
                    xmlFree (name);
                    xmlFree (mode);
                    /* restart search from root */
                    return lookup_node (instance, ns, xmlDocGetRootElement (node->doc), path);
                }
                xmlFree (name);
                if (mode)
                {
                    xmlFree (mode);
                }
                return lookup_node (instance, ns, n, path);
            }
            xmlFree (name);
            return n;
        }

        if (name)
        {
            xmlFree (name);
        }
    }

    free (key);
    return NULL;
}

sch_node *
sch_lookup (sch_instance * instance, const char *path)
{
    return lookup_node (instance, NULL, xmlDocGetRootElement (instance->doc), path);
}

sch_node *
sch_lookup_with_ns (sch_instance * instance, sch_ns *ns, const char *path)
{
    return lookup_node (instance, (xmlNs *) ns, xmlDocGetRootElement (instance->doc), path);
}

static sch_node *
_sch_node_child (xmlNs *ns, sch_node * parent, const char *child)
{
    xmlNode *xml = (xmlNode *) parent;
    xmlNode *n = xml->children;

    while (n)
    {
        if (n->type == XML_ELEMENT_NODE && n->name[0] == 'N')
        {
            char *name = (char *) xmlGetProp (n, (xmlChar *) "name");
            if (name && (name[0] == '*' || sch_match_name (name, child)) && _sch_ns_match (n, ns))
            {
                xmlFree (name);
                break;
            }
            xmlFree (name);
        }
        n = n->next;
    }
    return n;
}

sch_node *
sch_ns_node_child (sch_ns *ns, sch_node * parent, const char *child)
{
    return _sch_node_child ((xmlNs *)ns, parent, child);
}

static bool
_sch_node_find_name (xmlNs *ns, sch_node * parent, const char *path_name, GList **path_list)
{
    xmlNode *xml = (xmlNode *) parent;
    xmlNode *n = NULL;
    bool found = false;

    n = xml->children;
    while (n)
    {
        if (n->type == XML_ELEMENT_NODE && n->name[0] == 'N')
        {
            char *name = (char *) xmlGetProp (n, (xmlChar *) "name");
            if (sch_match_name (name, path_name) && _sch_ns_match (n, ns))
            {
                xmlFree (name);
                found = true;
                break;
            }
            if (n->children)
            {
                found = _sch_node_find_name (ns, n, path_name, path_list);
                if (found)
                {
                    *path_list = g_list_prepend (*path_list, g_strdup (name));
                    xmlFree (name);
                    break;
                }
            }
            xmlFree (name);
        }
        n = n->next;
    }
    return found;
}

static bool
sch_node_find_name (sch_instance *instance, xmlNs *ns, sch_node *parent, const char *path, int flags, GList **path_list)
{
    char *name;
    char *next;
    char *colon;
    bool found = false;

    if (path && path[0] == '/')
    {
        path++;

        /* Parse path element */
        next = strchr (path, '/');
        if (next)
            name = g_strndup (path, next - path);
        else
            name = g_strdup (path);
        colon = strchr (name, ':');
        if (colon)
        {
            colon[0] = '\0';
            xmlNs *nns = _sch_lookup_ns (instance, parent, name, flags, false);
            if (!nns)
            {
                /* No namespace found assume the node is supposed to have a colon in it */
                colon[0] = ':';
            }
            else
            {
                /* We found a namespace. Remove the prefix */
                char *_name = name;
                name = g_strdup (colon + 1);
                free (_name);
                ns = nns;
            }
        }
        found = _sch_node_find_name (ns, parent, name, path_list);
        g_free (name);
    }
    return found;
}

sch_node *
sch_child_first (sch_instance * instance)
{
    return instance ? sch_node_child_first (xmlDocGetRootElement (instance->doc)) : NULL;
}

sch_node *
sch_node_parent (sch_node *node)
{
    xmlNode *xml = (xmlNode *) node;
    if (!node)
        return NULL;
    if (xml->type == XML_ELEMENT_NODE && xml->name[0] == 'N')
        return (sch_node *) xml->parent;
    return NULL;
}

sch_node *
sch_node_child (sch_node * parent, const char *child)
{
    xmlNs *ns = parent ? ((xmlNode *) parent)->ns : NULL;
    return _sch_node_child (ns, parent, child);
}

sch_node *
sch_node_namespace_child (sch_node * parent, const char *namespace, const char *child)
{

    xmlNs *ns;
    xmlNode *module;
    sch_node *node;
    module = xmlNewNode (NULL, (xmlChar *) "MODULE");
    ns = xmlNewNs (module, (const xmlChar *) namespace, NULL);
    node =  _sch_node_child (ns, parent, child);

    /* Note the ns is freed as part of the xmlFreeNode */
    xmlFreeNode (module);
    return node;
}

sch_node *
sch_node_by_namespace (sch_instance * instance, const char *namespace, const char *prefix)
{
    xmlNode *xml = sch_child_first (instance);

    while (xml && xml->type == XML_ELEMENT_NODE)
    {
        if (xml->ns &&
            ((namespace && xml->ns->href && g_strcmp0 ((char *) xml->ns->href, namespace) == 0) ||
             (!namespace && prefix &&
              xml->ns->prefix && g_strcmp0 ((char *) xml->ns->prefix, prefix) == 0)))
            return xml;

        xml = xml->next;
    }
    return NULL;
}

sch_node *
sch_node_child_first (sch_node *parent)
{
    xmlNode *xml = (xmlNode *) parent;
    xmlNode *n = xml->children;

    while (n)
    {
        if (n->type == XML_ELEMENT_NODE && n->name[0] == 'N')
            break;
        n = n->next;
    }
    return n;
}

sch_node *
sch_node_next_sibling (sch_node *node)
{
    xmlNode *n = ((xmlNode *) node)->next;

    while (n)
    {
        if (n->type == XML_ELEMENT_NODE && n->name[0] == 'N')
            break;
        n = n->next;
    }
    return n;
}

/*
 * Returns the next node in a preorder traversal of the schema tree, or NULL if this would be the last in the traversal.
 * Only traverses nodes from root downwards. If root is null, will traverse all of tree
 */
sch_node *
sch_preorder_next(sch_node *current, sch_node *root) {
    /* Handle null */
    if (!current) {
        return NULL;
    }

    /* Check if this node has children */
    sch_node *next = sch_node_child_first(current);
    if (next) {
        return next;
    }

    /* Check if this node has siblings */
    next = sch_node_next_sibling(current);
    if (next) {
        return next;
    }

    /* Go up the tree, looking for siblings of ancestors */
    next = ((xmlNode *) current)->parent;
    while(next) {
        if (root && next == root) {
            break;
        }
        if (sch_node_next_sibling(next)) {
            next = sch_node_next_sibling(next);
            break;
        }
        next = ((xmlNode *) next)->parent;
    }

    /* At this stage either we have a next node or we have finished the tree */
    return next == root ? NULL : next;
}

char *
sch_name (sch_node * node)
{
    xmlNode *n = (xmlNode *) node;
    sch_instance *instance = n ? n->doc->_private : NULL;
    char *name = (char *) xmlGetProp (n, (xmlChar *) "name");
    if (!_sch_ns_native (instance, n->ns) && !sch_node_parent (sch_node_parent (node)))
    {
        char *_name = g_strdup_printf ("%s:%s", n->ns->prefix, name);
        free (name);
        name = _name;
    }
    return name;
}

/* Ignoring ancestors allows checking that this is a node with the model data directly attached. */
char *
sch_model (sch_node * node, bool ignore_ancestors)
{
    char *model = NULL;
    while (node)
    {
        model = (char *) xmlGetProp (node, (xmlChar *) "model");
        if (model || ignore_ancestors)
        {
            break;
        }
        node = ((xmlNode *) node)->parent;
    }
    return model;
}

char *
sch_organization (sch_node * node)
{
    char *organization = NULL;
    while (node)
    {
        organization = (char *) xmlGetProp (node, (xmlChar *) "organization");
        if (organization)
        {
            break;
        }
        node = ((xmlNode *) node)->parent;
    }
    return organization;
}

char *
sch_version (sch_node * node)
{
    char *version = NULL;
    while (node)
    {
        version = (char *) xmlGetProp (node, (xmlChar *) "version");
        if (version)
        {
            break;
        }
        node = ((xmlNode *) node)->parent;
    }
    return version;
}

char *
sch_namespace (sch_node * node)
{
    xmlNode *xml = ((xmlNode *) node);
    if (xml->ns && xml->ns->href)
    {
        return g_strdup ((char *) xml->ns->href);
    }
    return NULL;
}

char *
sch_prefix (sch_node * node)
{
    xmlNode *xml = ((xmlNode *) node);
    if (xml->ns && xml->ns->prefix)
    {
        return g_strdup ((char *) xml->ns->prefix);
    }
    return NULL;
}

char *
sch_default_value (sch_node * node)
{
    return (char *) xmlGetProp (node, (xmlChar *) "default");
}

char *
sch_path (sch_node * node)
{
    char *path = NULL;

    while (node)
    {
        char *tmp = NULL;
        char *name = (char *) xmlGetProp (node, (xmlChar *) "name");
        if (name == NULL)
        {
            break;
        }
        tmp = g_strdup_printf ("/%s%s", name, path ? : "");
        free (path);
        path = tmp;
        free (name);
        node = ((xmlNode *) node)->parent;
    }
    return path;
}

bool
sch_is_leaf (sch_node * node)
{
    xmlNode *xml = (xmlNode *) node;
    xmlNode *n;

    if (!xml->children && xmlHasProp (xml, (const xmlChar *)"mode"))
    {
        /* Defintely a leaf */
        return true;
    }
    for (n = xml->children; n; n = n->next)
    {
        if (n->type == XML_ELEMENT_NODE && n->name[0] == 'N')
        {
            /* Defintely not a leaf */
            return false;
        }
    }
    if (!xml->children && !xmlHasProp (xml, (const xmlChar *)"mode"))
    {
        /* Probably an empty container - so not a leaf */
        return false;
    }
    return true;
}

/* Elements can have different names (e.g. NODE, WATCH, REFRESH),
 * but only count the ones whose name is NODE
 */
static int
get_child_count (xmlNode *parent)
{
    xmlNode *iter;
    int count = 0;

    for (iter = parent->children; iter; iter = iter->next)
    {
        if (iter->name[0] == 'N')
        {
            count++;
        }
    }
    return count;
}

bool
sch_is_list (sch_node * node)
{
    xmlNode *xml = (xmlNode *) node;
    xmlNode *child = xml->children;

    if (child && get_child_count (xml) == 1 && child->type == XML_ELEMENT_NODE && child->name[0] == 'N')
    {
        char *name = (char *) xmlGetProp (child, (xmlChar *) "name");
        if (name && g_strcmp0 (name, "*") == 0)
        {
            xmlFree (name);
            return true;
        }
        if (name)
            xmlFree (name);
    }
    return false;
}

bool
sch_is_leaf_list (sch_node * node)
{
    xmlNode *xml = (xmlNode *) node;
    xmlNode *child = xml->children;

    if (!sch_is_list (node) || get_child_count (child) > 0)
    {
        return false;
    }

    return true;
}

char *
sch_list_key (sch_node * node)
{
    char *key = NULL;

    if (sch_is_list (node) && sch_node_child_first (sch_node_child_first (node)))
        key = sch_name (sch_node_child_first (sch_node_child_first (node)));
    return key;
}

bool
sch_is_readable (sch_node * node)
{
    xmlNode *xml = (xmlNode *) node;
    bool access = false;
    char *mode = (char *) xmlGetProp (xml, (xmlChar *) "mode");
    if (!mode || strchr (mode, 'r') != NULL || strchr (mode, 'p') != NULL)
    {
        access = true;
    }
    free (mode);
    return access;
}

bool
sch_is_writable (sch_node * node)
{
    xmlNode *xml = (xmlNode *) node;
    bool access = false;
    char *mode = (char *) xmlGetProp (xml, (xmlChar *) "mode");
    if (mode && strchr (mode, 'w') != NULL)
    {
        access = true;
    }
    free (mode);
    return access;
}

bool
sch_is_executable (sch_node * node)
{
    xmlNode *xml = (xmlNode *) node;
    bool access = false;
    char *mode = (char *) xmlGetProp (xml, (xmlChar *) "mode");
    if (mode && strchr (mode, 'x') != NULL)
    {
        access = true;
    }
    free (mode);
    return access;
}

bool
sch_is_hidden (sch_node * node)
{
    xmlNode *xml = (xmlNode *) node;
    bool access = false;
    char *mode = (char *) xmlGetProp (xml, (xmlChar *) "mode");
    if (mode && strchr (mode, 'h') != NULL)
    {
        access = true;
    }
    free (mode);
    return access;
}

bool
sch_is_config (sch_node * node)
{
    xmlNode *xml = (xmlNode *) node;
    bool access = false;
    char *mode = (char *) xmlGetProp (xml, (xmlChar *) "mode");
    if (mode && strchr (mode, 'c') != NULL)
    {
        access = true;
    }
    free (mode);
    return access;
}

bool
sch_is_proxy (sch_node * node)
{
    xmlNode *xml = (xmlNode *) node;
    bool access = false;
    char *mode = (char *) xmlGetProp (xml, (xmlChar *) "mode");
    if (mode && strchr (mode, 'p') != NULL)
    {
        access = true;
    }
    free (mode);
    return access;
}

bool
sch_is_read_only_proxy (sch_node * node)
{
    xmlNode *xml = (xmlNode *) node;
    bool read_only = false;
    char *mode = (char *) xmlGetProp (xml, (xmlChar *) "mode");
    if (mode && strchr (mode, 'p') != NULL && strchr (mode, 'r') != NULL)
    {
        read_only = true;
    }
    free (mode);
    return read_only;
}

char *
sch_translate_to (sch_node * node, char *value)
{
    xmlNode *xml = (xmlNode *) node;
    xmlNode *n;
    char *val;

    /* Get the default if needed - untranslated */
    if (!value)
    {
        value = (char *) xmlGetProp (node, (xmlChar *) "default");
    }

    /* Find the VALUE node with this value */
    for (n = xml->children; n && value; n = n->next)
    {
        if (n->type == XML_ELEMENT_NODE && n->name[0] == 'V')
        {
            val = (char *) xmlGetProp (n, (xmlChar *) "value");
            if (val && strcmp (value, val) == 0)
            {
                free (value);
                free (val);
                return (char *) xmlGetProp (n, (xmlChar *) "name");
            }
            free (val);
        }
    }
    return value;
}

char *
sch_translate_from (sch_node * node, char *value)
{
    xmlNode *xml = (xmlNode *) node;
    xmlNode *n;
    char *val;

    /* Find the VALUE node with this name */
    for (n = xml->children; n && value; n = n->next)
    {
        if (n->type == XML_ELEMENT_NODE && n->name[0] == 'V')
        {
            val = (char *) xmlGetProp (n, (xmlChar *) "name");
            if (val && strcmp (value, val) == 0)
            {
                free (value);
                free (val);
                return (char *) xmlGetProp (n, (xmlChar *) "value");
            }
            free (val);
        }
    }
    return value;
}

static bool
parse_integer (int flags, const char *value, bool *neg, uint64_t *vint)
{
    char *endptr = NULL;

    *neg = false;
    if (value[0] == '+' || value[0] == '-')
    {
        *neg = (value[0] == '-');
        value++;
    }
    *vint = strtoull (value, &endptr, 10);
    if ((*vint == ULLONG_MAX && errno == ERANGE) || !endptr || endptr[0] != '\0')
    {
        DEBUG (flags, "Failed to parse integer \"%s\"\n", value);
        return false;
    }
    return true;
}

static bool
parse_minmax (int flags, char *minmax, bool *min_neg, uint64_t *min, bool *max_neg, uint64_t *max)
{
    char *divider;
    bool rc = true;

    divider = strstr (minmax, "..");
    if (divider)
        divider[0] = '\0';
    if (!parse_integer (flags, minmax, min_neg, min))
    {
        rc = false;
    }
    if (rc && divider && !parse_integer (flags, divider + 2, max_neg, max))
    {
        rc = false;
    }
    if (divider)
        divider[0] = '.';
    else
    {
        *max_neg = *min_neg;
        *max = *min;
    }
    return rc;
}

bool
_sch_validate_pattern (sch_node * node, const char *value, int flags)
{
    xmlNode *xml = (xmlNode *) node;
    sch_instance *instance = xml ? xml->doc->_private : NULL;
    char message[100];
    int rc;

    if (!value)
        return false;
    /* Store compiled regex on the node */
    if (!xml->_private)
    {
        char *pattern = (char *) xmlGetProp (node, (xmlChar *) "pattern");
        if (pattern)
        {
            char message[100];
            regex_t *regex_obj = NULL;
            int rc;
            char *d_pattern = g_strdup_printf ("^%s$", pattern);

            regex_obj = g_malloc0 (sizeof (regex_t));
            rc = regcomp (regex_obj, d_pattern, REG_EXTENDED);
            if (rc != 0)
            {
                regerror (rc, NULL, message, sizeof (message));
                ERROR (flags, SCH_E_PATREGEX, "%i (\"%s\") for regex %s", rc, message, pattern);
                xmlFree (pattern);
                g_free (d_pattern);
                return false;
            }
            if (instance)
                instance->regexes = g_list_prepend (instance->regexes, regex_obj);
            xml->_private = (void *)regex_obj;
            g_free (d_pattern);
            xmlFree (pattern);
        }
    }
    if (xml->_private)
    {
        regex_t *regex_obj = (regex_t *)xml->_private;
        rc = regexec (regex_obj, value, 0, NULL, 0);
        if (rc == REG_ESPACE)
        {
            regerror (rc, NULL, message, sizeof (message));
            ERROR (flags, SCH_E_PATREGEX, "%i (\"%s\") for regex", rc, message);
            return false;
        }
        return (rc == 0);
    }
    char *range = (char *) xmlGetProp (node, (xmlChar *) "range");
    if (range)
    {
        bool vint_neg, min_neg, max_neg;
        uint64_t vint, min, max;

        if (!parse_integer (flags, value, &vint_neg, &vint))
        {
            ERROR (flags, SCH_E_OUTOFRANGE, "\"%s\" out of range \"%s\"", value, range);
            xmlFree (range);
            return false;
        }

        char *ptr = NULL;
        char *minmax = strtok_r (range, "|", &ptr);
        while (minmax)
        {
            if (!parse_minmax (flags, minmax, &min_neg, &min, &max_neg, &max))
            {
                ERROR (flags, SCH_E_INTERNAL, "Can't parse minmax \"%s\"", minmax);
                xmlFree (range);
                return false;
            }

            DEBUG (flags, "Checking %s%" PRIu64 " for range %s%" PRIu64 "..%s%" PRIu64 "\n",
                   vint_neg ? "-" : "", vint,
                   min_neg ? "-" : "", min,
                   max_neg ? "-" : "", max);

            bool ok = true;
            if (vint_neg && !min_neg) ok = false;
            if (vint_neg && min_neg && vint > min) ok = false;
            if (!vint_neg && !min_neg && vint < min) ok = false;
            if (!vint_neg && max_neg) ok = false;
            if (vint_neg && max_neg && vint < max) ok = false;
            if (!vint_neg && !max_neg && vint > max) ok = false;
            if (ok)
            {
                xmlFree (range);
                return true;
            }
            minmax = strtok_r (NULL, "|", &ptr);
        }
        ERROR (flags, SCH_E_OUTOFRANGE, "\"%s\" out of range \"%s\"", value, range);
        xmlFree (range);
        return false;
    }
    if (xml->children)
    {
        bool enumeration = false;
        bool rc = false;
        xmlNode *n;
        char *val;

        for (n = xml->children; n && value; n = n->next)
        {
            if (n->type == XML_ELEMENT_NODE && n->name[0] == 'V')
            {
                enumeration = true;
                val = (char *) xmlGetProp (n, (xmlChar *) "name");
                if (val && strcmp (value, val) == 0)
                {
                    free (val);
                    rc = true;
                    break;
                }
                free (val);
                val = (char *) xmlGetProp (n, (xmlChar *) "value");
                if (val && strcmp (value, val) == 0)
                {
                    free (val);
                    rc = true;
                    break;
                }
                free (val);
            }
        }
        if (enumeration && !rc)
        {
            ERROR (flags, SCH_E_ENUMINVALID, "\"%s\" not in enumeration\n", value);
            return false;
        }
    }
    return true;
}

bool
sch_validate_pattern (sch_node * node, const char *value)
{
    tl_error = SCH_E_SUCCESS;
    return _sch_validate_pattern (node, value, 0);
}

/* Data translation/manipulation */

static GList*
q2n_split_params (const char *params, char separator)
{
    GList *list = NULL;
    int depth = 0;
    GString *result = g_string_new (NULL);
    int i = 0;
    for (i = 0; i < strlen (params); i++)
    {
        char c = params[i];
        if (c == '(' || c == '[' || c == '{')
            depth += 1;
        else if (c == ')' || c == ']' || c == '}')
            depth -= 1;
        else if (depth == 0 && c == separator)
        {
            /* Save string if there's something in it, otherwise just keep using it. */
            if (result->len > 0)
            {
                list = g_list_append (list, g_string_free (result, false));
                result = g_string_new (NULL);
            }
            else
            {
                g_list_free_full (list, g_free);
                g_string_free (result, true);
                return NULL;
            }
            continue;
        }
        g_string_append_c (result, c);
    }
    if (result)
    {
        if (result->len > 0)
        {
            list = g_list_append (list, g_string_free (result, false));
        }
        else
        {
            g_list_free_full (list, g_free);
            g_string_free (result, true);
            return NULL;
        }
    }
    return list;
}

static bool
_check_tail (const gchar *tail)
{
    size_t len = strlen (tail);

    if (len < 2)
    {
        return false;
    }
    if (tail[0] != '/')
    {
        return false;
    }
    return true;
}

static bool
add_all_query_nodes (sch_node *schema, GNode *parent, bool config, bool state, int flags, int depth, int max)
{
    GNode *node = parent;
    char *name;

    if (depth >= max)
        return true;

    name = sch_name (schema);
    if (sch_is_leaf (schema))
    {
        if ((config && sch_is_writable (schema)) ||
            (state && !sch_is_writable (schema) && sch_is_readable (schema)))
        {
            APTERYX_NODE (parent, name);
            DEBUG (flags, "%*s%s\n", depth * 2, " ", name);
            name = NULL;
        }
    }
    else
    {
        node = APTERYX_NODE (parent, name);
        DEBUG (flags, "%*s%s\n", depth * 2, " ", name);
        name = NULL;

        /* Star nodes do not count when counting depth */
        if (!sch_is_list (sch_node_parent (schema)))
            depth++;
        for (sch_node *s = sch_node_child_first (schema); s && depth < max; s = sch_node_next_sibling (s))
        {
            if (!add_all_query_nodes (s, node, config, state, flags, depth, max))
                return false;
        }
    }

    free (name);
    return true;
}

/**
 * Split a node by ':' character into module and name. Only one ':' allowed, if line is split both
 * parts must be >0 characters. Return pointers point to original string, ':' is replaced by '\0'.
 */
static void
_split_module_name (char *node, char **name, char **module)
{
    char *c_pt;
    int count_sep = 0;
    bool on_sep = false;
    char *first_sep = NULL;

    *module = *name = node;
    for (c_pt = node; *c_pt != '\0'; c_pt++)
    {
        if (*c_pt == ':')
        {
            count_sep++;
            if (first_sep == NULL)
            {
                first_sep = c_pt;
                *first_sep = '\0';
                on_sep = true;
            }
            else
            {
                break;
            }
        }
        else if (on_sep)
        {
            *name = c_pt;
            on_sep = false;
        }
    }

    /* Clean up. */
    if (count_sep == 0)
    {
        *module = NULL;
    }
    else if (count_sep > 1 || on_sep || strlen (*module) == 0 || strlen (*name) == 0)
    {
        *module = NULL;
        *name = node;
        if (first_sep != NULL)
        {
            *first_sep = ':';
        }
    }
}

static bool
_check_model (char *module, sch_node *schema)
{
    bool ret = true;
    char *model;

    if (module == NULL || strlen (module) == 0)
    {
        return ret;
    }
    model = sch_model (schema, false);
    if (model != NULL)
    {
        ret = (g_strcmp0 (module, model) == 0);
    }
    g_free (model);
    return ret;
}

static GNode*
q2n_append_path (sch_node * schema, GNode *root, const char *path, int flags, int depth, sch_node **rschema, bool expand_non_leaf, bool config, bool nonconfig)
{
    char **nodes = g_strsplit (path, "/", -1);
    char **node = nodes;
    sch_node *index;
    sch_node *child;
    GNode *existing;

    while (*node)
    {
        char *name;
        char *module;

        _split_module_name (*node, &name, &module);

        /* Find schema node - since it might be an index node, call it such. */
        index = sch_node_child (schema, name);
        if (index == NULL)
        {
            ERROR (flags, SCH_E_NOSCHEMANODE, "No schema match for %s\n", name);
            g_strfreev (nodes);
            return NULL;
        }
        if (!sch_is_readable (index))
        {
            ERROR (flags, SCH_E_NOTREADABLE, "Ignoring non-readable node %s\n", name);
            g_strfreev (nodes);
            return NULL;
        }

        /* Because we allow for implicit wild-card index, need to check if this node is actually
         * for a child of a wildcard node.
         */
        if (sch_is_list (schema) && (child = sch_node_child (index, name)))
        {
            if (!sch_is_readable (child))
            {
                ERROR (flags, SCH_E_NOTREADABLE, "Ignoring non-readable node %s\n", name);
                g_strfreev (nodes);
                return NULL;
            }
            existing = apteryx_find_child (root, "*");
            if (!existing)
            {
                existing = APTERYX_NODE (root, g_strdup ("*"));
            }
            root = existing;
            schema = child;
        }
        else
        {
            schema = index;
        }

        /* Should be pointing at a true schema node, check that module matches if specified. */
        if (!_check_model (module, schema))
        {
            ERROR (flags, SCH_E_NOSCHEMANODE, "No model match for %s\n", module);
            g_strfreev (nodes);
            return NULL;
        }

        /* Create the node if it does not already exist */
        DEBUG (flags, "%*s%s\n", depth * 2, " ", name);
        existing = apteryx_find_child (root, name);
        if (existing)
            root = existing;
        else
            root = g_node_append_data (root, g_strdup (name));
        node++;
        depth++;
    }

    /* If schema is not a leaf, expand query with all subsequent children, if allowed. */
    if (!sch_is_leaf (schema) && expand_non_leaf)
    {
        for (sch_node *s = sch_node_child_first (schema); s; s = sch_node_next_sibling (s))
        {
            /* Recurse tree adding all elements */
            add_all_query_nodes (s, root, config, nonconfig, flags, depth + 1, INT_MAX);
        }
    }

    g_strfreev (nodes);
    if (rschema)
        *rschema = schema;
    return root;
}

static bool
_field_query_to_node (sch_node * schema, const char *fields, GNode *parent, int flags, int depth, const char *tail, bool config, bool nonconfig)
{
    GList *params = q2n_split_params (fields, ';');
    bool rc = true;
    gchar *left_side = NULL;
    gchar *middle = NULL;
    gchar *right_side = NULL;

    if (params == NULL)
    {
        return false;
    }
    for (GList *iter = g_list_first (params); rc && iter; iter = g_list_next (iter))
    {
        sch_node *nschema = schema;
        GNode *rroot = parent;
        fields = iter->data;
        if (strlen (fields) == 0) {
            rc = false;
            goto exit;
        }

        char *left = g_strstr_len (fields, -1, "(");
        char *right = g_strrstr_len (fields, -1, ")");
        if (right < left)
        {
            rc = false;
            goto exit;
        }
        if (left == NULL && right == NULL)
        {
            rroot = q2n_append_path (schema, rroot, fields, flags, depth, &nschema, tail == NULL, config, nonconfig);
            if (!rroot)
            {
                rc = false;
                goto exit;
            }
            if (rc && tail)
            {
                if (!_check_tail (tail))
                {
                    rc = false;
                    goto exit;
                }
                rroot = q2n_append_path (nschema, rroot, &tail[1], flags, depth, NULL, true, config, nonconfig);
                if (!rroot)
                {
                    rc = false;
                    goto exit;
                }
            }
            continue;
        }
        if (left == NULL || right == NULL)
        {
            rc = false;
            goto exit;
        }
        left_side = (left - fields) > 0 ? g_strndup (fields, left - fields) : NULL;
        middle = g_strndup (left + 1, right - left - 1);
        right_side = strlen (right + 1) > 0 ? g_strdup (right + 1) : NULL;
        if (left_side)
        {
            rroot = q2n_append_path (nschema, rroot, left_side, flags, depth, &nschema, middle == NULL, config, nonconfig);
            if (!rroot)
            {
                rc = false;
                goto exit1;
            }
        }
        if (rc && middle)
        {
            if (!_field_query_to_node (nschema, middle, rroot, flags, depth, right_side ?: tail, config, nonconfig))
            {
                rc = false;
                goto exit1;
            }
        }
        else if (rc && tail)
        {
            rroot = q2n_append_path (nschema, rroot, tail, flags, depth, NULL, true, config, nonconfig);
            if (!rroot)
            {
                rc = false;
                goto exit1;
            }
        }
        free (left_side);
        free (middle);
        free (right_side);
        left_side = NULL;
        middle = NULL;
        right_side = NULL;

    }
exit1:
    g_free (left_side);
    g_free (middle);
    g_free (right_side);
exit:
    g_list_free_full (params, g_free);
    return rc;
}

static bool
parse_query_fields (sch_node * schema, char *fields, GNode *parent, int flags, int depth, bool config, bool nonconfig)
{
    return _field_query_to_node (schema, fields, parent, flags, depth, NULL, config, nonconfig);
}

static bool
_sch_query_to_gnode (GNode *root, sch_node *schema, char *query, int *rflags, int depth, int *param_depth)
{
    int flags = rflags? * rflags : 0;
    GNode *node;
    char *ptr = NULL;
    char *parameter;
    bool content_seen = false;
    bool depth_seen = false;
    bool with_defaults_seen = false;
    bool config = true;
    bool nonconfig = true;
    char *qfields = NULL;
    int qdepth = INT_MAX;

    bool rc = false;

    /* Parse all queries out of uri first */
    query = g_strdup (query);
    parameter = strtok_r (query, "&", &ptr);
    while (parameter)
    {
        char *value = strchr (parameter, '=') + 1;
        if (strncmp (parameter, "fields=", strlen ("fields=")) == 0 && strlen (value) > 0)
        {
            if (qfields)
            {
                ERROR (flags, SCH_E_INVALIDQUERY, "Do not support multiple \"field\" queries\n");
                goto exit;
            }
            qfields = g_strdup (parameter + strlen ("fields="));
        }
        else if (strncmp (parameter, "content=", strlen ("content=")) == 0)
        {
            if (content_seen)
            {
                ERROR (flags, SCH_E_INVALIDQUERY, "Do not support multiple \"content\" queries\n");
                goto exit;
            }
            if (g_strcmp0 (value, "config") == 0)
                nonconfig = false;
            else if (g_strcmp0 (value, "nonconfig") == 0)
                config = false;
            else if (g_strcmp0 (value, "all") != 0)
            {
                ERROR (flags, SCH_E_INVALIDQUERY, "Do not support content query type \"%s\"\n", value);
                goto exit;
            }
            content_seen = true;
        }
        else if (strncmp (parameter, "depth=", strlen ("depth=")) == 0)
        {
            if (depth_seen)
            {
                ERROR (flags, SCH_E_INVALIDQUERY, "Do not support multiple \"depth\" queries\n");
                goto exit;
            }
            if (g_strcmp0 (value, "unbounded") != 0)
            {
                qdepth = g_ascii_strtoll (value, NULL, 10);
                if (qdepth <= 0 || qdepth > 65535)
                {
                    ERROR (flags, SCH_E_INVALIDQUERY, "Do not support depth query of \"%s\"\n", value);
                    goto exit;
                }
                if (qdepth == 1)
                    flags |= SCH_F_DEPTH_ONE;

                *param_depth = qdepth;
            }
            flags |= SCH_F_DEPTH;
            depth_seen = true;
        }
        else if (strncmp (parameter, "with-defaults=", strlen ("with-defaults=")) == 0)
        {
            if (with_defaults_seen)
            {
                ERROR (flags, SCH_E_INVALIDQUERY, "Do not support multiple \"with-defaults\" queries\n");
                goto exit;
            }
            if (g_strcmp0 (value, "report-all") == 0)
                flags |= SCH_F_ADD_DEFAULTS;
            else if (g_strcmp0 (value, "trim") == 0)
                flags |= SCH_F_TRIM_DEFAULTS;
            else if (g_strcmp0 (value, "explicit") != 0)
            {
                ERROR (flags, SCH_E_INVALIDQUERY, "Do not support with-defaults query type \"%s\"\n", value);
                goto exit;
            }
            with_defaults_seen = true;
        }
        else
        {
            ERROR (flags, SCH_E_INVALIDQUERY, "Do not support query \"%s\"\n", parameter);
            goto exit;
        }
        parameter = strtok_r (NULL, "&", &ptr);
    }

    /* May not have anything much */
    if (!qfields && config && nonconfig && qdepth == INT_MAX)
    {
        rc = true;
        goto exit;
    }

    /* Find end of the path node */
    node = root;
    while (node->children)
        node = node->children;

    /* Support fields || (depth AND/OR content) */
    if (qfields)
    {
        if (!parse_query_fields (schema, qfields, node, flags, depth + 1, config, nonconfig))
        {
            rc = false;
            goto exit;
        }
    }
    else
    {
        if (qdepth != INT_MAX)
            qdepth = depth;
        for (sch_node *s = sch_node_child_first (schema); s; s = sch_node_next_sibling (s))
        {
            /* Recurse tree only adding config elements */
            if (!add_all_query_nodes (s, node, config, nonconfig, flags, depth + 1, qdepth))
            {
                rc = false;
                goto exit;
            }
        }
    }

    rc = true;

exit:
    free (qfields);
    free (query);
    if (rc && rflags)
        *rflags = flags;
    return rc;
}

bool sch_query_to_gnode (sch_instance * instance, sch_node * schema, GNode *parent, const char * query, int flags,
                         int *rflags, int *param_depth)
{
    int _flags = flags;
    bool rc = _sch_query_to_gnode (parent, schema ?: xmlDocGetRootElement (instance->doc), (char *) query,
                                   &_flags, 0, param_depth);
    if (rflags)
        *rflags = _flags;
    return rc;
}

void
sch_check_condition (sch_node *node, GNode *root, int flags, char **path, char **condition)
{
    xmlChar *when_clause = xmlGetProp ((xmlNode *) node, BAD_CAST "when");
    xmlChar *must_clause = xmlGetProp ((xmlNode *) node, BAD_CAST "must");
    xmlChar *if_feature = xmlGetProp ((xmlNode *) node, BAD_CAST "if-feature");
    if (when_clause)
    {
        if (root)
            *path = apteryx_node_path (root);
        *condition = g_strdup ((char *) when_clause);
        DEBUG (flags, "when_clause <%s - %s>\n", *path, when_clause);
        xmlFree (when_clause);
    }

    if (must_clause)
    {
        if (root)
            *path = apteryx_node_path (root);
        *condition = g_strdup ((char *) must_clause);
        DEBUG (flags, "must_clause <%s - %s>\n", *path, must_clause);
        xmlFree (must_clause);
    }

    if (if_feature)
    {
        if (root)
            *path = apteryx_node_path (root);
        *condition = g_strdup_printf ("if-feature(%s)", (char *) if_feature);
        DEBUG (flags, "if_feature <%s - %s>\n", *path, if_feature);
        xmlFree (if_feature);
    }
}

static GNode *
_sch_path_to_gnode (sch_instance * instance, sch_node ** rschema, xmlNs *ns, const char *path, int flags, int depth)
{
    sch_node *schema = rschema && *rschema ? *rschema : xmlDocGetRootElement (instance->doc);
    xmlNs *nns = NULL;
    const char *next = NULL;
    GNode *node = NULL;
    GNode *rnode = NULL;
    GNode *child = NULL;
    char *query = NULL;
    char *pred = NULL;
    char *equals = NULL;
    char *new_path = NULL;
    char *colon;
    char *name = NULL;
    sch_node *last_good_schema = NULL;
    bool is_proxy = false;
    bool read_only = false;

    if (path && path[0] == '/')
    {
        path++;

        /* Parse path element */
        query = strchr (path, '?');
        next = strchr (path, '/');
        if (query && (!next || query < next))
            name = g_strndup (path, query - path);
        else if (next)
            name = g_strndup (path, next - path);
        else
            name = g_strdup (path);
        colon = strchr (name, ':');
        if (colon)
        {
            colon[0] = '\0';
            nns = _sch_lookup_ns (instance, schema, name, flags, false);
            if (!nns)
            {
                /* No namespace found assume the node is supposed to have a colon in it */
                colon[0] = ':';
            }
            else
            {
                /* We found a namespace. Remove the prefix */
                char *_name = name;
                name = g_strdup (colon + 1);
                free (_name);
                ns = nns;
            }
        }
        if (query && next && query < next)
            next = NULL;
        if (flags & SCH_F_XPATH)
        {
            pred = strchr (name, '[');
            if (pred)
            {
                char *temp = g_strndup (name, pred - name);
                pred = g_strdup (pred);
                free (name);
                name = temp;
            }
        }
        else
        {
            equals = strchr (name, '=');
            if (equals)
            {
                char *temp = g_strndup (name, equals - name);
                equals = g_strdup (equals + 1);
                free (name);
                name = temp;
            }
        }

        /* Find schema node */
        if (schema && sch_is_proxy (schema))
        {
            /* The schema containing the proxy node can have children */
            sch_node *child = sch_ns_node_child (ns, schema, name);
            if (!child)
            {
                is_proxy = sch_is_proxy (schema);
                read_only = sch_is_read_only_proxy (schema);
            }
        }

        if (!schema || is_proxy)
        {
            schema = xmlDocGetRootElement (instance->doc);
            /* Detect change in namespace with the new schema */
            colon = strchr (name, ':');
            if (colon)
            {
                colon[0] = '\0';
                nns = _sch_lookup_ns (instance, schema, name, flags, false);
                if (!nns)
                {
                    /* No namespace found assume the node is supposed to have a colon in it */
                    colon[0] = ':';
                }
                else
                {
                    /* We found a namespace. Remove the prefix */
                    char *_name = name;
                    name = g_strdup (colon + 1);
                    free (_name);
                    ns = nns;
                }
            }
        }

        last_good_schema = schema;
        schema = _sch_node_child (ns, schema, name);
        if ((flags & SCH_F_MODIFY_DATA) && schema && read_only)
        {
            ERROR (flags, SCH_E_NOTWRITABLE, "Node not writable \"%s\"\n", name);
            goto exit;
        }

        if ((flags & SCH_F_XPATH) && schema == NULL && g_strcmp0 (name, "*") == 0)
        {
            GList *path_list = NULL;
            bool found = sch_node_find_name (instance, ns, last_good_schema, next, flags, &path_list);
            if (found)
            {
                GList *list;
                int len = 0;
                bool first = true;
                for (list = g_list_first (path_list); list; list = g_list_next (list))
                {
                    len += strlen ((char *) list->data);
                }

                if (len)
                {
                    /* Note - the 64 bytes added to the length is to allow for extra slashes being added to the path */
                    len += strlen (path) +  64;
                    new_path = g_malloc0 (len);
                    len = 0;
                    /* Ammend the path with the new information. Note we drop the last list
                     * item as it contains a duplicate star slash already in the path */
                    for (list = g_list_first (path_list); list; list = g_list_next (list))
                    {
                        if (first)
                        {
                            g_free (name);
                            name = (char *) list->data;
                            first = false;
                        }
                        else
                        {
                            if (list->next)
                                len += sprintf (new_path + len, "/%s", (char *) list->data);
                            g_free (list->data);
                        }
                    }
                    sprintf (new_path + len, "/%s", path);
                    next = new_path;
                }
                g_list_free (path_list);
                if (new_path)
                    schema = _sch_node_child (ns, last_good_schema, name);
            }
        }

        /* Check RPC's are not bypassed */
        if (schema && next && next[0] != '\0' && sch_is_executable (schema))
        {
            DEBUG (flags, "Tried to access parameter node of RPC\n");
            schema = NULL;
        }

        if (schema == NULL)
        {
            ERROR (flags, SCH_E_NOSCHEMANODE, "No schema match for %s%s%s\n", ns ? (char *) ns->prefix : "",
                   ns ? ":" : "", name);
            goto exit;
        }

        /* Create node - include namespace node mapping if required */
        if (depth == 0 || is_proxy)
        {
            if (ns && ns->prefix && !_sch_ns_native (instance, ns))
            {
                rnode = APTERYX_NODE (NULL, g_strdup_printf ("%s%s:%s", depth == 0 ? "/" : "", ns->prefix, name));
            }
            else
            {
                rnode = APTERYX_NODE (NULL, g_strdup_printf ("%s%s", depth == 0 ? "/" : "", name));
            }
            DEBUG (flags, "%*s%s\n", depth * 2, " ", APTERYX_NAME (rnode));
        }
        else
        {
            rnode = APTERYX_NODE (NULL, name);
            name = NULL;
        }
        DEBUG (flags, "%*s%s\n", depth * 2, " ", APTERYX_NAME (rnode));

        /* XPATH predicates */
        if (pred && sch_is_list (schema))
        {
            char key[128 + 1];
            char value[128 + 1];

            schema = sch_node_child_first (schema);
            if (sscanf (pred, "[%128[^=]='%128[^']']", key, value) == 2) {
                // TODO make sure this key is the list key
                child = APTERYX_NODE (NULL, g_strdup (value));
                g_node_prepend (rnode, child);
                depth++;
                DEBUG (flags, "%*s%s\n", depth * 2, " ", APTERYX_NAME (child));
                if (next)
                {
                    if ((flags & SCH_F_XPATH) == 0 || !sch_is_proxy (schema) )
                        APTERYX_NODE (child, g_strdup (key));
                    depth++;
                    DEBUG (flags, "%*s%s\n", depth * 2, " ", APTERYX_NAME (child));
                }
            }
            g_free (pred);
        }
        else if (equals && sch_is_list (schema))
        {
            child = APTERYX_NODE (NULL, g_strdup (equals));
            g_node_prepend (rnode, child);
            depth++;
            DEBUG (flags, "%*s%s\n", depth * 2, " ", APTERYX_NAME (child));
            g_free (equals);
            schema = sch_node_child_first (schema);
        }

        if (next)
        {
            node = _sch_path_to_gnode (instance, &schema, ns, next, flags, depth + 1);
            if (!node)
            {
                free ((void *)rnode->data);
                g_node_destroy (rnode);
                rnode = NULL;
                goto exit;
            }
            g_node_prepend (child ? : rnode, node);
        }
    }

exit:
    if (rschema)
        *rschema = schema;
    free (name);
    g_free (new_path);
    return rnode;
}

GNode *
sch_path_to_gnode (sch_instance * instance, sch_node * schema, const char *path, int flags, sch_node ** rschema)
{
    GNode *node;
    char *_path = NULL;

    if ((flags & SCH_F_XPATH))
    {
        if (strstr (path, "//"))
        {
            char **split = g_strsplit (path, "//", -1);
            _path = g_strjoinv ("/*/", split);
            g_strfreev (split);
            path = _path;
        }
    }
    node = _sch_path_to_gnode (instance, rschema, NULL, path, flags, 0);
    g_free (_path);

    return node;
}

GNode *
sch_path_to_query (sch_instance * instance, sch_node * schema, const char *path, int flags)
{
    char *_path = NULL;
    char *query;
    GNode *root;
    int depth;
    int param_depth = 0;

    /* Split off any query first */
    query = strchr (path, '?');
    if (query)
    {
        path = _path = g_strndup (path, query - path);
        query++; /* Skip past the '?' */
    }

    /* Parse the path first */
    tl_error = SCH_E_SUCCESS;
    root = _sch_path_to_gnode (instance, &schema, NULL, path, flags, 0);
    if (!root || !schema)
    {
        free (_path);
        return NULL;
    }
    if (sch_is_leaf (schema) && !sch_is_readable (schema))
    {
        free (_path);
        return NULL;
    }

    /* Process the query */
    depth = g_node_max_height (root);
    if (query && !_sch_query_to_gnode (root, schema, query, &flags, depth, &param_depth))
    {
        apteryx_free_tree (root);
        root = NULL;
    }

    /* Without a query we may need to add a wildcard to get everything from here down */
    if (!query || (depth == g_node_max_height (root) && !(flags & SCH_F_DEPTH_ONE)))
    {
        if (schema && sch_node_child_first (schema) && !(flags & SCH_F_STRIP_DATA))
        {
            /* Find end of the path node */
            GNode *node = root;
            while (node->children)
                node = node->children;
            /* Get everything from here down if we do not already have a star */
            if (node && !g_node_first_child(node) && g_strcmp0 (APTERYX_NAME (node), "*") != 0)
            {
                APTERYX_NODE (node, g_strdup ("*"));
                DEBUG (flags, "%*s%s\n", g_node_max_height (root) * 2, " ", "*");
            }
        }
    }

    free (_path);
    return root;
}

static int
get_index (GNode * node, sch_node * schema)
{
    int index = 0;
    sch_node *n;
    for (n = sch_node_child_first (schema); n; index++, n = sch_node_next_sibling (n))
    {
        char *name = sch_name (n);
        if (g_strcmp0 (name, (char *) node->data) == 0)
        {
            free (name);
            break;
        }
        free (name);
    }
    return index;
}

static GNode *
merge (GNode * left, GNode * right, sch_node * schema)
{
    if (!left)
        return right;
    if (!right)
        return left;
    int ri = get_index (right, schema);
    int li = get_index (left, schema);
    if (ri > li)
    {
        left->next = merge (left->next, right, schema);
        left->next->prev = left;
        left->prev = NULL;
        return left;
    }
    else
    {
        right->next = merge (left, right->next, schema);
        right->next->prev = right;
        right->prev = NULL;
        return right;
    }
}

static GNode *
split (GNode * head)
{
    GNode *left, *right;
    left = right = head;
    while (right->next && right->next->next)
    {
        right = right->next->next;
        left = left->next;
    }
    right = left->next;
    left->next = NULL;
    return right;
}

static GNode *
merge_sort (GNode * head, sch_node * schema)
{
    GNode *left, *right;
    if (!head || !head->next)
        return head;
    left = head;
    right = split (left);
    left = merge_sort (left, schema);
    right = merge_sort (right, schema);
    return merge (left, right, schema);
}

void
sch_gnode_sort_children (sch_node * schema, GNode * parent)
{
    if (parent)
        parent->children = merge_sort (parent->children, schema);
}

static char *
decode_json_type (json_t *json)
{
    char *value;
    if (json_is_integer (json))
        value = g_strdup_printf ("%" JSON_INTEGER_FORMAT, json_integer_value (json));
    else if (json_is_boolean (json))
        value = g_strdup_printf ("%s", json_is_true (json) ? "true" : "false");
    else
        value = g_strdup (json_string_value (json));
    return value;
}

static bool
is_bool (xmlNodePtr parent)
{
    unsigned long value_count = 0;
    xmlNodePtr child;
    bool have_true = false;
    bool have_false = false;
    char *name;

    if (!parent || !parent->children)
        return false;

    child = parent->children;
    while (child != NULL) {
        /* Check for VALUE type nodes. Ignore nodes like WATCH or PROVIDE. */
        if (child->type == XML_ELEMENT_NODE && g_strcmp0 ((char *)child->name, "VALUE") == 0) {
            value_count++;
            if (value_count > 2)
                break;

            name = (char *) xmlGetProp (child, (xmlChar *) "name");
            if (g_strcmp0 (name, "true") == 0)
                have_true = true;
            else if (g_strcmp0 (name, "false") == 0)
                have_false = true;
            free (name);
        }
        child = child->next;
    }

    return value_count == 2 && have_true && have_false;
}

static json_t *
encode_json_type (sch_node *schema, char *val)
{
    json_t *json = NULL;
    json_int_t i;
    char *p;

    /* Only try and detect non string types if no pattern */
    if (*val != '\0' && !xmlHasProp ((xmlNode *)schema, (const xmlChar *)"pattern"))
    {
        /* Integers MUST(in XML) have a range property */
        if (xmlHasProp ((xmlNode *)schema, (const xmlChar *)"range"))
        {
            i = strtoll (val, &p, 10);
            if (*p == '\0')
                json = json_integer (i);
        }
        /* boolean MUST(in xml) be an enum of exactly two entities */
        if (!json && is_bool ((xmlNode *)schema))
        {
            if (g_strcmp0 (val, "true") == 0)
                json = json_true ();
            else if (g_strcmp0 (val, "false") == 0)
                json = json_false ();
        }
    }
    if (!json)
        json = json_string (val);
    return json;
}

static sch_node *
sch_traverse_get_schema (sch_instance * instance, GNode *node, int flags)
{
    sch_node *schema;
    char *colon;
    xmlNs *ns = NULL;
    char *name = APTERYX_NAME (node);
    if (name[0] == '/')
        name = name + 1;

    /* Check for a change in namespace */
    schema = xmlDocGetRootElement (instance->doc);
    colon = strchr (name, ':');
    if (colon)
    {
        char *namespace = g_strndup (name, colon - name);
        xmlNs *nns = _sch_lookup_ns (instance, schema, namespace, flags, false);
        free (namespace);
        if (nns)
        {
             /* We found a namespace. Skip the prefix */
            name = colon + 1;
            ns = nns;
        }
    }

    /* Find schema node */
    schema = _sch_node_child (ns, schema, name);
    if (schema == NULL)
    {
        ERROR (flags, SCH_E_NOSCHEMANODE, "No schema match for node %s\n", name);
        return NULL;
    }
    return schema;
}


static bool
_sch_traverse_nodes (sch_instance * instance, sch_node * schema, GNode * parent, int flags, int depth, int rdepth)
{
    char *name = sch_name (schema);
    GNode *child = apteryx_find_child (parent, name);
    bool rc = true;


    if (sch_is_proxy (schema) && g_strcmp0 (name, "*") == 0)
    {
        xmlNs *nns = NULL;
        char *colon;

        /* move to the list index specifier */
        child = parent->children;
        if (!child)
        {
            rc = false;
            goto exit;
        }
        /* skip over the list index specifier */
        child = child->children;
        if (!child)
        {
            rc = false;
            goto exit;
        }
        g_free (name);
        name = g_strdup (APTERYX_NAME (child));
        schema = xmlDocGetRootElement (instance->doc);
        colon = strchr (name, ':');
        if (schema && colon)
        {
            colon[0] = '\0';
            nns = sch_lookup_ns (instance, schema, name, flags, false);
            if (!nns)
            {
                /* No namespace found assume the node is supposed to have a colon in it */
                colon[0] = ':';
            }
            else
            {
                /* We found a namespace. Remove the prefix */
                char *_name = name;
                name = g_strdup (colon + 1);
                free (_name);
            }
        }
        schema = _sch_node_child (nns, schema, name);
        if (schema)
        {
            g_free (name);
            name = sch_name (schema);
        }
        else
        {
            /* This can happen if a query is for a field of the proxy schema itself */
            rc = false;
            goto exit;
        }
        depth++;
    }

    if (sch_is_leaf (schema))
    {
        if ((!child && flags & SCH_F_ADD_MISSING_NULL))
        {
            if (!(flags & SCH_F_FILTER_RDEPTH) || (depth >= rdepth))
            {
                child = APTERYX_LEAF (parent, name, g_strdup (""));
                name = NULL;
            }
        }
        else if (child && flags & SCH_F_SET_NULL)
        {
            if (!(flags & SCH_F_FILTER_RDEPTH) || (depth >= rdepth))
            {
                if (sch_is_hidden (schema) ||
                   (flags & SCH_F_CONFIG && !sch_is_writable (schema)))
                {
                    DEBUG (flags, "Silently ignoring node \"%s\"\n", name);
                    free ((void *)child->children->data);
                    free ((void *)child->data);
                    g_node_destroy (child);
                }
                else if (!sch_is_writable (schema))
                {
                    ERROR (flags, SCH_E_NOTWRITABLE, "Node not writable \"%s\"\n", name);
                    rc = false;
                    goto exit;
                }
                else
                {
                    free (child->children->data);
                    child->children->data = g_strdup ("");
                }
            }
        }
        else if (flags & SCH_F_ADD_DEFAULTS)
        {
            if (!(flags & SCH_F_FILTER_RDEPTH) || (depth >= rdepth ||
                (depth == rdepth - 1 && child && g_strcmp0(name, APTERYX_NAME (child)) == 0)))
            {
                /* We do not need to do anything at all if this leaf does not have a default */
                char *value = sch_translate_from (schema, sch_default_value (schema));
                if (value)
                {
                    /* Add completely missing leaves */
                    if (!child)
                    {
                        child = APTERYX_LEAF (parent, name, value);
                        name = NULL;
                        value = NULL;
                    }
                    /* Add missing values */
                    else if (!APTERYX_HAS_VALUE (child))
                    {
                        APTERYX_NODE (child, value);
                        value = NULL;
                    }
                    /* Replace empty value */
                    else if (APTERYX_VALUE (child) == NULL || g_strcmp0 (APTERYX_VALUE (child), "") == 0)
                    {
                        free (child->children->data);
                        child->children->data = value;
                        value = NULL;
                    }
                    free (value);
                }
            }
        }
        else if (child && (flags & SCH_F_TRIM_DEFAULTS))
        {
            if (!(flags & SCH_F_FILTER_RDEPTH) || (depth >= rdepth))
            {
                char *value = sch_translate_from (schema, sch_default_value (schema));
                if (value)
                {
                    if (g_strcmp0 (APTERYX_VALUE (child), value) == 0)
                    {
                        free ((void *)child->children->data);
                        free ((void *)child->data);
                        g_node_destroy (child);
                        child = NULL;
                    }
                    free (value);
                }
            }
        }
    }
    else if (g_strcmp0 (name, "*") == 0)
    {
        for (GNode *child = parent->children; child; child = child->next)
        {
            for (sch_node *s = sch_node_child_first (schema); s; s = sch_node_next_sibling (s))
            {
                if (flags & SCH_F_FILTER_RDEPTH)
                {
                    rc = _sch_traverse_nodes (instance, s, child, flags, depth+1, rdepth);
                }
                else
                {
                    rc = _sch_traverse_nodes (instance, s, child, flags, 0, 0);
                }
                if (!rc)
                    goto exit;
            }
        }
    }
    else if (sch_is_leaf_list (schema))
    {
        if (flags & SCH_F_SET_NULL)
        {
            if (!(flags & SCH_F_FILTER_RDEPTH) || (depth >= rdepth))
            {
                for (GNode *child = parent->children->children; child; child = child->next)
                {
                    free (child->children->data);
                    child->children->data = g_strdup ("");
                }
            }
        }
    }
    else
    {
        if (!child && !sch_is_list (schema) && (flags & (SCH_F_ADD_DEFAULTS|SCH_F_TRIM_DEFAULTS|SCH_F_ADD_MISSING_NULL)))
        {
            if (!(flags & SCH_F_FILTER_RDEPTH) || (depth >= rdepth))
            {
                child = APTERYX_NODE (parent, name);
                name = NULL;
            }
        }
        if (child)
        {
            for (sch_node *s = sch_node_child_first (schema); s; s = sch_node_next_sibling (s))
            {
                if (flags & SCH_F_FILTER_RDEPTH)
                {
                    rc = _sch_traverse_nodes (instance, s, child, flags, depth+1, rdepth);
                }
                else
                {
                    rc = _sch_traverse_nodes (instance, s, child, flags, 0, 0);
                }
                if (!rc)
                    goto exit;
            }
        }
    }

    /* Prune empty branches (unless it's a presence container) */
    if (child && !child->children && !sch_is_leaf (schema))
    {
        xmlNode *children = ((xmlNode *)schema)->children;
        if ((!children && (flags & SCH_F_ADD_DEFAULTS)) ||
            children || (flags & SCH_F_TRIM_DEFAULTS))
        {
            DEBUG (flags, "Throwing away node \"%s\"\n", APTERYX_NAME (child));
            free ((void *)child->data);
            g_node_destroy (child);
        }
    }

exit:
    free (name);
    return rc;
}

bool
sch_traverse_tree (sch_instance * instance, sch_node * schema, GNode * node, int flags, int rdepth)
{
    bool rc = false;
    if (flags & SCH_F_FILTER_RDEPTH)
    {
        schema = sch_traverse_get_schema (instance, node, flags);
        if (schema)
        {
            for (sch_node *s = sch_node_child_first (schema); s; s = sch_node_next_sibling (s))
            {
                rc = _sch_traverse_nodes (instance, s, node, flags, 1, rdepth);
                if (!rc)
                    break;
            }
        }
    }
    else
    {
        schema = schema ?: xmlDocGetRootElement (instance->doc);
        /* if this has been called from restconf, then the schema may be at a proxy node */
        if (sch_is_proxy (schema))
        {
            xmlNs *nns = NULL;
            char *colon;
            char *name;

            /* move to the list index specifier */
            node = node->children;
            if (!node)
                return rc;
            name = g_strdup (APTERYX_NAME (node));
            schema = xmlDocGetRootElement (instance->doc);
            colon = strchr (name, ':');
            if (schema && colon)
            {
                colon[0] = '\0';
                nns = sch_lookup_ns (instance, schema, name, flags, false);
                if (!nns)
                {
                    /* No namespace found assume the node is supposed to have a colon in it */
                    colon[0] = ':';
                }
                else
                {
                    /* We found a namespace. Remove the prefix */
                    char *_name = name;
                    name = g_strdup (colon + 1);
                    free (_name);
                }
            }
            schema = _sch_node_child (nns, schema, name);
            g_free (name);
            if (!schema)
                return rc;
        }

        if (sch_is_leaf (schema))
        {
            rc = _sch_traverse_nodes (instance, schema, node->parent, flags, 0, 0);
        }
        else
        {
            for (sch_node *s = sch_node_child_first (schema); s; s = sch_node_next_sibling (s))
            {
                rc = _sch_traverse_nodes (instance, s, node, flags, 0, 0);
                if (!rc)
                    break;
            }
        }
    }
    return rc;
}

static int _sch_strcmp_ll (const char *stra, const char *strb)
{
    int a = g_ascii_strtoll (stra, NULL, 10);
    int b = g_ascii_strtoll (strb, NULL, 10);
    return a - b;
}

static json_t *
_sch_gnode_to_json (sch_instance * instance, sch_node * schema, xmlNs *ns, GNode * node, int flags, int depth)
{
    json_t *data = NULL;
    char *colon;
    char *name;
    char *condition = NULL;
    char *path = NULL;

    /* Get the actual node name */
    if (depth == 0 && strlen (APTERYX_NAME (node)) == 1)
    {
        return _sch_gnode_to_json (instance, schema, ns, node->children, flags, depth);
    }
    else if (depth == 0 && APTERYX_NAME (node)[0] == '/')
    {
        name = g_strdup (APTERYX_NAME (node) + 1);
    }
    else
    {
        name = g_strdup (APTERYX_NAME (node));
    }

    colon = strchr (name, ':');
    if (colon)
    {
        colon[0] = '\0';
        xmlNs *nns = _sch_lookup_ns (instance, schema, name, flags, false);
        if (!nns)
        {
            /* No namespace found assume the node is supposed to have a colon in it */
            colon[0] = ':';
        }
        else
        {
            /* We found a namespace. Remove the prefix */
            char *_name = name;
            name = g_strdup (colon + 1);
            free (_name);
            ns = nns;
        }
    }

    /* Find schema node */
    if (sch_is_proxy (schema))
    {
        /* Two possible cases, the node is a child of the proxy node or we need to
         * move to access the remote database via the proxy */
        schema = _sch_node_child (ns, schema, name);
        if (!schema)
        {
            schema = xmlDocGetRootElement (instance->doc);
            colon = strchr (name, ':');
            if (schema && colon)
            {
                colon[0] = '\0';
                xmlNs *nns = sch_lookup_ns (instance, schema, name, flags, false);
                if (!nns)
                {
                    /* No namespace found assume the node is supposed to have a colon in it */
                    colon[0] = ':';
                }
                else
                {
                    /* We found a namespace. Remove the prefix */
                    char *_name = name;
                    name = g_strdup (colon + 1);
                    free (_name);
                    ns = nns;
                }
            }
            schema = _sch_node_child (ns, schema, name);
        }
    }
    else
    {
        if (!schema)
            schema = xmlDocGetRootElement (instance->doc);
        schema = _sch_node_child (ns, schema, name);
    }

    if (schema == NULL)
    {
        ERROR (flags, SCH_E_NOSCHEMANODE, "No schema match for gnode %s%s%s\n",
               ns ? (char *) ns->prefix : "", ns ? ":" : "", name);
        free (name);
        return NULL;
    }
    if (!sch_is_readable (schema))
    {
        ERROR (flags, SCH_E_NOTREADABLE, "Ignoring non-readable node %s%s%s\n",
               ns ? (char *) ns->prefix : "", ns ? ":" : "", name);
        free (name);
        return NULL;
    }

    if ((flags & SCH_F_CONDITIONS))
    {
        sch_check_condition (schema, node, flags, &path, &condition);
        if (condition)
        {
            if (!sch_process_condition (instance, node, path, condition))
            {
                g_free (condition);
                g_free (path);
                free (name);
                return NULL;
            }
            g_free (condition);
            g_free (path);
        }
    }

    if (sch_is_leaf_list (schema) && (flags & SCH_F_JSON_ARRAYS))
    {
        sch_node *kschema = sch_node_child_first (schema);
        if (kschema && xmlHasProp ((xmlNode *)kschema, (const xmlChar *)"range"))
            apteryx_sort_children (node, _sch_strcmp_ll);
        else
            apteryx_sort_children (node, g_strcmp0);
        data = json_array ();

        DEBUG (flags, "%*s%s[", depth * 2, " ", APTERYX_NAME (node));
        for (GNode * child = node->children; child; child = child->next)
        {
            if (child->children)
            {
                bool added = false;
                if (flags & SCH_F_JSON_TYPES)
                {
                    sch_node *cschema = sch_node_child_first (schema);
                    char *value = g_strdup (APTERYX_VALUE (child) ?: "");
                    value = sch_translate_to (cschema, value);
                    json_array_append_new (data, encode_json_type (cschema, value));
                    DEBUG (flags, "%s%s", value, child->next ? ", " : "");
                    free (value);
                    added = true;
                }
                if (!added)
                {
                    DEBUG (flags, "%s%s", APTERYX_VALUE (child), child->next ? ", " : "");
                    json_array_append_new (data, json_string ((const char* ) APTERYX_VALUE (child)));
                }
            }
        }
        DEBUG (flags, "]\n");
    }
    else if (sch_is_list (schema) && (flags & SCH_F_JSON_ARRAYS))
    {
        sch_node *kschema = sch_node_child_first (sch_node_child_first(schema));
        if (kschema && xmlHasProp ((xmlNode *)kschema, (const xmlChar *)"range"))
            apteryx_sort_children (node, _sch_strcmp_ll);
        else
            apteryx_sort_children (node, g_strcmp0);
        data = json_array ();
        for (GNode * child = node->children; child; child = child->next)
        {
            DEBUG (flags, "%*s%s[%s]\n", depth * 2, " ", APTERYX_NAME (node),
                   APTERYX_NAME (child));
            json_t *obj = json_object();
            sch_gnode_sort_children (sch_node_child_first (schema), child);
            for (GNode * field = child->children; field; field = field->next)
            {
                json_t *node = _sch_gnode_to_json (instance, sch_node_child_first (schema), ns, field, flags, depth + 1);
                bool added = false;
                if (flags & SCH_F_NS_PREFIX)
                {
                    sch_node *cschema = _sch_node_child (((xmlNode *) schema)->ns, sch_node_child_first (schema), APTERYX_NAME (field));
                    if (cschema && ((xmlNode *) cschema)->ns != ((xmlNode *) schema)->ns)
                    {
                        char * model = sch_model (cschema, false);
                        if (model)
                        {
                            char *pname = g_strdup_printf ("%s:%s", model, APTERYX_NAME (field));
                            json_object_set_new (obj, pname, node);
                            free (pname);
                            free (model);
                            added = true;
                        }
                    }
                }
                if (!added)
                    json_object_set_new (obj, APTERYX_NAME (field), node);
            }
            json_array_append_new (data, obj);
        }
    }
    else if (!sch_is_leaf (schema))
    {
        DEBUG (flags, "%*s%s\n", depth * 2, " ", APTERYX_NAME (node));
        data = json_object();
        sch_gnode_sort_children (schema, node);
        for (GNode * child = node->children; child; child = child->next)
        {
            if (!child->data && (flags & SCH_F_DEPTH))
                continue;

            json_t *node = _sch_gnode_to_json (instance, schema, ns, child, flags, depth + 1);
            bool added = false;
            if (flags & SCH_F_NS_PREFIX)
            {
                sch_node *cschema = _sch_node_child (ns, schema, APTERYX_NAME (child));
                if (cschema && ((xmlNode *) cschema)->ns != ((xmlNode *) schema)->ns)
                {
                    char * model = sch_model (cschema, false);
                    if (model)
                    {
                        char *pname = g_strdup_printf ("%s:%s", model, APTERYX_NAME (child));
                        json_object_set_new (data, pname, node);
                        free (pname);
                        free (model);
                        added = true;
                    }
                }
            }
            if (!added)
                json_object_set_new (data, APTERYX_NAME (child), node);
        }
        /* Throw away this node if no chldren (unless it's a presence container) */
        if ((flags & SCH_F_DEPTH) == 0 && json_object_iter (data) == NULL &&
            ((xmlNode *)schema)->children)
        {
            json_decref (data);
            data = NULL;
        }
    }
    else if (APTERYX_HAS_VALUE (node))
    {
        char *value = g_strdup (APTERYX_VALUE (node) ? APTERYX_VALUE (node) : "");

        if (flags & SCH_F_JSON_TYPES)
        {
            value = sch_translate_to (schema, value);
        }

        if (value && (flags & SCH_F_IDREF_VALUES))
        {
            /* Check to see if the schema has any identityref information */
            xmlChar *idref_module = xmlGetProp ((xmlNode *)schema, (const xmlChar *)"idref_module");
            if (idref_module)
            {
                char *temp = value;
                value = g_strdup_printf ("%s:%s", (char *) idref_module, value);
                g_free (temp);
                xmlFree (idref_module);
            }
        }

        if (flags & SCH_F_JSON_TYPES)
        {
            data = encode_json_type (schema, value);
        }
        else
            data = json_string (value);
        DEBUG (flags, "%*s%s = %s\n", depth * 2, " ", APTERYX_NAME (node), value);
        free (value);
    }

    free (name);
    return data;
}

static gchar *
_model_name (xmlNs *ns, char *model, char *name)
{
    gchar *ret;
    gchar **n_split = g_strsplit (name, ":", 2);

    /* If first part of name is actually the namespace prefix, we don't want to see it */
    if (g_strv_length (n_split) == 2 && g_strcmp0 (n_split[0], (gchar *) ns->prefix) == 0)
    {
        ret = g_strdup_printf ("%s:%s", model, n_split[1]);
    }
    else
    {
        ret = g_strdup_printf ("%s:%s", model, name);
    }
    g_strfreev (n_split);
    return ret;
}

json_t *
sch_gnode_to_json (sch_instance * instance, sch_node * schema, GNode * node, int flags)
{
    sch_node *pschema = schema ? ((xmlNode *)schema)->parent : xmlDocGetRootElement (instance->doc);
    xmlNs *ns = schema ? ((xmlNode *) schema)->ns : ((xmlNode *) pschema)->ns;
    json_t *json = NULL;
    json_t *child;

    tl_error = SCH_E_SUCCESS;
    child = _sch_gnode_to_json (instance, pschema, ns, node, flags, g_node_depth (node) - 1);
    if (child)
    {
        char *name;
        if (strlen (APTERYX_NAME (node)) == 1)
        {
            return child;
        }
        else if (APTERYX_NAME (node)[0] == '/')
        {
            name = APTERYX_NAME (node) + 1;
        }
        else
        {
            name = APTERYX_NAME (node);
        }
        if ((flags & SCH_F_NS_PREFIX) && schema)
        {
            char * model = sch_model (schema, false);
            if (model)
            {
                name = _model_name (ns, model, name);
                json = json_object ();
                json_object_set_new (json, name, child);
                free (name);
                free (model);
            }
        }
        if (!json)
        {
            json = json_object ();
            json_object_set_new (json, name, child);
        }
    }
    return json;
}

/* List keys should not have a '/' in them.
   Instead we escape it as %2F when storing in apteryx */
static char *
generate_list_key_from_value (const char *value)
{
    GString *key = g_string_new (NULL);
    while (*value)
    {
        if (*value == '/')
            g_string_append_printf (key, "%%%02X", *value);
        else
            g_string_append_c (key, *value);
        value++;
    }
    return g_string_free (key, false);
}

static GNode *
_sch_json_to_gnode (sch_instance * instance, sch_node * schema, xmlNs *ns,
                   json_t * json, const char *name, int flags, int depth)
{
    char *colon;
    json_t *child;
    json_t *kchild;
    const char *cname;
    size_t index;
    GNode *tree = NULL;
    GNode *node = NULL;
    char *key = NULL;
    char *value;

    /* Check for a change in namespace */
    colon = strchr (name, ':');
    if (colon)
    {
        char *namespace = g_strndup (name, colon - name);
        xmlNs *nns = _sch_lookup_ns (instance, schema, namespace, flags, false);
        free (namespace);
        if (nns)
        {
             /* We found a namespace. Skip the prefix */
            name = colon + 1;
            ns = nns;
        }
    }

    /* Find schema node */
    if (!schema)
        schema = xmlDocGetRootElement (instance->doc);
    schema = _sch_node_child (ns, schema, name);
    if (schema == NULL)
    {
        ERROR (flags, SCH_E_NOSCHEMANODE, "No schema match for json node %s\n", name);
        return NULL;
    }

    /* LEAF-LIST */
    if (sch_is_leaf_list (schema) && json_is_array (json))
    {
        depth++;
        tree = node = APTERYX_NODE (NULL, g_strdup (name));
        schema = sch_node_child_first (schema);
        json_array_foreach (json, index, child)
        {
            value = decode_json_type (child);
            if (value && value[0] != '\0' && flags & SCH_F_JSON_TYPES)
            {
                value = sch_translate_from (schema, value);
                if (!_sch_validate_pattern (schema, value, flags))
                {
                    DEBUG (flags, "Invalid value \"%s\" for node \"%s\"\n", value, name);
                    free (value);
                    apteryx_free_tree (tree);
                    return NULL;
                }
            }
            char *key = generate_list_key_from_value (value);
            APTERYX_LEAF (tree, key, value);
            DEBUG (flags, "%*s%s = %s\n", depth * 2, " ", key, value);
        }
    }
    /* LIST */
    else if (sch_is_list (schema) && json_is_array (json))
    {
        /* Get the key for this list */
        char *kname = NULL;
        key = sch_name (sch_node_child_first (sch_node_child_first (schema)));
        DEBUG (flags, "%*s%s%s\n", depth * 2, " ", depth ? "" : "/", name);
        depth++;
        tree = node = APTERYX_NODE (NULL, g_strdup (name));
        schema = sch_node_child_first (schema);
        json_array_foreach (json, index, child)
        {
            json_t *subchild;
            const char *subname;

            /* Get the key name for this json object and create a GNode with it */
            kchild = json_object_get (child, key);
            if (kchild)
            {
                kname = decode_json_type (kchild);
            }
            if (!kname)
            {
                ERROR (flags, SCH_E_KEYMISSING, "List \"%s\" missing key \"%s\"\n", name, key);
                apteryx_free_tree (tree);
                return NULL;
            }

            char *key = generate_list_key_from_value (kname);
            node = APTERYX_NODE (tree, key);
            free (kname);
            DEBUG (flags, "%*s%s\n", depth * 2, " ", APTERYX_NAME (node));

            /* Prepend each key-value pair of this object into the node */
            json_object_foreach (child, subname, subchild)
            {
                GNode *cn = _sch_json_to_gnode (instance, schema, ns, subchild, subname, flags, depth + 1);
                if (!cn)
                {
                    apteryx_free_tree (tree);
                    return NULL;
                }
                g_node_prepend (node, cn);
            }
        }
    }
    /* CONTAINER */
    else if (!sch_is_leaf (schema))
    {
        DEBUG (flags, "%*s%s%s\n", depth * 2, " ", depth ? "" : "/", name);
        tree = node = APTERYX_NODE (NULL, g_strdup_printf ("%s%s", depth ? "" : "/", name));
        json_object_foreach (json, cname, child)
        {
            GNode *cn = _sch_json_to_gnode (instance, schema, ns, child, cname, flags, depth + 1);
            if (!cn)
            {
                apteryx_free_tree (tree);
                return NULL;
            }
            g_node_append (node, cn);
        }
    }
    /* LEAF */
    else
    {
        if (!sch_is_writable (schema))
        {
            ERROR (flags, SCH_E_NOTWRITABLE, "Node \"%s\" not writable\n", name);
            return NULL;
        }

        tree = node = APTERYX_NODE (NULL, g_strdup (name));
        value = decode_json_type (json);
        if (value && value[0] != '\0' && flags & SCH_F_JSON_TYPES)
        {
            value = sch_translate_from (schema, value);
            if (!_sch_validate_pattern (schema, value, flags))
            {
                DEBUG (flags, "Invalid value \"%s\" for node \"%s\"\n", value, name);
                free (value);
                apteryx_free_tree (tree);
                return NULL;
            }
        }
        node = APTERYX_NODE (tree, value);
        DEBUG (flags, "%*s%s = %s\n", depth * 2, " ", name, APTERYX_NAME (node));
        return tree;
    }

    free (key);
    return tree;
}

static int
sch_node_height (sch_node * schema)
{
    xmlNode *xml = (xmlNode *) schema;
    int depth = 0;
    while (xml->parent)
    {
        xml = xml->parent;
        depth++;
    }
    return depth ? depth - 1 : 0;
}

GNode *
sch_json_to_gnode (sch_instance * instance, sch_node * schema, json_t * json, int flags)
{
    xmlNs *ns = schema ? ((xmlNode *) schema)->ns : NULL;
    const char *key;
    json_t *child;
    GNode *root;
    GNode *node;
    int depth = 0;

    tl_error = SCH_E_SUCCESS;
    root = g_node_new (g_strdup ("/"));
    json_object_foreach (json, key, child)
    {
        if (schema)
        {
            sch_node * child_schema = sch_node_child (schema, key);
            if (child_schema)
            {
                depth = sch_node_height (child_schema);
            }
            else
            {
                depth = sch_node_height (schema);
            }
        }
        node = _sch_json_to_gnode (instance, schema, ns, child, key, flags, depth);
        if (!node)
        {
            apteryx_free_tree (root);
            return NULL;
        }
        g_node_append (root, node);
    }
    return root;
}

static bool
_sch_apply_conditions (sch_instance * instance, sch_node * schema, GNode * parent, int flags)
{
    char *name = sch_name (schema);
    GNode *child = apteryx_find_child (parent, name);
    char *condition = NULL;
    char *path = NULL;
    bool rc = true;

    if (sch_is_proxy (schema) && g_strcmp0 (name, "*") == 0)
    {
        xmlNs *nns = NULL;
        char *colon;

        /* move to the list index specifier */
        child = parent->children;
        if (!child)
        {
            rc = false;
            goto exit;
        }
        /* skip over the list index specifier */
        child = child->children;
        if (!child)
        {
            rc = false;
            goto exit;
        }
        g_free (name);
        name = g_strdup (APTERYX_NAME (child));
        schema = xmlDocGetRootElement (instance->doc);
        colon = strchr (name, ':');
        if (schema && colon)
        {
            colon[0] = '\0';
            nns = sch_lookup_ns (instance, schema, name, flags, false);
            if (!nns)
            {
                /* No namespace found assume the node is supposed to have a colon in it */
                colon[0] = ':';
            }
            else
            {
                /* We found a namespace. Remove the prefix */
                char *_name = name;
                name = g_strdup (colon + 1);
                free (_name);
            }
        }
        schema = _sch_node_child (nns, schema, name);
        if (schema)
        {
            g_free (name);
            name = sch_name (schema);
        }
        else
        {
            /* This can happen if a query is for a field of the proxy schema itself */
            rc = false;
            goto exit;
        }
    }

    /* Check the YANG condition if we have a node with a child with data */
    if (child && child->children && ((char *) child->children->data)[0] != '\0')
    {
        sch_check_condition (schema, child, flags, &path, &condition);
        if (condition)
        {
            if (!sch_process_condition (instance, child, path, condition))
            {
                g_free (condition);
                g_free (path);
                rc = false;
                goto exit;
            }
            g_free (condition);
            g_free (path);
        }
    }

    if (!sch_is_leaf (schema))
    {
        if (g_strcmp0 (name, "*") == 0)
        {
            for (GNode *child = parent->children; child; child = child->next)
            {
                for (sch_node *s = sch_node_child_first (schema); s; s = sch_node_next_sibling (s))
                {
                    rc = _sch_apply_conditions (instance, s, child, flags);
                    if (!rc)
                        goto exit;
                }
            }
        }
        else if (!sch_is_leaf_list (schema) && child)
        {
            for (sch_node *s = sch_node_child_first (schema); s; s = sch_node_next_sibling (s))
            {
                rc = _sch_apply_conditions (instance, s, child, flags);
                if (!rc)
                    goto exit;
            }
        }
    }

exit:
    free (name);
    return rc;
}

bool
sch_apply_conditions (sch_instance * instance, sch_node * schema, GNode *node, int flags)
{
    bool rc = false;

    schema = sch_traverse_get_schema (instance, node, flags);
    if (schema)
    {
        for (sch_node *s = sch_node_child_first (schema); s; s = sch_node_next_sibling (s))
        {
            rc = _sch_apply_conditions (instance, s, node, flags);
            if (!rc)
                break;
        }
    }

    return rc;
}

static bool
_sch_trim_tree_by_depth (sch_instance *instance, sch_node *schema, GNode *parent, int flags, int depth, int rdepth)
{
    char *name = sch_name (schema);
    GNode *child = apteryx_find_child (parent, name);
    bool rc = true;

    if (sch_is_proxy (schema) && g_strcmp0 (name, "*") == 0)
    {
        xmlNs *nns = NULL;
        char *colon;

        /* move to the list index specifier */
        child = parent->children;
        if (!child)
        {
            rc = false;
            goto exit;
        }
        /* skip over the list index specifier */
        child = child->children;
        if (!child)
        {
            rc = false;
            goto exit;
        }
        g_free (name);
        name = g_strdup (APTERYX_NAME (child));
        schema = xmlDocGetRootElement (instance->doc);
        colon = strchr (name, ':');
        if (schema && colon)
        {
            colon[0] = '\0';
            nns = sch_lookup_ns (instance, schema, name, flags, false);
            if (!nns)
            {
                /* No namespace found assume the node is supposed to have a colon in it */
                colon[0] = ':';
            }
            else
            {
                /* We found a namespace. Remove the prefix */
                char *_name = name;
                name = g_strdup (colon + 1);
                free (_name);
            }
        }
        schema = _sch_node_child (nns, schema, name);
        if (schema)
        {
            g_free (name);
            name = sch_name (schema);
        }
        else
        {
            /* This can happen if a query is for a field of the proxy schema itself */
            rc = false;
            goto exit;
        }
        depth++;
    }

    if (sch_is_leaf_list (schema))
    {
        if (depth >= rdepth - 1)
        {
            GList *deletes = NULL;
            if (g_strcmp0 (name, APTERYX_NAME (parent->children)) == 0)
            {
                for (GNode *pcc = parent->children->children; pcc; pcc = pcc->next)
                    deletes = g_list_prepend (deletes, pcc);

                for (GList *iter = g_list_first (deletes); iter; iter = g_list_next (iter))
                {
                    GNode *pc = iter->data;
                    g_node_unlink (pc);
                    apteryx_free_tree (pc);
                }
                g_list_free (deletes);
            }
        }
    }
    else if (sch_is_leaf (schema))
    {
        if ((depth >= rdepth) && child)
        {
            free ((void *)child->children->data);
            free ((void *)child->data);
            g_node_unlink (child);
            g_node_destroy (child);
            child = NULL;
        }
    }
    else if (g_strcmp0 (name, "*") == 0)
    {
        if (depth < rdepth)
        {
            for (GNode *child = parent->children; child; child = child->next)
            {
                for (sch_node *s = sch_node_child_first (schema); s; s = sch_node_next_sibling (s))
                {
                    rc = _sch_trim_tree_by_depth (instance, s, child, flags, depth+1, rdepth);
                    if (!rc)
                        goto exit;
                }
            }
        }
    }
    else if (child && depth < rdepth)
    {
        for (sch_node *s = sch_node_child_first (schema); s; s = sch_node_next_sibling (s))
        {
            if (depth + 2 >= rdepth)
            {
                GList *deletes = NULL;
                for (GNode *cc = child->children; cc; cc = cc->next)
                    deletes = g_list_prepend (deletes, cc);

                for (GList *iter = g_list_first (deletes); iter; iter = g_list_next (iter))
                {
                    GNode *cc = iter->data;
                    g_node_unlink (cc);
                    apteryx_free_tree (cc);
                }
                g_list_free (deletes);
                break;
            }

            rc = _sch_trim_tree_by_depth (instance, s, child, flags, depth+1, rdepth);
            if (!rc)
                goto exit;
        }
    }

exit:
    free (name);
    return rc;
}

bool
sch_trim_tree_by_depth (sch_instance *instance, sch_node *schema, GNode *node, int flags, int rdepth)
{
    bool rc = false;

    schema = schema ?: xmlDocGetRootElement (instance->doc);
    /* if this has been called from restconf, then the schema may be at a proxy node */
    if (sch_is_proxy (schema))
    {
        xmlNs *nns = NULL;
        char *colon;
        char *name;

        /* move to the list index specifier */
        node = node->children;
        if (!node)
            return rc;
        name = g_strdup (APTERYX_NAME (node));
        schema = xmlDocGetRootElement (instance->doc);
        colon = strchr (name, ':');
        if (schema && colon)
        {
            colon[0] = '\0';
            nns = sch_lookup_ns (instance, schema, name, flags, false);
            if (!nns)
            {
                /* No namespace found assume the node is supposed to have a colon in it */
                colon[0] = ':';
            }
            else
            {
                /* We found a namespace. Remove the prefix */
                char *_name = name;
                name = g_strdup (colon + 1);
                free (_name);
            }
        }
        schema = _sch_node_child (nns, schema, name);
        g_free (name);
        if (!schema)
            return rc;
    }

    if (sch_is_leaf (schema))
    {
        rc = _sch_trim_tree_by_depth (instance, schema, node->parent, flags, 0, rdepth);
    }
    else
    {
        for (sch_node *s = sch_node_child_first (schema); s; s = sch_node_next_sibling (s))
        {
            rc = _sch_trim_tree_by_depth (instance, s, node, flags, 0, rdepth);
            if (!rc)
                break;
        }
    }

    return rc;
}
