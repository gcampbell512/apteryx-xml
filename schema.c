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

/* List full paths for all XML files in the search path */
static void
list_xml_files (GList ** files, const char *path)
{
    DIR *dp;
    struct dirent *ep;
    char *saveptr = NULL;
    char *cpath;
    char *dpath;

    cpath = strdup (path);
    dpath = strtok_r (cpath, ":", &saveptr);
    while (dpath != NULL)
    {
        dp = opendir (dpath);
        if (dp != NULL)
        {
            while ((ep = readdir (dp)))
            {
                char *filename = NULL;
                if ((fnmatch ("*.xml", ep->d_name, FNM_PATHNAME) != 0) &&
                    (fnmatch ("*.xml.gz", ep->d_name, FNM_PATHNAME) != 0))
                {
                    continue;
                }
                if (asprintf (&filename, "%s/%s", dpath, ep->d_name) > 0)
                {
                    *files = g_list_append (*files, filename);
                }
            }
            (void) closedir (dp);
        }
        dpath = strtok_r (NULL, ":", &saveptr);
    }
    free (cpath);
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
add_module_info_to_child (xmlNode *module)
{
    xmlChar *mod = xmlGetProp (module, (xmlChar *)"model");
    xmlChar *org = xmlGetProp (module, (xmlChar *)"organization");
    xmlChar *ver = xmlGetProp (module, (xmlChar *)"version");
    xmlNsPtr def = xmlSearchNs (module->doc, module, NULL);
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
            if (ns->prefix && !xmlSearchNsByHref (doc, xmlDocGetRootElement (doc), ns->href))
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
        if (n->ns) {
            n->ns = xmlSearchNsByHref (doc, xmlDocGetRootElement (doc), n->ns->href);
        }

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

/* Parse all XML files in the search path and merge trees */
sch_instance *
sch_load (const char *path)
{
    xmlDoc *doc;
    xmlNode *module;
    GList *files = NULL;
    GList *iter;

    /* Create a new doc and root node for the merged MODULE */
    doc = xmlNewDoc ((xmlChar *) "1.0");
    module = xmlNewNode (NULL, (xmlChar *) "MODULE");
    // TODO configurable default namespace
    xmlNewNs (module, (const xmlChar *) "https://github.com/alliedtelesis/apteryx", NULL);
    xmlNewNs (module, (const xmlChar *) "http://www.w3.org/2001/XMLSchema-instance", (const xmlChar *) "xsi");
    xmlNewProp (module, (const xmlChar *) "xsi:schemaLocation",
        (const xmlChar *) "https://github.com/alliedtelesis/apteryx-xml https://github.com/alliedtelesis/apteryx-xml/releases/download/v1.2/apteryx.xsd");
    xmlDocSetRootElement (doc, module);

    list_xml_files (&files, path);
    for (iter = files; iter; iter = g_list_next (iter))
    {
        char *filename = (char *) iter->data;
        xmlDoc *doc_new = xmlParseFile (filename);
        if (doc_new == NULL)
        {
            syslog (LOG_ERR, "XML: failed to parse \"%s\"", filename);
            continue;
        }
        xmlNode *module_new = xmlDocGetRootElement (doc_new);
        cleanup_nodes (module_new);
        copy_nsdef_to_root (doc, module_new);
        add_module_info_to_child (module_new);
        merge_nodes (module, module->children, module_new->children, 0);
        xmlFreeDoc (doc_new);
        assign_ns_to_root (doc, module->children);
    }
    g_list_free_full (files, free);

    return (sch_instance *) xmlDocGetRootElement (doc);
}

void
sch_free (sch_instance * schema)
{
    xmlNode *xml = (xmlNode *) schema;
    xmlFreeDoc (xml->doc);
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
sch_dump_xml (sch_instance * schema)
{
    xmlNode *xml = (xmlNode *) schema;
    xmlChar *xmlbuf = NULL;
    int bufsize;

    xmlDoc *copy = xmlCopyDoc (xml->doc, 1);
    remove_hidden_children (xmlDocGetRootElement (copy));
    xmlDocDumpFormatMemory (copy, &xmlbuf, &bufsize, 1);
    xmlFreeDoc (copy);
    return (char *) xmlbuf;
}

bool
sch_ns_match (xmlNode *node, const char *namespace)
{
    /* Check for default namespace match */
    if (namespace == NULL)
    {
        if (!node->ns || node->ns->prefix == NULL)
            return TRUE;
        return FALSE;
    }

    /* Check for a prefix match */
    // TODO this allows /t2:test/settings/aug2:speed to match t2:speed
    while (node && node->type == XML_ELEMENT_NODE)
    {
        if (node->ns && node->ns->prefix && g_strcmp0 ((const char *)node->ns->prefix, namespace) == 0)
            return TRUE;
        node = node->parent;
    }

    /* No match */
    return FALSE;
}

static xmlNode *
lookup_node (const char *namespace, xmlNode * node, const char *path)
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
        key = strndup (path, len);
        path += len;
    }
    else
    {
        key = strdup (path);
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
        if (name && (name[0] == '*' || match_name (name, key)) && sch_ns_match (n, namespace))
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
                    return lookup_node (namespace, xmlDocGetRootElement (node->doc), path);
                }
                xmlFree (name);
                if (mode)
                {
                    xmlFree (mode);
                }
                return lookup_node (namespace, n, path);
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
sch_lookup (sch_instance * schema, const char *path)
{
    return lookup_node (NULL, (xmlNode *) schema, path);
}

static sch_node *
_sch_node_child (const char *namespace, sch_node * parent, const char *child)
{
    xmlNode *xml = (xmlNode *) parent;
    xmlNode *n = xml->children;

    while (n)
    {
        if (n->type == XML_ELEMENT_NODE && n->name[0] == 'N')
        {
            char *name = (char *) xmlGetProp (n, (xmlChar *) "name");
            if (name && (name[0] == '*' || match_name (name, child)) && sch_ns_match (n, namespace))
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
sch_node_child (sch_node * parent, const char *child)
{
    return _sch_node_child (NULL, parent, child);
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

static bool parse_fields (sch_instance * instance, sch_node * schema, char *fields, GNode *parent, int flags, int depth);
static GNode *
parse_field (sch_instance * instance, sch_node * schema, const char *path, int flags, int depth)
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
        name = strndup (path, sublist - path);
    else if (next)
        name = strndup (path, next - path);
    else
        name = strdup (path);

    /* Find schema node */
    if (!schema)
        schema = sch_lookup (instance, name);
    else
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
        child = parse_field (instance, schema, next + 1, flags, depth + 1);
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
        if (!parse_fields (instance, schema, fields, rnode, flags, depth + 1))
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
parse_fields (sch_instance * instance, sch_node * schema, char *fields, GNode *parent, int flags, int depth)
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
            char *field = strndup (t, (h - t + 1));
            GNode *node = parse_field (instance, schema, field, flags, depth);
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

static char *
convert_model_to_prefix (xmlNode *node, char *ns)
{
    char *namespace = ns;

    while (node && node->type == XML_ELEMENT_NODE)
    {
        char *model = (char *) xmlGetProp (node, (xmlChar *) "model");
        if (model)
        {
            if (g_strcmp0 (model, ns) == 0)
            {
                free (ns);
                namespace = NULL;
                if (node->ns && node->ns->prefix)
                {
                    namespace = strdup ((char *) node->ns->prefix);
                }
                return namespace;
            }
        }
        /* Check parent */
        node = node->next;
    }
    return namespace;
}

static GNode *
_sch_path_to_query (sch_instance * instance, sch_node * schema, char *namespace, const char *path, int flags, int depth)
{
    const char *next = NULL;
    GNode *node = NULL;
    GNode *rnode = NULL;
    GNode *child = NULL;
    char *query = NULL;
    char *pred = NULL;
    char *ns = NULL;
    char *name = NULL;

    if (path && path[0] == '/')
    {
        path++;

        /* Parse path element */
        ns = strchr (path, ':');
        if (ns)
        {
            namespace = strndup (path, ns - path);
            if (flags & SCH_F_NS_MODEL_NAME)
                namespace = convert_model_to_prefix (schema ?: sch_node_child_first (instance), namespace);
            path = ns + 1;
            ns = namespace; /* Need to free this */
        }
        query = strchr (path, '?');
        next = strchr (path, '/');
        if (query && (!next || query < next))
            name = strndup (path, query - path);
        else if (next)
            name = strndup (path, next - path);
        else
            name = strdup (path);
        if (query && next && query < next)
            next = NULL;
        if (flags & SCH_F_XPATH)
        {
            pred = strchr (name, '[');
            if (pred)
            {
                char *temp = strndup (name, pred - name);
                pred = strdup (pred);
                free (name);
                name = temp;
            }
        }

        /* Find schema node */
        if (!schema || sch_is_proxy (schema))
            schema = lookup_node ((const char *)namespace, instance, name);
        else
            schema = _sch_node_child (namespace, schema, name);
        if (schema == NULL)
        {
            ERROR (flags, SCH_E_NOSCHEMANODE, "No schema match for %s:%s\n", namespace, name);
            goto exit;
        }
        if (!sch_is_readable (schema))
        {
            ERROR (flags, SCH_E_NOTREADABLE, "Ignoring non-readable node %s\n", name);
            goto exit;
        }

        /* Create node */
        if (depth == 0)
        {
            if (namespace)
            {
                rnode = APTERYX_NODE (NULL, g_strdup_printf ("/%s:%s", namespace, name));
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

        if (next)
        {
            node = _sch_path_to_query (instance, schema, namespace, next, flags, depth + 1);
            if (!node)
            {
                free ((void *)rnode->data);
                g_node_destroy (rnode);
                rnode = NULL;
                goto exit;
            }
            g_node_prepend (child ? : rnode, node);
        }
        else if (query)
        {
            char *ptr = NULL;
            char *parameter;

            /* Split query after '?' by '&' */
            query = g_strdup (query + 1);
            parameter = strtok_r (query, "&", &ptr);
            while (parameter)
            {
                if (strncmp (parameter, "fields=", strlen ("fields=")) == 0)
                {
                    if (!parse_fields (instance, schema, parameter + strlen ("fields="), rnode, flags, depth + 1))
                    {
                        apteryx_free_tree (rnode);
                        rnode = NULL;
                        free (query);
                        goto exit;
                    }
                }
                else
                {
                    ERROR (flags, SCH_E_INVALIDQUERY, "Do not support query \"%s\"\n", parameter);
                    apteryx_free_tree (rnode);
                    rnode = NULL;
                    free (query);
                    goto exit;
                }
                parameter = strtok_r (NULL, "&", &ptr);
            }
            free (query);
        }
        else if (sch_node_child_first (schema))
        {
            /* Get everything from here down if we do not already have a star */
            if (child && g_strcmp0 (APTERYX_NAME (child), "*") != 0)
            {
                APTERYX_NODE (child, g_strdup ("*"));
                DEBUG (flags, "%*s%s\n", (depth + 1) * 2, " ", "*");
            }
            else if (g_strcmp0 (APTERYX_NAME (rnode), "*") != 0)
            {
                APTERYX_NODE (rnode, g_strdup ("*"));
                DEBUG (flags, "%*s%s\n", (depth + 1) * 2, " ", "*");
            }
        }
    }

exit:
    free (name);
    free (ns);
    return rnode;
}

GNode *
sch_path_to_query (sch_instance * instance, sch_node * schema, const char *path, int flags)
{
    tl_error = SCH_E_SUCCESS;
    return _sch_path_to_query (instance, schema, NULL, path, flags, 0);
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
_sch_gnode_to_xml (sch_instance * instance, sch_node * schema, char *namespace, xmlNode * parent,
                   GNode * node, int flags, int depth)
{
    xmlNode *data = NULL;
    char *ns = NULL;
    char *name;

    /* Get the actual node name */
    if (depth == 0 && strlen (APTERYX_NAME (node)) == 1)
    {
        return _sch_gnode_to_xml (instance, schema, namespace, parent, node->children, flags, depth);
    }
    else if (depth == 0 && APTERYX_NAME (node)[0] == '/')
    {
        name = APTERYX_NAME (node) + 1;
    }
    else
    {
        name = APTERYX_NAME (node);
    }

    ns = strchr (name, ':');
    if (ns)
    {
        namespace = strndup (name, ns - name);
        name = ns + 1;
        ns = namespace; /* Need to free this */
    }

    /* Find schema node */
    if (!schema)
        schema = lookup_node (namespace, instance, name);
    else
        schema = _sch_node_child (namespace, schema, name);
    if (schema == NULL)
    {
        ERROR (flags, SCH_E_NOSCHEMANODE, "No schema match for gnode %s:%s\n", namespace, name);
        free (ns);
        return NULL;
    }
    if (!sch_is_readable (schema))
    {
        ERROR (flags, SCH_E_NOTREADABLE, "Ignoring non-readable node %s:%s\n", namespace, name);
        free (ns);
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
                if (_sch_gnode_to_xml (instance, sch_node_child_first (schema), namespace, 
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
            if (_sch_gnode_to_xml (instance, schema, namespace, data, child, flags, depth + 1))
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
            char *value = strdup (APTERYX_VALUE (node) ? APTERYX_VALUE (node) : "");
            data = xmlNewNode (NULL, BAD_CAST name);
            value = sch_translate_to (schema, value);
            xmlNodeSetContent (data, (const xmlChar *) value);
            if (parent)
                xmlAddChildList (parent, data);
            DEBUG (flags, "%*s%s = %s\n", depth * 2, " ", APTERYX_NAME (node), value);
            free (value);
        }
    }

    free (ns);
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

static GNode *
_sch_xml_to_gnode (sch_instance * instance, sch_node * schema, const char * namespace, GNode * parent,
                   xmlNode * xml, int flags, int depth)
{
    char *name = (char *) xml->name;
    xmlNode *child;
    char *attr;
    GNode *tree = NULL;
    GNode *node = NULL;
    char *key = NULL;

    /* Detect change in namespace */
    if (xml->ns && xml->ns->href)
    {
        xmlNsPtr ns = NULL;

        // TODO configurable default namespace
        if (g_strcmp0 ((const char *)xml->ns->href, "https://github.com/alliedtelesis/apteryx") == 0)
        {
            namespace = NULL;
        }
        else if (schema)
        {
            ns = xmlSearchNsByHref (((xmlNode *)instance)->doc, schema, xml->ns->href);
        }
        else
        {
            child = ((xmlNode *)instance)->children;
            while (child)
            {
                ns = xmlSearchNsByHref (((xmlNode *)instance)->doc, child, xml->ns->href);
                if (ns)
                    break;
                child = child->next;
            }
        }
        if (ns)
            namespace = (const char *) ns->prefix;
    }

    /* Find schema node */
    if (!schema)
        schema = lookup_node (namespace, instance, name);
    else
        schema = _sch_node_child (namespace, schema, name);
    if (schema == NULL)
    {
        ERROR (flags, SCH_E_NOSCHEMANODE, "No schema match for xml node %s:%s\n", namespace, name);
        return NULL;
    }

    /* Prepend non default namespaces to root nodes */
    if (depth == 0 && namespace)
        name = g_strdup_printf ("%s:%s", namespace, (const char *) xml->name);

    /* LIST */
    if (sch_is_list (schema))
    {
        key = sch_name (sch_node_child_first (sch_node_child_first (schema)));
        DEBUG (flags, "%*s%s%s\n", depth * 2, " ", depth ? "" : "/", name);
        depth++;
        tree = node = APTERYX_NODE (NULL, g_strdup (name));
        attr = (char *) xmlGetProp (xml, BAD_CAST key);
        if (attr)
        {
            node = APTERYX_NODE (node, attr);
            DEBUG (flags, "%*s%s\n", depth * 2, " ", APTERYX_NAME (node));
            if (!(flags & SCH_F_STRIP_KEY) || xmlFirstElementChild (xml))
            {
                APTERYX_NODE (node, g_strdup (key));
                DEBUG (flags, "%*s%s\n", (depth + 1) * 2, " ", key);
            }
        }
        else if (xmlFirstElementChild (xml) &&
                 g_strcmp0 ((const char *) xmlFirstElementChild (xml)->name, key) == 0 &&
                 xml_node_has_content (xmlFirstElementChild (xml)))
        {
            node =
                APTERYX_NODE (node,
                              (char *) xmlNodeGetContent (xmlFirstElementChild (xml)));
            DEBUG (flags, "%*s%s\n", depth * 2, " ", APTERYX_NAME (node));
        }
        else
        {
            node = APTERYX_NODE (node, g_strdup ("*"));
            DEBUG (flags, "%*s%s\n", depth * 2, " ", APTERYX_NAME (node));
        }
        schema = sch_node_child_first (schema);
    }
    /* CONTAINER */
    else if (!sch_is_leaf (schema))
    {
        DEBUG (flags, "%*s%s%s\n", depth * 2, " ", depth ? "" : "/", name);
        tree = node = APTERYX_NODE (NULL, g_strdup_printf ("%s%s", depth ? "" : "/", name));
    }
    /* LEAF */
    else
    {
        tree = node = APTERYX_NODE (NULL, g_strdup (name));
        attr = (char *) xmlGetProp (xml, BAD_CAST "operation");
        if (g_strcmp0 (attr, "delete") == 0)
        {
            node = APTERYX_NODE (tree, g_strdup (""));
            DEBUG (flags, "%*s%s = NULL\n", depth * 2, " ", name);
        }
        else if (xml_node_has_content (xml))
        {
            node = APTERYX_NODE (tree, (char *) xmlNodeGetContent (xml));
            DEBUG (flags, "%*s%s = %s\n", depth * 2, " ", name, APTERYX_NAME (node));
        }
        else
        {
            DEBUG (flags, "%*s%s%s\n", depth * 2, " ", depth ? "" : "/", name);
        }
        free (attr);
    }

    for (child = xmlFirstElementChild (xml); child; child = xmlNextElementSibling (child))
    {
        if ((flags & SCH_F_STRIP_KEY) && key &&
            g_strcmp0 ((const char *) child->name, key) == 0)
        {
            /* The only child is the key with value */
            if (xmlChildElementCount (xml) == 1)
            {
                if (xml_node_has_content (child))
                {
                    /* Want all parameters for one entry in list. */
                    APTERYX_NODE (node, g_strdup ("*"));
                    DEBUG (flags, "%*s%s\n", (depth + 1) * 2, " ", "*");
                }
                else
                {
                    /* Want one field in list element for one or more entries */
                    APTERYX_NODE (node, g_strdup ((const char *) child->name));
                    DEBUG (flags, "%*s%s\n", (depth + 1) * 2, " ", child->name);
                }
                break;
            }
            /* Multiple children - make sure key appears */
            else if (xmlChildElementCount (xml) > 1)
            {
                APTERYX_NODE (node, g_strdup ((const char *) child->name));
                DEBUG (flags, "%*s%s\n", depth * 2, " ", APTERYX_NAME (node));
            }
        }
        else
        {
            GNode *cn = _sch_xml_to_gnode (instance, schema, namespace, NULL, child, flags, depth + 1);
            if (!cn)
            {
                apteryx_free_tree (tree);
                tree = NULL;
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
        DEBUG (flags, "%*s%s\n", (depth + 1) * 2, " ", "*");
    }

exit:
    if (depth == 0 && namespace)
        free (name);
    free (key);
    return tree;
}

GNode *
sch_xml_to_gnode (sch_instance * instance, sch_node * schema, xmlNode * xml, int flags)
{
    tl_error = SCH_E_SUCCESS;
    return _sch_xml_to_gnode (instance, schema, NULL, NULL, xml, flags, 0);
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
encode_json_type (char *val)
{
    json_t *json = NULL;
    json_int_t i;
    char *p;

    if (*val != '\0')
    {
        i = strtoll (val, &p, 10);
        if (*p == '\0')
            json = json_integer (i);
        if (g_strcmp0 (val, "true") == 0)
            json = json_true ();
        if (g_strcmp0 (val, "false") == 0)
            json = json_false ();
    }
    if (!json)
        json = json_string (val);
    return json;
}

static json_t *
_sch_gnode_to_json (sch_instance * instance, sch_node * schema, char *namespace, GNode * node, int flags, int depth)
{
    json_t *data = NULL;
    char *ns = NULL;
    char *name;

    /* Get the actual node name */
    if (depth == 0 && strlen (APTERYX_NAME (node)) == 1)
    {
        return _sch_gnode_to_json (instance, schema, namespace, node->children, flags, depth);
    }
    else if (depth == 0 && APTERYX_NAME (node)[0] == '/')
    {
        name = APTERYX_NAME (node) + 1;
    }
    else
    {
        name = APTERYX_NAME (node);
    }

    ns = strchr (name, ':');
    if (ns)
    {
        namespace = strndup (name, ns - name);
        name = ns + 1;
        ns = namespace; /* Need to free this */
    }

    /* Find schema node */
    if (!schema)
        schema = lookup_node (namespace, instance, name);
    else
        schema = _sch_node_child (namespace, schema, name);
    if (schema == NULL)
    {
        ERROR (flags, SCH_E_NOSCHEMANODE, "No schema match for gnode %s:%s\n", namespace, name);
        free (ns);
        return NULL;
    }
    if (!sch_is_readable (schema))
    {
        ERROR (flags, SCH_E_NOTREADABLE, "Ignoring non-readable node %s:%s\n", namespace, name);
        free (ns);
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
                json_t *node = _sch_gnode_to_json (instance, sch_node_child_first (schema), namespace, field, flags, depth + 1);
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
            json_t *node = _sch_gnode_to_json (instance, schema, namespace, child, flags, depth + 1);
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
        char *value = strdup (APTERYX_VALUE (node) ? APTERYX_VALUE (node) : "");
        if (flags & SCH_F_JSON_TYPES)
        {
            value = sch_translate_to (schema, value);
            data = encode_json_type (value);
        }
        else
            data = json_string (value);
        DEBUG (flags, "%*s%s = %s\n", depth * 2, " ", APTERYX_NAME (node), value);
        free (value);
    }

    free (ns);
    return data;
}

json_t *
sch_gnode_to_json (sch_instance * instance, sch_node * schema, GNode * node, int flags)
{
    json_t *json = NULL;
    json_t *child;

    tl_error = SCH_E_SUCCESS;
    if (schema)
    {
        schema = ((xmlNode *)schema)->parent;
        child = _sch_gnode_to_json (instance, schema, NULL, node, flags, g_node_max_height (node));
        if (child)
        {
            json = json_object ();
            json_object_set_new (json, APTERYX_NAME (node), child);
        }
    }
    else
    {
        child = _sch_gnode_to_json (instance, schema, NULL, node, flags, 0);
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
_sch_json_to_gnode (sch_instance * instance, sch_node * schema,
                   json_t * json, const char *name, int flags, int depth)
{
    json_t *child;
    json_t *kchild;
    const char *cname;
    size_t index;
    GNode *tree = NULL;
    GNode *node = NULL;
    char *key = NULL;
    char *value;

    /* Find schema node */
    if (!schema)
        schema = sch_lookup (instance, name);
    else
        schema = sch_node_child (schema, name);
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
                GNode *cn = _sch_json_to_gnode (instance, schema, subchild, subname, flags, depth + 1);
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
            GNode *cn = _sch_json_to_gnode (instance, schema, child, cname, flags, depth + 1);
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
        node = _sch_json_to_gnode (instance, schema, child, key, flags, depth);
        if (!node)
        {
            apteryx_free_tree (root);
            return NULL;
        }
        g_node_append (root, node);
    }
    return root;
}
