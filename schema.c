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

#define DEBUG(fmt, args...)
// #define DEBUG(fmt, args...)  printf (fmt, ## args);

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

/* Merge nodes from a new tree to the original tree */
static void
merge_nodes (xmlNode * orig, xmlNode * new, int depth)
{
    xmlNode *n;
    xmlNode *o;

    for (n = new; n; n = n->next)
    {
        char *orig_name = NULL;
        char *new_name;
        if (n->type != XML_ELEMENT_NODE)
        {
            continue;
        }
        new_name = (char *) xmlGetProp (n, (xmlChar *) "name");
        if (new_name)
        {
            for (o = orig; o; o = o->next)
            {
                orig_name = (char *) xmlGetProp (o, (xmlChar *) "name");
                if (orig_name)
                {
                    if (strcmp (new_name, orig_name) == 0)
                    {
                        xmlFree (orig_name);
                        break;
                    }
                    xmlFree (orig_name);
                }
            }
            xmlFree (new_name);
            if (o)
            {
                merge_nodes (o->children, n->children, depth + 1);
            }
            else
            {
                xmlAddPrevSibling (orig, xmlCopyNode (n, 1));
            }
        }
    }
    return;
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
            xmlSetNs (n, NULL);
        }
        else
        {
            xmlUnlinkNode (n);
            xmlFreeNode (n);
        }
        n = next;
    }
}

/* Parse all XML files in the search path and merge trees */
sch_instance *
sch_load (const char *path)
{
    xmlDoc *doc = NULL;
    GList *files = NULL;
    GList *iter;

    list_xml_files (&files, path);
    for (iter = files; iter; iter = g_list_next (iter))
    {
        char *filename = (char *) iter->data;
        xmlDoc *new = xmlParseFile (filename);
        if (new == NULL)
        {
            syslog (LOG_ERR, "LUA: failed to parse \"%s\"", filename);
            continue;
        }
        cleanup_nodes (xmlDocGetRootElement (new)->children);
        if (doc == NULL)
        {
            doc = new;
        }
        else
        {
            merge_nodes (xmlDocGetRootElement (doc)->children,
                         xmlDocGetRootElement (new)->children, 0);
            xmlFreeDoc (new);
        }
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

char *
sch_dump_xml (sch_instance * schema)
{
    xmlNode *xml = (xmlNode *) schema;
    xmlChar *xmlbuf = NULL;
    int bufsize;

    xmlDocDumpFormatMemory (xml->doc, &xmlbuf, &bufsize, 1);
    return (char *) xmlbuf;
}

static xmlNode *
lookup_node (xmlNode * node, const char *path, char **namespace)
{
    xmlNode *n;
    char *name, *mode;
    char *key = NULL;
    char *ns = NULL;
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
    if (namespace)
    {
        ns = strchr (key, ':');
        if (ns)
        {
            len = ns - key;
            free (*namespace);
            *namespace = key;
            (*namespace)[len] = '\0';
            key = strndup (key + len + 1, strlen (key) - len - 1);
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
        if (name && (name[0] == '*' || match_name (name, key)))
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
                    return lookup_node (xmlDocGetRootElement (node->doc), path, namespace);
                }
                xmlFree (name);
                if (mode)
                {
                    xmlFree (mode);
                }
                return lookup_node (n, path, namespace);
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
    return lookup_node ((xmlNode *) schema, path, NULL);
}

sch_node *
sch_ns_lookup (sch_instance * schema, const char *namespace, const char *path)
{
    char *ns = namespace ? strdup (namespace) : NULL;
    sch_node *node = lookup_node ((xmlNode *) schema, path, &ns);
    free (ns);
    return node;
}

sch_node *
sch_node_child (sch_node * parent, const char *child)
{
    xmlNode *xml = (xmlNode *) parent;
    xmlNode *n = xml->children;

    while (n)
    {
        if (n->type == XML_ELEMENT_NODE && n->name[0] == 'N')
        {
            char *name = (char *) xmlGetProp (n, (xmlChar *) "name");
            if (name && (name[0] == '*' || match_name (name, child)))
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
sch_node_child_next (sch_node * parent, sch_node * node)
{
    xmlNode *xml = (xmlNode *) parent;
    xmlNode *n = node ? ((xmlNode *) node)->next : xml->children;

    while (n)
    {
        if (n->type == XML_ELEMENT_NODE && n->name[0] == 'N')
            break;
        n = n->next;
    }
    return n;
}

sch_node *
sch_node_child_first (sch_node * parent)
{
    return sch_node_child_next (parent, NULL);
}

char *
sch_name (sch_node * node)
{
    return (char *) xmlGetProp (node, (xmlChar *) "name");
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

bool
sch_is_list (sch_node * node)
{
    xmlNode *xml = (xmlNode *) node;
    xmlNode *child = xml->children;

    if (child && !child->next && child->type == XML_ELEMENT_NODE && child->name[0] == 'N')
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
    if (!mode || strchr (mode, 'r') != NULL)
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
sch_validate_pattern (sch_node * node, const char *value)
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
            DEBUG ("SCHEMA: %i (\"%s\") for regex %s", rc, message, pattern);
            xmlFree (pattern);
            return false;
        }

        rc = regexec (&regex_obj, value, 0, NULL, 0);
        regfree (&regex_obj);
        if (rc == REG_ESPACE)
        {
            regerror (rc, NULL, message, sizeof (message));
            DEBUG ("SCHEMA: %i (\"%s\") for regex %s", rc, message, pattern);
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
            DEBUG ("SCHEMA: \"%s\" not in enumeration\n", value);
            return false;
        }
    }
    return true;
}

/* Data translation/manipulation */

static GNode *
_sch_path_to_query (sch_instance * instance, sch_node * schema, const char *path, int flags, int depth)
{
    const char *next;
    GNode *node = NULL;
    GNode *rnode = NULL;
    GNode *child = NULL;
    char *pred = NULL;
    char *name;

    if (path && path[0] == '/')
    {
        path++;

        /* Find name */
        next = strchr (path, '/');
        if (next)
            name = strndup (path, next - path);
        else
            name = strdup (path);
        if (flags & SCH_F_XPATH)
        {
            pred = strchr (name, '[');
            if (pred)
            {
                char *temp = strndup (name, pred - name);
                pred = strdup (pred);
                g_free (name);
                name = temp;
            }
        }

        /* Find schema node */
        if (!schema)
            schema = sch_lookup (instance, name);
        else
            schema = sch_node_child (schema, name);
        if (schema == NULL)
        {
            DEBUG ("ERROR: No schema match for %s\n", name);
            g_free (name);
            return NULL;
        }
        if (!sch_is_readable (schema))
        {
            DEBUG ("Ignoring non-readable node %s\n", name);
            g_free (name);
            return NULL;
        }

        /* Create node */
        if (depth == 0)
        {
            rnode = APTERYX_NODE (NULL, g_strdup_printf ("/%s", name));
            g_free (name);
        }
        else
            rnode = APTERYX_NODE (NULL, name);
        DEBUG ("%*s%s\n", depth * 2, " ", APTERYX_NAME (rnode));

        /* XPATH predicates */
        if (pred && sch_is_list (schema)) {
            char key[128 + 1];
            char value[128 + 1];

            if (sscanf (pred, "[%128[^=]='%128[^']']", key, value) == 2) {
                // TODO make sure this key is the list key
                child = APTERYX_NODE (NULL, g_strdup (value));
                g_node_prepend (rnode, child);
                depth++;
                DEBUG ("%*s%s\n", depth * 2, " ", APTERYX_NAME (child));
                if (next) {
                    APTERYX_NODE (child, g_strdup (key));
                    depth++;
                    DEBUG ("%*s%s\n", depth * 2, " ", APTERYX_NAME (child));
                }
            }
            g_free (pred);
            schema = sch_node_child_first (schema);
        }

        if (next)
        {
            node = _sch_path_to_query (instance, schema, next, flags, depth + 1);
            if (!node)
            {
                free ((void *)rnode->data);
                g_node_destroy (rnode);
                return NULL;
            }
            g_node_prepend (child ? : rnode, node);
        }
        else if (sch_node_child_first (schema))
        {
            /* Get everything from here down if we do not already have a star */
            if (child && g_strcmp0 (APTERYX_NAME (child), "*") != 0)
            {
                APTERYX_NODE (child, g_strdup ("*"));
                DEBUG ("%*s%s\n", (depth + 1) * 2, " ", "*");
            }
            else if (g_strcmp0 (APTERYX_NAME (rnode), "*") != 0)
            {
                APTERYX_NODE (rnode, g_strdup ("*"));
                DEBUG ("%*s%s\n", (depth + 1) * 2, " ", "*");
            }
        }
    }

    return rnode;
}

GNode *
sch_path_to_query (sch_instance * instance, sch_node * schema, const char *path, int flags)
{
    return _sch_path_to_query (instance, schema, path, flags, 0);
}

static int
get_index (GNode * node, sch_node * schema)
{
    int index = 0;
    sch_node *n;
    for (n = sch_node_child_first (schema); n; index++, n = sch_node_child_next (schema, n))
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
_sch_gnode_to_xml (sch_instance * instance, sch_node * schema, xmlNode * parent,
                   GNode * node, int flags, int depth)
{
    xmlNode *data = NULL;
    char *name;

    /* Get the actual node name */
    if (depth == 0 && strlen (APTERYX_NAME (node)) == 1)
    {
        return _sch_gnode_to_xml (instance, schema, parent, node->children, flags, depth);
    }
    else if (depth == 0 && APTERYX_NAME (node)[0] == '/')
    {
        name = APTERYX_NAME (node) + 1;
    }
    else
    {
        name = APTERYX_NAME (node);
    }

    /* Find schema node */
    if (!schema)
        schema = sch_lookup (instance, name);
    else
        schema = sch_node_child (schema, name);
    if (schema == NULL)
    {
        DEBUG ("ERROR: No schema match for gnode %s\n", name);
        return NULL;
    }

    if (sch_is_list (schema))
    {
        apteryx_sort_children (node, g_strcmp0);
        for (GNode * child = node->children; child; child = child->next)
        {
            DEBUG ("%*s%s[%s]\n", depth * 2, " ", APTERYX_NAME (node),
                   APTERYX_NAME (child));
            data = xmlNewNode (NULL, BAD_CAST name);
            gnode_sort_children (sch_node_child_first (schema), child);
            for (GNode * field = child->children; field; field = field->next)
            {
                _sch_gnode_to_xml (instance, sch_node_child_first (schema), data, field,
                                   flags, depth + 1);
            }
            xmlAddChildList (parent, data);
        }
    }
    else if (!sch_is_leaf (schema))
    {
        DEBUG ("%*s%s\n", depth * 2, " ", APTERYX_NAME (node));
        if (parent)
            data = xmlNewChild (parent, NULL, BAD_CAST name, NULL);
        else
            data = xmlNewNode (NULL, BAD_CAST name);
        gnode_sort_children (schema, node);
        for (GNode * child = node->children; child; child = child->next)
        {
            _sch_gnode_to_xml (instance, schema, data, child, flags, depth + 1);
        }
    }
    else if (APTERYX_HAS_VALUE (node))
    {
        DEBUG ("%*s%s = %s\n", depth * 2, " ", APTERYX_NAME (node), APTERYX_VALUE (node));
        data = xmlNewNode (NULL, BAD_CAST name);
        xmlNodeSetContent (data, (const xmlChar *) APTERYX_VALUE (node));
        if (parent)
            xmlAddChildList (parent, data);
    }

    return data;
}

xmlNode *
sch_gnode_to_xml (sch_instance * instance, sch_node * schema, GNode * node, int flags)
{
    if (node && g_node_n_children (node) > 1 && strlen (APTERYX_NAME (node)) == 1)
    {
        xmlNode *first = NULL;
        xmlNode *last = NULL;
        xmlNode *next;

        apteryx_sort_children (node, g_strcmp0);
        for (GNode * child = node->children; child; child = child->next)
        {
            next = _sch_gnode_to_xml (instance, schema, NULL, child, flags, 1);
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
        return _sch_gnode_to_xml (instance, schema, NULL, node, flags, 0);
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
_sch_xml_to_gnode (sch_instance * instance, sch_node * schema, GNode * parent,
                   xmlNode * xml, int flags, int depth)
{
    const char *name = (const char *) xml->name;
    xmlNode *child;
    char *attr;
    GNode *tree = NULL;
    GNode *node = NULL;
    char *key = NULL;

    /* Find schema node */
    if (!schema)
        schema = sch_lookup (instance, name);
    else
        schema = sch_node_child (schema, name);
    if (schema == NULL)
    {
        DEBUG ("ERROR: No schema match for xml node %s\n", name);
        return NULL;
    }

    /* LIST */
    if (sch_is_list (schema))
    {
        key = sch_name (sch_node_child_first (sch_node_child_first (schema)));
        DEBUG ("%*s%s%s\n", depth * 2, " ", depth ? "" : "/", name);
        depth++;
        tree = node = APTERYX_NODE (NULL, g_strdup (name));
        attr = (char *) xmlGetProp (xml, BAD_CAST key);
        if (attr)
        {
            node = APTERYX_NODE (node, attr);
            DEBUG ("%*s%s\n", depth * 2, " ", APTERYX_NAME (node));
            if (!(flags && SCH_F_STRIP_KEY) || xmlFirstElementChild (xml))
            {
                APTERYX_NODE (node, g_strdup (key));
                DEBUG ("%*s%s\n", (depth + 1) * 2, " ", key);
            }
        }
        else if (xmlFirstElementChild (xml) &&
                 g_strcmp0 ((const char *) xmlFirstElementChild (xml)->name, key) == 0 &&
                 xml_node_has_content (xmlFirstElementChild (xml)))
        {
            node =
                APTERYX_NODE (node,
                              (char *) xmlNodeGetContent (xmlFirstElementChild (xml)));
            DEBUG ("%*s%s\n", depth * 2, " ", APTERYX_NAME (node));
        }
        else
        {
            node = APTERYX_NODE (node, g_strdup ("*"));
            DEBUG ("%*s%s\n", depth * 2, " ", APTERYX_NAME (node));
        }
        schema = sch_node_child_first (schema);
    }
    /* CONTAINER */
    else if (!sch_is_leaf (schema))
    {
        DEBUG ("%*s%s%s\n", depth * 2, " ", depth ? "" : "/", name);
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
            DEBUG ("%*s%s = NULL\n", depth * 2, " ", name);
        }
        else if (xml_node_has_content (xml))
        {
            node = APTERYX_NODE (tree, (char *) xmlNodeGetContent (xml));
            DEBUG ("%*s%s = %s\n", depth * 2, " ", name, APTERYX_NAME (node));
        }
        else
        {
            DEBUG ("%*s%s%s\n", depth * 2, " ", depth ? "" : "/", name);
        }
        free (attr);
    }

    for (child = xmlFirstElementChild (xml); child; child = xmlNextElementSibling (child))
    {
        if (flags && SCH_F_STRIP_KEY && key &&
            g_strcmp0 ((const char *) child->name, key) == 0)
        {
            /* The only child is the key with value - GET expects /list/<key-value> with wildcard */
            if (xmlChildElementCount (xml) == 1 && xml_node_has_content (child))
            {
                APTERYX_NODE (node, g_strdup ("*"));
                DEBUG ("%*s%s\n", (depth + 1) * 2, " ", "*");
                break;
            }
            /* Multiple children - but the key has a value - GET expects no value */
            else if (xmlChildElementCount (xml) > 1 && xml_node_has_content (child))
            {
                APTERYX_NODE (node, g_strdup ((const char *) child->name));
                DEBUG ("%*s%s\n", depth * 2, " ", APTERYX_NAME (node));
            }
        }
        GNode *cn = _sch_xml_to_gnode (instance, schema, NULL, child, flags, depth + 1);
        if (!cn)
        {
            apteryx_free_tree (tree);
            return NULL;
        }
        g_node_append (node, cn);
    }

    /* Get everything from here down if a trunk of a subtree */
    if (!xmlFirstElementChild (xml) && sch_node_child_first (schema) &&
        g_strcmp0 (APTERYX_NAME (node), "*") != 0)
    {
        APTERYX_NODE (node, g_strdup ("*"));
        DEBUG ("%*s%s\n", (depth + 1) * 2, " ", "*");
    }

    free (key);
    return tree;
}

GNode *
sch_xml_to_gnode (sch_instance * instance, sch_node * schema, xmlNode * xml, int flags)
{
    return _sch_xml_to_gnode (instance, schema, NULL, xml, flags, 0);
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
_sch_gnode_to_json (sch_instance * instance, sch_node * schema, GNode * node, int flags, int depth)
{
    json_t *data = NULL;
    char *name;

    /* Get the actual node name */
    if (depth == 0 && strlen (APTERYX_NAME (node)) == 1)
    {
        return _sch_gnode_to_json (instance, schema, node->children, flags, depth);
    }
    else if (depth == 0 && APTERYX_NAME (node)[0] == '/')
    {
        name = APTERYX_NAME (node) + 1;
    }
    else
    {
        name = APTERYX_NAME (node);
    }

    /* Find schema node */
    if (!schema)
        schema = sch_lookup (instance, name);
    else
        schema = sch_node_child (schema, name);
    if (schema == NULL)
    {
        DEBUG ("ERROR: No schema match for gnode %s\n", name);
        return NULL;
    }
    if (!sch_is_readable (schema))
    {
        DEBUG ("REST: Ignoring non-readable node %s\n", name);
        return NULL;
    }

    if (sch_is_list (schema) && (flags & SCH_F_JSON_ARRAYS))
    {
        data = json_array ();
        apteryx_sort_children (node, g_strcmp0);
        for (GNode * child = node->children; child; child = child->next)
        {
            DEBUG ("%*s%s[%s]\n", depth * 2, " ", APTERYX_NAME (node),
                   APTERYX_NAME (child));
            json_t *obj = json_object();
            gnode_sort_children (sch_node_child_first (schema), child);
            for (GNode * field = child->children; field; field = field->next)
            {
                json_t *node = _sch_gnode_to_json (instance, sch_node_child_first (schema), field, flags, depth + 1);
                json_object_set_new (obj, APTERYX_NAME (field), node);
            }
            json_array_append_new (data, obj);
        }
    }
    else if (!sch_is_leaf (schema))
    {
        DEBUG ("%*s%s\n", depth * 2, " ", APTERYX_NAME (node));
        data = json_object();
        gnode_sort_children (schema, node);
        for (GNode * child = node->children; child; child = child->next)
        {
            json_t *node = _sch_gnode_to_json (instance, schema, child, flags, depth + 1);
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
        char *value = strdup (APTERYX_VALUE (node));
        if (flags & SCH_F_JSON_TYPES)
        {
            value = sch_translate_to (schema, value);
            data = encode_json_type (value);
        }
        else
            data = json_string (value);
        DEBUG ("%*s%s = %s\n", depth * 2, " ", APTERYX_NAME (node), value);
        free (value);
    }

    return data;
}

json_t *
sch_gnode_to_json (sch_instance * instance, sch_node * schema, GNode * node, int flags)
{
    json_t *json = NULL;
    json_t *child;

    if (schema)
    {
        schema = ((xmlNode *)schema)->parent;
        child = _sch_gnode_to_json (instance, schema, node, flags, g_node_max_height (node));
        if (child)
        {
            json = json_object ();
            json_object_set_new (json, APTERYX_NAME (node), child);
        }
    }
    else
        json = _sch_gnode_to_json (instance, schema, node, flags, 0);
    return json;
}
