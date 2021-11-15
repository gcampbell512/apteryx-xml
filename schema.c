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
#include <apteryx.h>
#include "apteryx-xml.h"

#define DEBUG(fmt, args...) 
// #define DEBUG(fmt, args...)  printf (fmt, ## args);

/* List full paths for all XML files in the search path */
static void
list_xml_files (GList **files, const char *path)
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
merge_nodes (xmlNode *orig, xmlNode *new, int depth)
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
cleanup_nodes (xmlNode *node)
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
sch_free (sch_instance *schema)
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
    } while (c1 == c2);
    return false;
}

static xmlNode *
lookup_node (xmlNode *node, const char *path)
{
    xmlNode *n;
    char *name, *mode;
    char *key = NULL;
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
                    return lookup_node (xmlDocGetRootElement (node->doc), path);
                }
                xmlFree (name);
                if (mode)
                {
                    xmlFree (mode);
                }
                return lookup_node (n, path);
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
sch_lookup (sch_instance *schema, const char *path)
{
    return lookup_node ((xmlNode *) schema, path);
}

sch_node *
sch_node_child (sch_node *parent, const char *child)
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

sch_node*
sch_node_child_next (sch_node *parent, sch_node *node)
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

sch_node*
sch_node_child_first (sch_node *parent)
{
    return sch_node_child_next (parent, NULL);
}

bool
sch_is_list (sch_node *node)
{
    xmlNode *xml = (xmlNode *) node;
    xmlNode *child = xml->children;

    if (child && !child->next && child->type == XML_ELEMENT_NODE && child->name[0] == 'N')
    {
        char *name = (char *) xmlGetProp (child, (xmlChar *) "name");
        if (name && g_strcmp0 (name ,"*") == 0) {
            xmlFree (name);
            return true;
        }
        if (name)
            xmlFree (name);
    }
    return false;
}

bool
sch_is_leaf (sch_node *node)
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
sch_is_readable (sch_node *node)
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
sch_is_writable (sch_node *node)
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
sch_is_config (sch_node *node)
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
sch_translate_to (sch_node *node, char *value)
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
sch_translate_from (sch_node *node, char *value)
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

char*
sch_name (sch_node *node)
{
    return (char *) xmlGetProp (node, (xmlChar *) "name");
}

/* Data translation/manipulation */

static int
get_index (GNode *node, sch_node *schema)
{
    int index = 0;
    sch_node *n;
    for (n = sch_node_child_first (schema); n; index++, n = sch_node_child_next (schema, n))
        if (g_strcmp0(sch_name (n), (char *) node->data) == 0)
            break;
    return index;
}

static GNode *
merge (GNode *left, GNode *right, sch_node *schema)
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
split (GNode *head)
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
merge_sort (GNode *head, sch_node *schema)
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
gnode_sort_children (sch_node *schema, GNode *parent)
{
    if (parent)
        parent->children = merge_sort (parent->children, schema);
}

xmlNode* 
sch_gnode_to_xml (sch_instance *instance, sch_node *schema, xmlNode *parent, GNode *node, int depth)
{
    xmlNode *data = NULL;
    char *name;

    /* Get the actual node name */
    if (depth == 0 && strlen (APTERYX_NAME (node)) == 1) {
        return sch_gnode_to_xml (instance, schema, parent, node->children, depth);
    } else if (depth == 0 && APTERYX_NAME( node)[0] == '/') {
        name = APTERYX_NAME (node) + 1;
    } else {
        name = APTERYX_NAME (node);
    }

    /* Find schema node */
    if (!schema)
        schema = sch_lookup (instance, name);
    else
        schema = sch_node_child (schema, name);
    if (schema == NULL) {
        fprintf (stderr, "ERROR: No match for %s\n", name);
        return NULL;
    }

    if (sch_is_list (schema)) {
        apteryx_sort_children (node, g_strcmp0);
        for (GNode *child = node->children; child; child = child->next) {
            DEBUG ("%*s%s[%s]\n", depth * 2, " ", APTERYX_NAME(node), APTERYX_NAME(child));
            data = xmlNewNode (NULL, BAD_CAST name);
            gnode_sort_children (sch_node_child_first (schema), child);
            for (GNode *field = child->children; field; field = field->next) {
                sch_gnode_to_xml (instance, sch_node_child_first (schema), data, field, depth + 1);
            }
            xmlAddChildList (parent, data);
        }
    }
    else if (!sch_is_leaf (schema)) {
        DEBUG ("%*s%s\n", depth * 2, " ", APTERYX_NAME(node));
        if (parent)
            data = xmlNewChild (parent, NULL, BAD_CAST name, NULL);
        else
            data = xmlNewNode (NULL, BAD_CAST name);
        gnode_sort_children (schema, node);
        for (GNode *child = node->children; child; child = child->next) {
            sch_gnode_to_xml (instance, schema, data, child, depth + 1);
        }
    }
    else if (APTERYX_HAS_VALUE (node)) {
        DEBUG ("%*s%s = %s\n", depth * 2, " ", APTERYX_NAME(node), APTERYX_VALUE(node));
        data = xmlNewNode (NULL, BAD_CAST name);
        xmlNodeSetContent (data, (const xmlChar *)APTERYX_VALUE(node));
        if (parent)
            xmlAddChildList (parent, data);
    } 

    return data;
}

GNode *
sch_xml_to_gnode (sch_instance *instance, sch_node *schema, GNode *parent, xmlNode *xml, int depth)
{
    const char *name = (const char *) xml->name;
    xmlNode *child;
    const char *attr;
    GNode *tree = NULL;
    GNode *node = NULL;

    /* Find schema node */
    if (!schema)
        schema = sch_lookup (instance, name);
    else
        schema = sch_node_child (schema, name);
    if (schema == NULL) {
        fprintf (stderr, "ERROR: No match for %s\n", name);
        return NULL;
    }

    /* LIST */
    if (sch_is_list (schema)) {
        char *key = sch_name (sch_node_child_first (sch_node_child_first (schema)));
        DEBUG ("%*s%s%s\n", depth * 2, " ", depth ? "" : "/", name);
        depth++;
        tree = node = APTERYX_NODE (NULL, g_strdup (name));
        if (xmlFirstElementChild (xml) && g_strcmp0 ((const char *)xmlFirstElementChild (xml)->name, key) == 0 &&
            strlen ((char *) xmlNodeGetContent (xmlFirstElementChild (xml))) > 0) {
            node = APTERYX_NODE (node, g_strdup ((char *) xmlNodeGetContent (xmlFirstElementChild (xml))));
        }
        else {
            node = APTERYX_NODE (node, g_strdup ("*"));
        }
        DEBUG ("%*s%s\n", depth * 2, " ", APTERYX_NAME(node));
        schema = sch_node_child_first (schema);
    }
    /* CONTAINER */
    else if (!sch_is_leaf (schema)) {
        DEBUG ("%*s%s%s\n", depth * 2, " ", depth ? "" : "/", name);
        tree = node = APTERYX_NODE (NULL, g_strdup_printf ("%s%s", depth ? "" : "/", name));
    }
    /* LEAF */
    else {
        tree = node = APTERYX_NODE (NULL, g_strdup (name));
        attr = (const char *) xmlGetProp (xml, BAD_CAST "operation");
        if (g_strcmp0 (attr, "delete") == 0) {
            APTERYX_NODE (tree, g_strdup (""));
            DEBUG ("%*s%s = NULL\n", depth * 2, " ", name);
        }
        else if (strlen ((char *) xmlNodeGetContent (xml)) != 0) {
            APTERYX_NODE (tree, g_strdup ((char *)xmlNodeGetContent (xml)));
            DEBUG ("%*s%s = %s\n", depth * 2, " ", name, (char *)xmlNodeGetContent (xml));
        }
        else {
            DEBUG ("%*s%s%s\n", depth * 2, " ", depth ? "" : "/", name);
        }
    }

    //TODO - the only child is the key with value - GET expects /list/<key-value>/*
    //TODO - multiple children and one of them is the key with value - GET expects the value to be dropped */
    for (child = xmlFirstElementChild (xml); child; child = xmlNextElementSibling (child)) {
        GNode *cn = sch_xml_to_gnode (instance, schema, NULL, child, depth + 1);
        if (!cn) {
            fprintf (stderr, "ERROR: No child match for %s\n", sch_name (sch_node_child_first (schema)));
            apteryx_free_tree (node);
            return NULL;
        }
        g_node_append (node, cn);
    }

    /* Get everything from here down if a trunk of a subtree */
    if (!xmlFirstElementChild (xml) && sch_node_child_first (schema) && g_strcmp0 (APTERYX_NAME (node), "*") != 0) {
        APTERYX_NODE (node, g_strdup ("*"));
        DEBUG ("%*s%s\n", (depth + 1) * 2, " ", "*");
    }
    return tree;
}
