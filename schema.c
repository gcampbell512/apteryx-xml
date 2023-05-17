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
        DEBUG (flags, fmt, ## args); \
    }

typedef struct _sch_instance
{
    xmlDoc *doc;
    GList *models_list;
    GHashTable *map_hash_table;
} sch_instance;

typedef struct _sch_xml_to_gnode_parms_s
{
    sch_instance *in_instance;
    int in_flags;
    char *in_def_op;
    bool in_is_edit;
    GNode *out_tree;
    char *out_error_tag;
    GList *out_deletes;
    GList *out_removes;
    GList *out_creates;
    GList *out_replaces;
} _sch_xml_to_gnode_parms;

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

/* List full paths for all schema files in the search path */
static void
list_schema_files (GList ** files, const char *path)
{
    DIR *dp;
    struct dirent *ep;
    char *saveptr = NULL;
    char *cpath;
    char *dpath;

    cpath = g_strdup (path);
    dpath = strtok_r (cpath, ":", &saveptr);
    while (dpath != NULL)
    {
        dp = opendir (dpath);
        if (dp != NULL)
        {
            while ((ep = readdir (dp)))
            {
                char *filename;
                if ((fnmatch ("*.xml", ep->d_name, FNM_PATHNAME) != 0) &&
                    (fnmatch ("*.xml.gz", ep->d_name, FNM_PATHNAME) != 0) &&
                    (fnmatch ("*.map", ep->d_name, FNM_PATHNAME) != 0))
                {
                    continue;
                }
                if (dpath[strlen (dpath) - 1] == '/')
                    filename = g_strdup_printf ("%s%s", dpath, ep->d_name);
                else
                    filename = g_strdup_printf ("%s/%s", dpath, ep->d_name);
                *files = g_list_append (*files, filename);
            }
            (void) closedir (dp);
        }
        dpath = strtok_r (NULL, ":", &saveptr);
    }
    free (cpath);
    *files = g_list_sort (*files, (GCompareFunc) strcasecmp);
    return;
}

static bool
sch_ns_node_equal (xmlNode * a, xmlNode * b)
{
    char *a_name = NULL;
    char *b_name = NULL;
    bool ret = FALSE;

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
        ret = TRUE;
    }

exit:
    free (a_name);
    free (b_name);
    return ret;
}

/* Merge nodes from a new tree to the original tree */
static void
merge_nodes (xmlNode * parent, xmlNode * orig, xmlNode * new, int depth)
{
    xmlNode *n;
    xmlNode *o;

    for (n = new; n; n = n->next)
    {
        /* Check if this node is already in the existing tree */
        for (o = orig; o; o = o->next)
        {
            if (sch_ns_node_equal (n, o))
            {
                /* May need to set the model info even if this is a match */
                xmlChar *mod_n = xmlGetProp (n, (xmlChar *)"model");
                if (mod_n)
                {
                    xmlChar *mod_o = xmlGetProp (o, (xmlChar *)"model");
                    if (!mod_o)
                    {
                        xmlChar *org = xmlGetProp (n, (xmlChar *)"organization");
                        xmlChar *ver = xmlGetProp (n, (xmlChar *)"version");
                        xmlNewProp (o, (const xmlChar *)"model", mod_n);
                        xmlNewProp (o, (const xmlChar *)"organization", org);
                        xmlNewProp (o, (const xmlChar *)"version", ver);
                        xmlFree (org);
                        xmlFree (ver);
                    }
                    else if (g_strcmp0 ((char *) mod_o, (char *) mod_n) != 0)
                    {
                        xmlChar *name = xmlGetProp (n, (xmlChar *)"name");
                        syslog (LOG_ERR, "XML: Conflicting model names in same namespace \"%s:%s\" \"%s:%s\"",
                            (char *) mod_o, (char *) name, (char *) mod_n, (char *) name);
                        xmlFree (name);
                    }
                    xmlFree (mod_n);
                    if (mod_o)
                        xmlFree (mod_o);
                }
                break;
            }
        }
        if (o)
        {
            /* Already exists - merge in the children */
            merge_nodes (o, o->children, n->children, depth + 1);
        }
        else
        {
            /* New node */
            o = xmlCopyNode (n, 1);

            /* Add as a child to the existing tree node */
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
add_module_info_to_children (xmlNode *node, xmlNsPtr ns, xmlChar *mod, xmlChar *org, xmlChar *ver)
{
    xmlNode *n = node;
    while (n)
    {
        if (n->ns && g_strcmp0 ((char *)n->ns->href, (char *)ns->href) == 0)
        {
            if (!xmlHasProp (n, (const xmlChar *)"model"))
            {
                xmlNewProp (n, (const xmlChar *)"model", mod);
                xmlNewProp (n, (const xmlChar *)"organization", org);
                xmlNewProp (n, (const xmlChar *)"version", ver);
            }
        }
        else
        {
            add_module_info_to_children (n->children, ns, mod, org, ver);
        }
        n = n->next;
    }
}

static void
add_module_info_to_child (sch_instance *instance, xmlNode *module)
{
    sch_loaded_model *loaded;
    xmlChar *mod = xmlGetProp (module, (xmlChar *)"model");
    xmlChar *org = xmlGetProp (module, (xmlChar *)"organization");
    xmlChar *ver = xmlGetProp (module, (xmlChar *)"version");
    xmlNsPtr def = xmlSearchNs (module->doc, module, NULL);

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
        instance->models_list = (void *) g_list_append (instance->models_list, loaded);
    }

    add_module_info_to_children (module->children, def, mod, org, ver);
    if (mod)
        xmlFree (mod);
    if (org)
        xmlFree (org);
    if (ver)
        xmlFree (ver);
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
            if (ns->prefix && ns->href && !xmlSearchNsByHref (doc, xmlDocGetRootElement (doc), ns->href))
            {
                xmlNewNs (xmlDocGetRootElement (doc), ns->href, ns->prefix);
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
    char buf[256];

    if (!instance)
        return;

    fp = fopen (filename, "r");
    if (fp)
    {
        instance->map_hash_table = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

        while (fgets (buf, sizeof (buf), fp) != NULL)
        {
            /* Skip comment lines */
            if (buf[0] == '#')
                continue;

            /* Remove any trailing LF */
            buf[strcspn(buf, "\n")] = '\0';

            ns_names = g_strsplit (buf, " ", 2);
            if (ns_names[0] && ns_names[1])
            {
                void *old_key;
                void *old_value;

                /* Look up this node name to check for duplicates. */
                if (g_hash_table_lookup_extended (instance->map_hash_table, ns_names[0],
                                                  &old_key, &old_value))
                {
                    g_hash_table_insert (instance->map_hash_table, g_strdup (ns_names[0]),
                                         g_strdup (ns_names[1]));
                    g_free (old_key);
                    g_free (old_value);
                }
                else
                {
                    g_hash_table_insert (instance->map_hash_table, g_strdup (ns_names[0]),
                                         g_strdup (ns_names[1]));
                }
            }
            g_strfreev (ns_names);
        }
        fclose (fp);
    }
}

/* Parse all XML files in the search path and merge trees */
sch_instance *
sch_load (const char *path)
{
    sch_instance *instance;
    xmlNode *module;
    GList *files = NULL;
    GList *iter;

    /* New instance */
    instance = g_malloc0 (sizeof (sch_instance));

    /* Create a new doc and root node for the merged MODULE */
    instance->doc = xmlNewDoc ((xmlChar *) "1.0");
    module = xmlNewNode (NULL, (xmlChar *) "MODULE");
    xmlNewNs (module, (const xmlChar *) "https://github.com/alliedtelesis/apteryx", NULL);
    xmlNewNs (module, (const xmlChar *) "http://www.w3.org/2001/XMLSchema-instance", (const xmlChar *) "xsi");
    xmlNewProp (module, (const xmlChar *) "xsi:schemaLocation",
        (const xmlChar *) "https://github.com/alliedtelesis/apteryx-xml https://github.com/alliedtelesis/apteryx-xml/releases/download/v1.2/apteryx.xsd");
    xmlDocSetRootElement (instance->doc, module);

    list_schema_files (&files, path);
    for (iter = files; iter; iter = g_list_next (iter))
    {
        char *filename = (char *) iter->data;
        char *ext = strrchr(filename, '.');
        if (g_strcmp0 (ext, ".map") == 0)
        {
            sch_load_namespace_mappings (instance, filename);
            continue;
        }
        xmlDoc *doc_new = xmlParseFile (filename);
        if (doc_new == NULL)
        {
            syslog (LOG_ERR, "XML: failed to parse \"%s\"", filename);
            continue;
        }
        xmlNode *module_new = xmlDocGetRootElement (doc_new);
        cleanup_nodes (module_new);
        copy_nsdef_to_root (instance->doc, module_new);
        add_module_info_to_child (instance, module_new);
        merge_nodes (module, module->children, module_new->children, 0);
        xmlFreeDoc (doc_new);
        assign_ns_to_root (instance->doc, module->children);
    }
    g_list_free_full (files, free);

    /* Store a link back to the instance in the xmlDoc stucture */
    instance->doc->_private = (void *) instance;

    return instance;
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
            g_free (loaded);
        }
        g_list_free (loaded_models);
        loaded_models = NULL;
    }
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
        g_free (instance);
    }
}

GList *
sch_get_loaded_models (sch_instance * instance)
{
    return instance->models_list;
}

static gboolean
match_name (const char *s1, const char *s2)
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
remove_hidden_children (xmlNode *node)
{
    char *mode;
    xmlNode *child;

    if (node == NULL)
        return false;

    /* Keep all the value nodes */
    if (node->name[0] == 'V')
        return true;

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

    /* If it doesn't have a mode, and there are no children, pretend it doesn't exist */
    mode = (char *)xmlGetProp (node, (xmlChar *) "mode");
    if (mode == NULL && node->children == NULL)
    {
        return false;
    }
    xmlFree(mode);

    return true;
}

char *
sch_dump_xml (sch_instance * instance)
{
    xmlNode *xml = xmlDocGetRootElement (instance->doc);
    xmlChar *xmlbuf = NULL;
    int bufsize;

    xmlDoc *copy = xmlCopyDoc (xml->doc, 1);
    remove_hidden_children (xmlDocGetRootElement (copy));
    xmlDocDumpFormatMemory (copy, &xmlbuf, &bufsize, 1);
    xmlFreeDoc (copy);
    return (char *) xmlbuf;
}

static bool
sch_ns_native (sch_instance *instance, xmlNs *ns)
{
    if (instance && instance->map_hash_table)
    {
        if (g_hash_table_lookup (instance->map_hash_table, (const char *) ns->href))
            return false;
    }
    return true;
}

static bool
sch_ns_match (xmlNode *node, xmlNs *ns)
{
    sch_instance *instance = node ? node->doc->_private : NULL;

    /* Check for native namespace match */
    if (node && !ns)
    {
        if (sch_ns_native (instance, node->ns))
            return TRUE;
        return FALSE;
    }

    /* Check for a namespace uri match */
    while (node && node->type == XML_ELEMENT_NODE)
    {
        if (node->ns && node->ns->href &&
            g_strcmp0 ((const char *) node->ns->href, (const char *) ns->href) == 0)
            return TRUE;
        node = node->parent;
    }

    /* No match */
    return FALSE;
}

static xmlNs *
sch_lookup_ns (sch_instance * instance, xmlNode *schema, const char *name, int flags, bool href)
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

static xmlNode *
lookup_node (xmlNs *ns, xmlNode *node, const char *path)
{
    xmlNode *n;
    char *name, *mode;
    char *key = NULL;
    char *lk = NULL;
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
        if (name && (name[0] == '*' || match_name (name, key)) && sch_ns_match (n, ns))
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
                    return lookup_node (ns, xmlDocGetRootElement (node->doc), path);
                }
                xmlFree (name);
                if (mode)
                {
                    xmlFree (mode);
                }
                return lookup_node (ns, n, path);
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
    return lookup_node (NULL, xmlDocGetRootElement (instance->doc), path);
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
            if (name && (name[0] == '*' || match_name (name, child)) && sch_ns_match (n, ns))
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
sch_child_first (sch_instance * instance)
{
    return instance ? sch_node_child_first (xmlDocGetRootElement (instance->doc)) : NULL;
}

sch_node *
sch_node_parent (sch_node *node)
{
    xmlNode *xml = (xmlNode *) node;
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
    return (char *) xmlGetProp (node, (xmlChar *) "name");
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

    if (!xml->children)
    {
        return true;
    }
    for (n = xml->children; n; n = n->next)
    {
        if (n->type == XML_ELEMENT_NODE && n->name[0] == 'N')
        {
            return false;
        }
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

bool
_sch_validate_pattern (sch_node * node, const char *value, int flags)
{
    xmlNode *xml = (xmlNode *) node;
    char *pattern = (char *) xmlGetProp (node, (xmlChar *) "pattern");
    if (pattern)
    {
        char message[100];
        regex_t regex_obj;
        int rc;

        rc = regcomp (&regex_obj, pattern, REG_EXTENDED);
        if (rc != 0)
        {
            regerror (rc, NULL, message, sizeof (message));
            ERROR (flags, SCH_E_PATREGEX, "%i (\"%s\") for regex %s", rc, message, pattern);
            xmlFree (pattern);
            return false;
        }

        rc = regexec (&regex_obj, value, 0, NULL, 0);
        regfree (&regex_obj);
        if (rc == REG_ESPACE)
        {
            regerror (rc, NULL, message, sizeof (message));
            ERROR (flags, SCH_E_PATREGEX, "%i (\"%s\") for regex %s", rc, message, pattern);
            xmlFree (pattern);
            return false;
        }

        xmlFree (pattern);
        return (rc == 0);
    }
    else if (xml->children)
    {
        bool enumeration = false;
        int rc = false;
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

static bool parse_query_fields (sch_node * schema, char *fields, GNode *parent, int flags, int depth);
static GNode *
parse_field (sch_node * schema, const char *path, int flags, int depth)
{
    GNode *rnode = NULL;
    GNode *child = NULL;
    const char *next = NULL;
    const char *sublist = NULL;
    char *name;

    /* Find name */
    sublist = strchr (path, '(');
    next = strchr (path, '/');
    if (sublist && (!next || sublist < next))
        name = g_strndup (path, sublist - path);
    else if (next)
        name = g_strndup (path, next - path);
    else
        name = g_strdup (path);

    /* Find schema node */
    schema = sch_node_child (schema, name);
    if (schema == NULL)
    {
        ERROR (flags, SCH_E_NOSCHEMANODE, "No schema match for %s\n", name);
        g_free (name);
        return NULL;
    }
    if (!sch_is_readable (schema))
    {
        ERROR (flags, SCH_E_NOTREADABLE, "Ignoring non-readable node %s\n", name);
        g_free (name);
        return NULL;
    }

    /* Create the node */
    rnode = APTERYX_NODE (NULL, name);
    DEBUG (flags, "%*s%s\n", depth * 2, " ", APTERYX_NAME (rnode));

    /* Process subpath */
    if (next)
    {
        child = parse_field (schema, next + 1, flags, depth + 1);
        if (!child)
        {
            free ((void *)rnode->data);
            g_node_destroy (rnode);
            return NULL;
        }
        g_node_prepend (rnode, child);
    }
    else if (sublist)
    {
        char *fields = g_strndup (sublist + 1, strlen (sublist) - 2);
        if (!parse_query_fields (schema, fields, rnode, flags, depth + 1))
        {
            free ((void *)rnode->data);
            g_node_destroy (rnode);
            free (fields);
            return false;
        }
        free (fields);
    }

    return rnode;
}

static void
merge_node_into_parent (GNode *parent, GNode *node)
{
    for (GNode *pchild = parent->children; pchild; pchild = pchild->next)
    {
        if (g_strcmp0 (pchild->data, node->data) == 0)
        {
            /* Unlink all the children and add to the original parent */
            GList *children = NULL;
            for (GNode *nchild = node->children; nchild; nchild = nchild->next)
            {
                children = g_list_append (children, nchild);
            }
            for (GList *nchild = children; nchild; nchild = nchild->next)
            {
                g_node_unlink (nchild->data);
                merge_node_into_parent (pchild, nchild->data);
            }
            g_list_free (children);
            node->children = NULL;
            free ((void *)node->data);
            g_node_destroy (node);
            return;
        }
    }
    g_node_prepend (parent, node);
}

static bool
parse_query_fields (sch_node * schema, char *fields, GNode *parent, int flags, int depth)
{
    char *h, *t;
    bool skip = false;

    h = t = fields;
    while (*h)
    {
        if (*(h + 1) == '(')
            skip = true;
        else if (*(h + 1) == '\0' || (!skip && *(h + 1) == ';'))
        {
            char *field = g_strndup (t, (h - t + 1));
            GNode *node = parse_field (schema, field, flags, depth);
            free (field);
            if (!node)
                return false;
            merge_node_into_parent (parent, node);
            t = h + 2;
        }
        else if (*(h + 1) == ')')
            skip = false;

        h++;
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

static bool
_sch_query_to_gnode (GNode *root, sch_node *schema, char *query, int *rflags, int depth)
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
            }
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
        /* If a list we need to wildcard the entry name before adding field query */
        if (sch_is_list (schema))
        {
            node = APTERYX_NODE (node, g_strdup ("*"));
            DEBUG (flags, "%*s%s\n", (depth + 1) * 2, " ", "*");
            depth++;
        }
        if (!parse_query_fields (schema, qfields, node, flags, depth + 1))
        {
            rc = false;
            goto exit;
        }
    }
    else
    {
        if (qdepth != INT_MAX)
            qdepth += depth;
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

bool sch_query_to_gnode (sch_instance * instance, sch_node * schema, GNode *parent, const char * query, int flags, int *rflags)
{
    int _flags = flags;
    bool rc = _sch_query_to_gnode (parent, schema ?: xmlDocGetRootElement (instance->doc), (char *) query, &_flags, 0);
    if (rflags)
        *rflags = _flags;
    return rc;
}

static GNode *
_sch_path_to_gnode (sch_instance * instance, sch_node ** rschema, xmlNs *ns, const char *path, int flags, int depth)
{
    sch_node *schema = rschema && *rschema ? *rschema : xmlDocGetRootElement (instance->doc);
    const char *next = NULL;
    GNode *node = NULL;
    GNode *rnode = NULL;
    GNode *child = NULL;
    char *query = NULL;
    char *pred = NULL;
    char *equals = NULL;
    char *colon;
    char *name = NULL;

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
            ns = sch_lookup_ns (instance, schema, name, flags, false);
            if (!ns)
            {
                ERROR (flags, SCH_E_NOSCHEMANODE, "No namespace found \"%s\"\n", name);
                goto exit;
            }
            char *_name = name;
            name = g_strdup (colon + 1);
            free (_name);
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
        if (!schema || sch_is_proxy (schema))
            schema = xmlDocGetRootElement (instance->doc);
        schema = _sch_node_child (ns, schema, name);
        if (schema == NULL)
        {
            ERROR (flags, SCH_E_NOSCHEMANODE, "No schema match for %s%s%s\n", ns ? (char *) ns->prefix : "",
                   ns ? ":" : "", name);
            goto exit;
        }

        /* Create node */
        if (depth == 0)
        {
            if (ns && ns->prefix && !sch_ns_native (instance, ns))
            {
                rnode = APTERYX_NODE (NULL, g_strdup_printf ("/%s:%s", ns->prefix, name));
            }
            else
            {
                rnode = APTERYX_NODE (NULL, g_strdup_printf ("/%s", name));
            }
        }
        else
        {
            rnode = APTERYX_NODE (NULL, name);
            name = NULL;
        }
        DEBUG (flags, "%*s%s\n", depth * 2, " ", APTERYX_NAME (rnode));

        /* XPATH predicates */
        if (pred && sch_is_list (schema)) {
            char key[128 + 1];
            char value[128 + 1];

            if (sscanf (pred, "[%128[^=]='%128[^']']", key, value) == 2) {
                // TODO make sure this key is the list key
                child = APTERYX_NODE (NULL, g_strdup (value));
                g_node_prepend (rnode, child);
                depth++;
                DEBUG (flags, "%*s%s\n", depth * 2, " ", APTERYX_NAME (child));
                if (next) {
                    APTERYX_NODE (child, g_strdup (key));
                    depth++;
                    DEBUG (flags, "%*s%s\n", depth * 2, " ", APTERYX_NAME (child));
                }
            }
            g_free (pred);
            schema = sch_node_child_first (schema);
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
    return rnode;
}

GNode *
sch_path_to_gnode (sch_instance * instance, sch_node * schema, const char *path, int flags, sch_node ** rschema)
{
    return _sch_path_to_gnode (instance, rschema, NULL, path, flags, 0);
}

GNode *
sch_path_to_query (sch_instance * instance, sch_node * schema, const char *path, int flags)
{
    char *_path = NULL;
    char *query;
    GNode *root;
    int depth;

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
    if (query && !_sch_query_to_gnode (root, schema, query, &flags, depth))
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

static void
gnode_sort_children (sch_node * schema, GNode * parent)
{
    if (parent)
        parent->children = merge_sort (parent->children, schema);
}

static xmlNode *
_sch_gnode_to_xml (sch_instance * instance, sch_node * schema, xmlNs *ns, xmlNode * parent,
                   GNode * node, int flags, int depth)
{
    xmlNode *data = NULL;
    char *colon = NULL;
    char *name;  

    /* Get the actual node name */
    if (depth == 0 && strlen (APTERYX_NAME (node)) == 1)
    {
        return _sch_gnode_to_xml (instance, schema, ns, parent, node->children, flags, depth);
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
        ns = sch_lookup_ns (instance, schema, name, flags, false);
        if (!ns)
        {
            ERROR (flags, SCH_E_NOSCHEMANODE, "No namespace found \"%s\"\n", name);
            free (name);
            return NULL;
        }
        char *_name = name;
        name = g_strdup (colon + 1);
        free (_name);
    }

    /* Find schema node */
    if (!schema)
        schema = xmlDocGetRootElement (instance->doc);
    schema = _sch_node_child (ns, schema, name);
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

    if (sch_is_list (schema))
    {
        apteryx_sort_children (node, g_strcmp0);
        for (GNode * child = node->children; child; child = child->next)
        {
            gboolean has_child = false;

            DEBUG (flags, "%*s%s[%s]\n", depth * 2, " ", APTERYX_NAME (node),
                   APTERYX_NAME (child));
            data = xmlNewNode (NULL, BAD_CAST name);
            gnode_sort_children (sch_node_child_first (schema), child);
            for (GNode * field = child->children; field; field = field->next)
            {
                if (_sch_gnode_to_xml (instance, sch_node_child_first (schema), ns,
                                       data, field, flags, depth + 1))
                {
                    has_child = true;
                }
            }
            if (has_child)
            {
                xmlAddChildList (parent, data);
            }
            else
            {
                xmlFreeNode (data);
                data = NULL;
            }
        }
    }
    else if (!sch_is_leaf (schema))
    {
        gboolean has_child = false;

        DEBUG (flags, "%*s%s\n", depth * 2, " ", APTERYX_NAME (node));
        data = xmlNewNode (NULL, BAD_CAST name);
        gnode_sort_children (schema, node);
        for (GNode * child = node->children; child; child = child->next)
        {
            if (_sch_gnode_to_xml (instance, schema, ns, data, child, flags, depth + 1))
            {
                has_child = true;
            }
        }
        if (has_child && parent)
        {
            xmlAddChild (parent, data);
        }
        else if (!has_child)
        {
            xmlFreeNode (data);
            data = NULL;
        }
    }
    else if (APTERYX_HAS_VALUE (node))
    {
        if (!(flags & SCH_F_CONFIG) || sch_is_writable (schema))
        {
            char *value = g_strdup (APTERYX_VALUE (node) ? APTERYX_VALUE (node) : "");
            data = xmlNewNode (NULL, BAD_CAST name);
            value = sch_translate_to (schema, value);
            xmlNodeSetContent (data, (const xmlChar *) value);
            if (parent)
                xmlAddChildList (parent, data);
            DEBUG (flags, "%*s%s = %s\n", depth * 2, " ", APTERYX_NAME (node), value);
            free (value);
        }
    }

    free (name);
    return data;
}

xmlNode *
sch_gnode_to_xml (sch_instance * instance, sch_node * schema, GNode * node, int flags)
{
    tl_error = SCH_E_SUCCESS;
    if (node && g_node_n_children (node) > 1 && strlen (APTERYX_NAME (node)) == 1)
    {
        xmlNode *first = NULL;
        xmlNode *last = NULL;
        xmlNode *next;

        apteryx_sort_children (node, g_strcmp0);
        for (GNode * child = node->children; child; child = child->next)
        {
            next = _sch_gnode_to_xml (instance, schema, NULL, NULL, child, flags, 1);
            if (next)
            {
                if (last)
                    xmlAddSibling (last, next);
                last = next;
                if (!first)
                    first = next;
            }
        }
        return first;
    }
    else
        return _sch_gnode_to_xml (instance, schema, NULL, NULL, node, flags, 0);
}

static bool
xml_node_has_content (xmlNode * xml)
{
    char *content = (char *) xmlNodeGetContent (xml);
    bool ret = (content && strlen (content) > 0);
    free (content);
    return ret;
}

/**
 * Check XML node for the operation attribute and extract it. Return whether the
 * operation is recognised or not.
 */
static bool
_operation_ok (_sch_xml_to_gnode_parms *_parms, xmlNode *xml, char *curr_op, char **new_op)
{
    char *attr;

    attr = (char *) xmlGetProp (xml, BAD_CAST "operation");
    if (attr != NULL)
    {
        if (!_parms->in_is_edit)
        {
            _parms->out_error_tag = "bad-attribute";
            return false;
        }

        /* Find new attribute. */
        if (g_strcmp0 (attr, "delete") == 0)
        {
            *new_op = "delete";
        }
        else if (g_strcmp0 (attr, "merge") == 0)
        {
            *new_op = "merge";
        }
        else if (g_strcmp0 (attr, "replace") == 0)
        {
            *new_op = "replace";
        }
        else if (g_strcmp0 (attr, "create") == 0)
        {
            *new_op = "create";
        }
        else if (g_strcmp0 (attr, "remove") == 0)
        {
            *new_op = "remove";
        }
        else
        {
            g_free (attr);
            _parms->out_error_tag = "bad-attribute";
            return false;
        }
        g_free (attr);

        /* Check for invalid transitions between sub-operations. We only allow
         * merge->anything transitions.
         */
        if (g_strcmp0 (curr_op, *new_op) != 0 && g_strcmp0 (curr_op, "merge") != 0)
        {
            _parms->out_error_tag = "operation-not-supported";
            return false;
        }
    }
    return true;
}

static void
_perform_actions (_sch_xml_to_gnode_parms *_parms, char *curr_op, char *new_op, char *new_xpath)
{
    /* Do nothing if not an edit, or operation not changing. */
    if (!_parms->in_is_edit || g_strcmp0 (curr_op, new_op) == 0)
    {
        return;
    }

    /* Handle operations. */
    if (g_strcmp0 (new_op, "delete") == 0)
    {
        _parms->out_deletes = g_list_append (_parms->out_deletes, g_strdup (new_xpath));
    }
    else if (g_strcmp0 (new_op, "remove") == 0)
    {
        _parms->out_removes = g_list_append (_parms->out_removes, g_strdup (new_xpath));
    }
    else if (g_strcmp0 (new_op, "create") == 0)
    {
        _parms->out_creates = g_list_append (_parms->out_creates, g_strdup (new_xpath));
    }
    else if (g_strcmp0 (new_op, "replace") == 0)
    {
        _parms->out_replaces = g_list_append (_parms->out_replaces, g_strdup (new_xpath));
    }
}

static GNode *
_sch_xml_to_gnode (_sch_xml_to_gnode_parms *_parms, sch_node * schema, xmlNs *ns, char * part_xpath,
                   char * curr_op, GNode * pparent, xmlNode * xml, int depth)
{
    sch_instance *instance = _parms->in_instance;
    int flags = _parms->in_flags;
    char *name = (char *) xml->name;
    xmlNode *child;
    char *attr;
    GNode *tree = NULL;
    GNode *node = NULL;
    char *key = NULL;
    char *new_xpath = NULL;
    char *new_op = curr_op;


    /* Detect change in namespace */
    if (xml->ns && xml->ns->href)
        ns = sch_lookup_ns (instance, schema, (const char *) xml->ns->href, flags, true);

    /* Find schema node */
    if (!schema)
        schema = xmlDocGetRootElement (instance->doc);
    schema = _sch_node_child (ns, schema, name);
    if (schema == NULL)
    {
        ERROR (flags, SCH_E_NOSCHEMANODE, "No schema match for xml node %s%s%s\n",
               ns ? (char *) ns->prefix : "", ns ? ":" : "", name);
        _parms->out_error_tag = "malformed-message";
        return NULL;
    }

    /* Prepend non default namespaces to root nodes */
    if (depth == 0 && ns && ns->prefix && !sch_ns_native (instance, ns))
        name = g_strdup_printf ("%s:%s", ns->prefix, (const char *) xml->name);
    else
        name = g_strdup ((char *) xml->name);

    /* Update xpath. */
    new_xpath = g_strdup_printf ("%s/%s", part_xpath, name);

    /* Check operation, error tag set on exit from routine. */
    if (!_operation_ok (_parms, xml, curr_op, &new_op))
    {
        ERROR (_parms->in_flags, SCH_E_INVALIDQUERY, "Invalid operation\n");
        free (new_xpath);
        free (name);
        return NULL;
    }

    /* LIST */
    if (sch_is_list (schema))
    {
        char *old_xpath = new_xpath;
        char *key_value;

        key = sch_name (sch_node_child_first (sch_node_child_first (schema)));
        DEBUG (_parms->in_flags, "%*s%s%s\n", depth * 2, " ", depth ? "" : "/", name);
        depth++;
        tree = node = APTERYX_NODE (NULL, g_strdup (name));
        attr = (char *) xmlGetProp (xml, BAD_CAST key);
        if (attr)
        {
            node = APTERYX_NODE (node, attr);
            DEBUG (_parms->in_flags, "%*s%s\n", depth * 2, " ", APTERYX_NAME (node));
            if (!(_parms->in_flags & SCH_F_STRIP_KEY) || xmlFirstElementChild (xml))
            {
                APTERYX_NODE (node, g_strdup (key));
                DEBUG (_parms->in_flags, "%*s%s\n", (depth + 1) * 2, " ", key);
            }
            key_value = attr;
        }
        else if (xmlFirstElementChild (xml) &&
                 g_strcmp0 ((const char *) xmlFirstElementChild (xml)->name, key) == 0 &&
                 xml_node_has_content (xmlFirstElementChild (xml)))
        {
            node =
                APTERYX_NODE (node,
                              (char *) xmlNodeGetContent (xmlFirstElementChild (xml)));
            DEBUG (_parms->in_flags, "%*s%s\n", depth * 2, " ", APTERYX_NAME (node));
            key_value = (char *) xmlNodeGetContent (xmlFirstElementChild (xml));
        }
        else
        {
            node = APTERYX_NODE (node, g_strdup ("*"));
            DEBUG (_parms->in_flags, "%*s%s\n", depth * 2, " ", APTERYX_NAME (node));
            key_value = g_strdup ("*");
        }
        schema = sch_node_child_first (schema);
        new_xpath = g_strdup_printf ("%s/%s", old_xpath, key_value);
        g_free (old_xpath);
        g_free (key_value);
    }
    /* CONTAINER */
    else if (!sch_is_leaf (schema))
    {
        DEBUG (_parms->in_flags, "%*s%s%s\n", depth * 2, " ", depth ? "" : "/", name);
        tree = node = APTERYX_NODE (NULL, g_strdup_printf ("%s%s", depth ? "" : "/", name));
    }
    /* LEAF */
    else
    {
        if (g_strcmp0 (new_op, "delete") != 0 && g_strcmp0 (new_op, "remove") != 0)
        {
            tree = node = APTERYX_NODE (NULL, g_strdup (name));
            if (xml_node_has_content (xml) && !(_parms->in_flags & SCH_F_STRIP_DATA))
            {
                char *value = (char *) xmlNodeGetContent (xml);
                value = sch_translate_from (schema, value);
                node = APTERYX_NODE (tree, value);
                DEBUG (_parms->in_flags, "%*s%s = %s\n", depth * 2, " ", name, APTERYX_NAME (node));
            }
            else
            {
                DEBUG (_parms->in_flags, "%*s%s%s\n", depth * 2, " ", depth ? "" : "/", name);
            }
        }
    }

    /* Carry out actions for this operation. Does nothing if not edit-config. */
    _perform_actions (_parms, curr_op, new_op, new_xpath);

    for (child = xmlFirstElementChild (xml); child; child = xmlNextElementSibling (child))
    {
        if ((_parms->in_flags & SCH_F_STRIP_KEY) && key &&
            g_strcmp0 ((const char *) child->name, key) == 0)
        {
            /* The only child is the key with value */
            if (xmlChildElementCount (xml) == 1)
            {
                if (xml_node_has_content (child))
                {
                    /* Want all parameters for one entry in list. */
                    APTERYX_NODE (node, g_strdup ("*"));
                    DEBUG (_parms->in_flags, "%*s%s\n", (depth + 1) * 2, " ", "*");
                }
                else
                {
                    /* Want one field in list element for one or more entries */
                    APTERYX_NODE (node, g_strdup ((const char *) child->name));
                    DEBUG (_parms->in_flags, "%*s%s\n", (depth + 1) * 2, " ", child->name);
                }
                break;
            }
            /* Multiple children - make sure key appears */
            else if (xmlChildElementCount (xml) > 1)
            {
                APTERYX_NODE (node, g_strdup ((const char *) child->name));
                DEBUG (_parms->in_flags, "%*s%s\n", depth * 2, " ", APTERYX_NAME (node));
            }
        }
        else
        {
            GNode *cn = _sch_xml_to_gnode (_parms, schema, ns, new_xpath, new_op, NULL, child, depth + 1);
            if (_parms->out_error_tag)
            {
                apteryx_free_tree (tree);
                tree = NULL;
                ERROR (_parms->in_flags, SCH_E_INVALIDQUERY, "recursive call failed: depth=%d\n", depth);
                goto exit;
            }
            g_node_append (node, cn);
        }
    }

    /* Get everything from here down if a trunk of a subtree */
    if (!xmlFirstElementChild (xml) && sch_node_child_first (schema) &&
        g_strcmp0 (APTERYX_NAME (node), "*") != 0)
    {
        APTERYX_NODE (node, g_strdup ("*"));
        DEBUG (_parms->in_flags, "%*s%s\n", (depth + 1) * 2, " ", "*");
    }

exit:
    free (name);
    free (key);
    if (!tree)
    {
        ERROR (_parms->in_flags, SCH_E_INVALIDQUERY, "returning NULL: xpath=%s\n", new_xpath);
    }
    g_free (new_xpath);
    return tree;
}

static _sch_xml_to_gnode_parms *
sch_parms_init (sch_instance * instance, int flags, char * def_op, bool is_edit)
{
    _sch_xml_to_gnode_parms *_parms = g_malloc (sizeof (*_parms));
    _parms->in_instance = instance;
    _parms->in_flags = flags;
    _parms->in_def_op = def_op;
    _parms->in_is_edit = is_edit;
    _parms->out_tree = NULL;
    _parms->out_error_tag = NULL;
    _parms->out_deletes = NULL;
    _parms->out_removes = NULL;
    _parms->out_creates = NULL;
    _parms->out_replaces = NULL;
    return _parms;
}

GNode *
sch_xml_to_gnode (sch_instance * instance, sch_node * schema, xmlNode * xml, int flags, char * def_op, bool is_edit)
{
    _sch_xml_to_gnode_parms *_parms = sch_parms_init(instance, flags, def_op, is_edit);
    tl_error = SCH_E_SUCCESS;
    _parms->out_tree = _sch_xml_to_gnode (_parms, schema, NULL, "", def_op, NULL, xml, 0);
    return (sch_xml_to_gnode_parms) _parms;
}

GNode *
sch_parm_tree (sch_xml_to_gnode_parms parms)
{
    _sch_xml_to_gnode_parms *_parms = parms;
    GNode *ret;

    if (!_parms)
    {
        return NULL;
    }
    ret = _parms->out_tree;
    _parms->out_tree = NULL;
    return ret;
}

char *
sch_parm_error_tag (sch_xml_to_gnode_parms parms)
{
    _sch_xml_to_gnode_parms *_parms = parms;

    if (!_parms)
    {
        return NULL;
    }
    return _parms->out_error_tag;
}

GList *
sch_parm_deletes (sch_xml_to_gnode_parms parms)
{
    _sch_xml_to_gnode_parms *_parms = parms;

    if (!_parms)
    {
        return NULL;
    }
    return _parms->out_deletes;
}

GList *
sch_parm_removes (sch_xml_to_gnode_parms parms)
{
    _sch_xml_to_gnode_parms *_parms = parms;

    if (!_parms)
    {
        return NULL;
    }
    return _parms->out_removes;
}

GList *
sch_parm_creates (sch_xml_to_gnode_parms parms)
{
    _sch_xml_to_gnode_parms *_parms = parms;

    if (!_parms)
    {
        return NULL;
    }
    return _parms->out_creates;
}

GList *
sch_parm_replaces (sch_xml_to_gnode_parms parms)
{
    _sch_xml_to_gnode_parms *_parms = parms;

    if (!_parms)
    {
        return NULL;
    }
    return _parms->out_replaces;
}

void
sch_parm_free (sch_xml_to_gnode_parms parms)
{
    _sch_xml_to_gnode_parms *_parms = parms;

    if (_parms)
    {
        g_list_free_full (_parms->out_deletes, g_free);
        g_list_free_full (_parms->out_removes, g_free);
        g_list_free_full (_parms->out_creates, g_free);
        g_list_free_full (_parms->out_replaces, g_free);
        g_free (_parms);
    }
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

static json_t *
encode_json_type (sch_node *schema, char *val)
{
    json_t *json = NULL;
    json_int_t i;
    char *p;

    if (*val != '\0')
    {
        /* Only try and detect non string types if no pattern and not enum */
        char *pattern = (char *) xmlGetProp ((xmlNode *)schema, (xmlChar *) "pattern");
        if (((xmlNode *)schema)->children || pattern)
        {
            i = strtoll (val, &p, 10);
            if (*p == '\0')
                json = json_integer (i);
            if (g_strcmp0 (val, "true") == 0)
                json = json_true ();
            if (g_strcmp0 (val, "false") == 0)
                json = json_false ();
        }
        free (pattern);
    }
    if (!json)
        json = json_string (val);
    return json;
}

static bool
_sch_traverse_nodes (sch_node * schema, GNode * parent, int flags)
{
    char *name = sch_name (schema);
    GNode *child = apteryx_find_child (parent, name);
    bool rc = true;

    if (sch_is_leaf (schema))
    {
        if (!child && flags & SCH_F_ADD_MISSING_NULL)
        {
            child = APTERYX_LEAF (parent, name, g_strdup (""));
            name = NULL;
        }
        else if (child && flags & SCH_F_SET_NULL)
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
        else if (flags & SCH_F_ADD_DEFAULTS)
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
                    APTERYX_NODE (child->children, value);
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
        else if (child && flags & SCH_F_TRIM_DEFAULTS)
        {
            char *value = sch_translate_from (schema, sch_default_value (schema));
            if (g_strcmp0 (APTERYX_VALUE (child), value) == 0)
            {
                free ((void *)child->children->data);
                free ((void *)child->data);
                g_node_destroy (child);
            }
            free (value);
        }
    }
    else if (g_strcmp0 (name, "*") == 0)
    {
        for (GNode *child = parent->children; child; child = child->next)
        {
            for (sch_node *s = sch_node_child_first (schema); s; s = sch_node_next_sibling (s))
            {
                rc = _sch_traverse_nodes (s, child, flags);
                if (!rc)
                    goto exit;
            }
        }
    }
    else if (sch_is_leaf_list (schema))
    {
        if (flags & SCH_F_SET_NULL)
        {
            for (GNode *child = parent->children->children; child; child = child->next)
            {
                free (child->children->data);
                child->children->data = g_strdup ("");
            }
        }
    }
    else
    {
        if (!child && !sch_is_list (schema) && (flags & (SCH_F_ADD_DEFAULTS|SCH_F_ADD_MISSING_NULL)))
        {
            child = APTERYX_NODE (parent, name);
            name = NULL;
        }
        if (child)
        {
            for (sch_node *s = sch_node_child_first (schema); s; s = sch_node_next_sibling (s))
            {
                rc = _sch_traverse_nodes (s, child, flags);
                if (!rc)
                    goto exit;
            }
        }
    }

    /* Prune empty branches */
    if (child && !child->children && !sch_is_leaf (schema))
    {
        DEBUG (flags, "Throwing away node \"%s\"\n", APTERYX_NAME (child));
        free ((void *)child->data);
        g_node_destroy (child);
    }

exit:
    free (name);
    return rc;
}

bool
sch_traverse_tree (sch_instance * instance, sch_node * schema, GNode * node, int flags)
{
    bool rc;
    schema = schema ?: xmlDocGetRootElement (instance->doc);
    if (sch_is_leaf (schema))
    {
        rc = _sch_traverse_nodes (schema, node->parent, flags);
    }
    else
    {
        for (sch_node *s = sch_node_child_first (schema); s; s = sch_node_next_sibling (s))
        {
            rc = _sch_traverse_nodes (s, node, flags);
            if (!rc)
                break;
        }
    }
    return rc;
}

static json_t *
_sch_gnode_to_json (sch_instance * instance, sch_node * schema, xmlNs *ns, GNode * node, int flags, int depth)
{
    json_t *data = NULL;
    char *colon;
    char *name;

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
        ns = sch_lookup_ns (instance, schema, name, flags, false);
        if (!ns)
        {
            ERROR (flags, SCH_E_NOSCHEMANODE, "No namespace found \"%s\"\n", name);
            free (name);
            return NULL;
        }
        char *_name = name;
        name = g_strdup (colon + 1);
        free (_name);
    }

    /* Find schema node */
    if (!schema)
        schema = xmlDocGetRootElement (instance->doc);
    schema = _sch_node_child (ns, schema, name);
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

    if (sch_is_leaf_list (schema) && (flags & SCH_F_JSON_ARRAYS))
    {
        data = json_array ();
        apteryx_sort_children (node, g_strcmp0);

        DEBUG (flags, "%*s%s[", depth * 2, " ", APTERYX_VALUE (node));
        for (GNode * child = node->children; child; child = child->next)
        {
            DEBUG (flags, "%s%s", APTERYX_VALUE (child), child->next ? ", " : "");
            json_array_append_new (data, json_string ((const char* ) APTERYX_VALUE (child)));
        }
        DEBUG (flags, "]\n");
    }
    else if (sch_is_list (schema) && (flags & SCH_F_JSON_ARRAYS))
    {
        data = json_array ();
        apteryx_sort_children (node, g_strcmp0);
        for (GNode * child = node->children; child; child = child->next)
        {
            DEBUG (flags, "%*s%s[%s]\n", depth * 2, " ", APTERYX_NAME (node),
                   APTERYX_NAME (child));
            json_t *obj = json_object();
            gnode_sort_children (sch_node_child_first (schema), child);
            for (GNode * field = child->children; field; field = field->next)
            {
                json_t *node = _sch_gnode_to_json (instance, sch_node_child_first (schema), ns, field, flags, depth + 1);
                json_object_set_new (obj, APTERYX_NAME (field), node);
            }
            json_array_append_new (data, obj);
        }
    }
    else if (!sch_is_leaf (schema))
    {
        DEBUG (flags, "%*s%s\n", depth * 2, " ", APTERYX_NAME (node));
        data = json_object();
        gnode_sort_children (schema, node);
        for (GNode * child = node->children; child; child = child->next)
        {
            json_t *node = _sch_gnode_to_json (instance, schema, ns, child, flags, depth + 1);
            json_object_set_new (data, APTERYX_NAME (child), node);
        }
        if (json_object_iter (data) == NULL)
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

json_t *
sch_gnode_to_json (sch_instance * instance, sch_node * schema, GNode * node, int flags)
{
    xmlNs *ns = schema ? ((xmlNode *) schema)->ns : NULL;
    json_t *json = NULL;
    json_t *child;

    tl_error = SCH_E_SUCCESS;
    if (schema)
    {
        schema = ((xmlNode *)schema)->parent;
        child = _sch_gnode_to_json (instance, schema, ns, node, flags, g_node_max_height (node));
        if (child)
        {
            json = json_object ();
            json_object_set_new (json, APTERYX_NAME (node), child);
        }
    }
    else
    {
        child = _sch_gnode_to_json (instance, schema, ns, node, flags, 0);
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
            json = json_object ();
            json_object_set_new (json, name, child);
        }
    }
    return json;
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
        ns = sch_lookup_ns (instance, schema, namespace, flags, false);
        free (namespace);
        name = colon + 1;
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
            APTERYX_LEAF_STRING (tree, json_string_value (child), json_string_value (child));
            DEBUG (flags, "%*s%s = %s\n", depth * 2, " ", json_string_value (child), json_string_value (child));
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

            node = APTERYX_NODE (tree, kname);
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
