/**
 * @file sch_condition.c
 * Process XML "if-feature", "when" or "must" conditions
 *
 * Copyright 2024, Allied Telesis Labs New Zealand, Ltd
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

#include "sch_xpath.h"
#include <apteryx.h>
#include <apteryx-xml.h>
#include <libxml/tree.h>
#include <inttypes.h>
#define APTERYX_XML_LIBXML2

typedef struct _cond_result
{
    bool result;
    char *path;
    char *value;
    char *step_value;  /* actual value of step if step_exists has been called */
    sch_instance *instance;
} cond_result;

typedef enum
{
    PROC_F_IF_FEATURE = (1 << 0),   /* Parent is a if-feature */
    PROC_F_FIRST_CHILD = (1 << 1),  /* First child seen in step */
} proc_flags;


static void sch_process_xnode (sch_instance *instance, GNode *root, char *path,
                               char *step_path, xpath_node *xnode,
                               cond_result *presult, int depth, int *flags);


static void
sch_axis_child (sch_instance *instance, char *path, char *step_path, char *child_name,
                char *child_prefix, cond_result *presult, int *flags)
{
    char *_path;

    if (child_name)
    {
        if ((*flags & PROC_F_IF_FEATURE))
            presult->value = g_strdup (child_name);
        else
        {
            _path = g_strdup (step_path ? : path);
            if ((*flags & PROC_F_FIRST_CHILD) == 0 && g_strcmp0 (path, step_path) == 0)
            {
                sch_node *s_node = sch_lookup (instance, path);
                if (s_node)
                {
                    sch_node *p_node = sch_node_parent (s_node);
                    if (p_node && !sch_is_leaf_list (p_node) &&
                        !sch_is_list (p_node) && !sch_is_leaf (p_node))
                    {
                        char *ptr = strrchr (_path, '/');
                        if (ptr)
                            *ptr = '\0';
                        *flags |= PROC_F_FIRST_CHILD;
                    }
                }
            }
            presult->value = g_strdup_printf ("%s/%s", _path, child_name);
            g_free (_path);
        }
        presult->result = true;
    }
    else
    {
        presult->result = false;
    }
}

static void
sch_axis_parent (char *path, char *step_path, cond_result *presult, bool self, int flags)
{
    char *ptr;
    char *_path;

    presult->result = false;
    if (self && step_path && g_strcmp0 (path, step_path) != 0)
    {
        presult->result = true;
        presult->value = g_strdup (step_path);
        return;
    }

    /* Remove the last path directive to move to a parent path */
    _path = g_strdup (step_path ? : path);
    ptr = strrchr (_path, '/');
    if (ptr)
    {
        *ptr = '\0';
        presult->value = _path;
        presult->result = true;
    }
    else
        g_free (_path);
}

static gboolean
sch_step_exists_traverse_nodes (GNode *node, gpointer data)
{
    cond_result *exists = (cond_result *) data;
    bool ret = false;
    char *path = apteryx_node_path (node);

    if (g_strcmp0 (path, exists->path) == 0)
    {
        exists->result = true;
        if (APTERYX_HAS_VALUE (node))
        {
            exists->value = g_strdup (APTERYX_VALUE (node));
            sch_node *s_node = sch_lookup (exists->instance, path);
            if (s_node)
            {
                /* We now have the schema node for the value, see if any translation is needed */
                exists->value = sch_translate_from (s_node, exists->value);
            }
        }

        /* Returning true stops the tree traverse */
        ret = true;
    }

    g_free (path);
    return ret;
}

static void
sch_step_exists (sch_instance *instance, GNode *root, char *path, cond_result *presult)
{
    cond_result exists = { };

    exists.path = path;
    exists.instance = instance;
    g_node_traverse (root, G_IN_ORDER, G_TRAVERSE_ALL, -1,
                     sch_step_exists_traverse_nodes, &exists);
    presult->result = exists.result;
    presult->value = exists.value;
    if (!presult->result)
    {
        GNode *tree = apteryx_get_tree (path);

        if (tree)
        {
            presult->result = true;
            if (APTERYX_HAS_VALUE (tree))
            {
                presult->value = g_strdup (APTERYX_VALUE (tree));
            }
            apteryx_free_tree (tree);
        }
    }
}

static void
sch_process_operator (sch_instance *instance, GNode *root, char *path,
                      char *step_path, xpath_node *xnode, cond_result *presult,
                      int depth, int *flags)
{
    char *new_step_path = NULL;
    bool is_number = false;
    bool error = false;
    cond_result lresult = { };
    cond_result rresult = { };
    int64_t lnumber = 0;
    int64_t rnumber = 0;

    presult->result = false;
    if (xnode->left)
        sch_process_xnode (instance, root, path, step_path, xnode->left, &lresult,
                           depth + 1, flags);
    if (xnode->right)
    {
        sch_process_xnode (instance, root, path, step_path, xnode->right, &rresult,
                           depth + 1, flags);
        if (xnode->right->type == XPATH_TYPE_NUMBER)
            is_number = true;
        else if (rresult.result && rresult.value)
        {
            char *ptr = strchr (rresult.value, ':');
            /* Prune any type information from the value */
            if (ptr)
            {
                char *temp = rresult.value;

                rresult.value = g_strdup (ptr + 1);
                g_free (temp);
            }
        }
    }

    if (lresult.result)
    {
        if (xnode->left->type == XPATH_TYPE_FUNCTION &&
            (g_strcmp0 (xnode->left->name, "current") == 0 ||
             g_strcmp0 (xnode->left->name, "boolean") == 0))
        {
            new_step_path = lresult.value;
            lresult.value = apteryx_get_string (new_step_path, NULL);
            g_free (new_step_path);
        }
        else if (xnode->left->type == XPATH_TYPE_STEP)
        {
            /* We already looked up step value and saved it. */
            if (rresult.result && rresult.value)
            {
                /* The left node retains the path to the node with the value */
                sch_node *s_node = sch_lookup (instance, lresult.value);
                if (s_node)
                {
                    /* We now have the schema node for the value, see if any translation is needed */
                    rresult.value = sch_translate_from (s_node, rresult.value);
                }
            }
            g_free (lresult.value);
            lresult.value = lresult.step_value;
            lresult.step_value = NULL;
        }
    }

    if (is_number)
    {
        if (xnode->left->type != XPATH_TYPE_STEP)
        {
            if (lresult.value)
                lnumber = strtoll (lresult.value, NULL, 10);

            if (rresult.value)
                rnumber = strtoll (rresult.value, NULL, 10);
        }
        else
        {
            error = true;
        }
    }

    if (!error)
    {
        switch (xnode->type)
        {
        case XPATH_TYPE_OR:
            presult->result = lresult.result || rresult.result;
            break;
        case XPATH_TYPE_AND:
            presult->result = lresult.result && rresult.result;
            break;
        case XPATH_TYPE_EQ:
            if (is_number)
                presult->result = lnumber == rnumber;
            else if (g_strcmp0 (lresult.value, rresult.value) == 0)
                presult->result = true;
            break;
        case XPATH_TYPE_NE:
            if (is_number)
                presult->result = lnumber != rnumber;
            else if (g_strcmp0 (lresult.value, rresult.value) != 0)
                presult->result = true;
            break;
        case XPATH_TYPE_LT:
            if (is_number)
                presult->result = lnumber < rnumber;
            break;
        case XPATH_TYPE_LE:
            if (is_number)
                presult->result = lnumber <= rnumber;
            break;
        case XPATH_TYPE_GT:
            if (is_number)
                presult->result = lnumber > rnumber;
            break;
        case XPATH_TYPE_GE:
            if (is_number)
                presult->result = lnumber >= rnumber;
            break;
        case XPATH_TYPE_PLUS:
            if (is_number)
            {
                presult->value = g_strdup_printf ("%" PRId64 "", lnumber + rnumber);
                presult->result = true;
            }
            break;
        case XPATH_TYPE_MINUS:
            if (is_number)
            {
                presult->value = g_strdup_printf ("%" PRId64 "", lnumber - rnumber);
                presult->result = true;
            }
            break;
        case XPATH_TYPE_MULTIPLY:
            if (is_number)
            {
                presult->value = g_strdup_printf ("%" PRId64 "", lnumber * rnumber);
                presult->result = true;
            }
            break;
        case XPATH_TYPE_DIVIDE:
            if (is_number && rnumber)
            {
                presult->value = g_strdup_printf ("%" PRId64 "", lnumber / rnumber);
                presult->result = true;
            }
            break;
        case XPATH_TYPE_MODULO:
            if (is_number && rnumber)
            {
                presult->value = g_strdup_printf ("%" PRId64 "", lnumber % rnumber);
                presult->result = true;
            }
            break;
        case XPATH_TYPE_UNARY_MINUS:
            if (is_number)
            {
                presult->value = g_strdup_printf ("-%" PRId64 "", lnumber);
                presult->result = true;
            }
            break;
        default:
            presult->result = false;
            break;
        }
    }
    g_free (lresult.value);
    g_free (rresult.value);
}

static void
sch_process_predicate (sch_instance *instance, GNode *root, char *path,
                       char *step_path, xpath_node *xnode, cond_result *presult,
                       int depth, int *flags)
{
    cond_result lresult = { };
    cond_result rresult = { };

    presult->result = false;
    if (xnode->left)
    {
        sch_process_xnode (instance, root, path, step_path, xnode->left, &lresult,
                           depth + 1, flags);
        presult->result = lresult.result;
        if (lresult.result && xnode->right)
        {
            sch_process_xnode (instance, root, path, lresult.value, xnode->right,
                               &rresult, depth + 1, flags);
            presult->result = rresult.result;
        }
    }
    if (rresult.result)
    {
        presult->value = rresult.value;
        g_free (lresult.value);
    }
    else if (lresult.result)
    {
        presult->value = lresult.value;
        g_free (rresult.value);
    }

}

static void
sch_derived_from_process (sch_node *s_node, xmlChar *name, char *key, char *cmp_value,
                          cond_result *presult)
{
    xmlChar *idref;
    char *test_value;

    idref = xmlGetProp ((xmlNode *) s_node, BAD_CAST key);
    if (idref)
    {
        test_value = g_strdup_printf ("%s:%s", (char *) idref, name);
        if (g_strcmp0 (test_value, cmp_value) == 0)
            presult->result = true;

        g_free (test_value);
        xmlFree (idref);
    }
}

static void
sch_function_derived_from (sch_instance *instance, GNode *root, char *path,
                           char *step_path, xpath_node *xnode, cond_result *presult,
                           int depth, bool or_self, int *flags)
{
    xmlChar *name;
    GList *iter;
    xpath_node *node;
    sch_node *s_node = NULL;
    cond_result result = { };

    /* The list should have 2 members */
    presult->result = false;

    if (g_list_length (xnode->arg_list) != 2)
        return;

    iter = g_list_first (xnode->arg_list);
    if (iter)
    {
        node = (xpath_node *) iter->data;
        sch_process_xnode (instance, root, path, step_path, node, &result,
                           depth + 1, flags);
        if (result.result)
            s_node = sch_lookup (instance, result.value);

        g_free (result.value);
        result.value = NULL;
        iter = iter->next;
        if (result.result && iter)
        {
            node = (xpath_node *) iter->data;
            sch_process_xnode (instance, root, path, step_path, node, &result,
                               depth + 1, flags);
            if (!result.result)
            {
                g_free (result.value);
                g_free (result.step_value);
                return;
            }
        }
    }
    else
        return;

    if (!s_node)
    {
        g_free (result.value);
        g_free (result.step_value);
        return;
    }

    name = xmlGetProp ((xmlNode *) s_node, BAD_CAST "name");
    sch_derived_from_process (s_node, name, "idref_prefix", result.value, presult);
    if (or_self && !presult->result)
        sch_derived_from_process (s_node, name, "idref_self", result.value, presult);

    xmlFree (name);
    g_free (result.value);
    g_free (result.step_value);
}

static void
sch_process_arg_list (sch_instance *instance, GNode *root, char *path,
                      char *step_path, xpath_node *xnode, cond_result *presult,
                      int depth, int *flags)
{
    GList *iter;
    xpath_node *lnode;
    cond_result result = { };
    bool first = true;

    presult->result = false;
    for (iter = g_list_first (xnode->arg_list); iter; iter = g_list_next (iter))
    {
        lnode = (xpath_node *) iter->data;
        sch_process_xnode (instance, root, path, step_path, lnode, &result,
                           depth + 1, flags);
        if (first)
        {
            presult->result = result.result;
            first = false;
        }
        else
        {
            presult->result |= result.result;
        }

        g_free (result.value);
        g_free (result.step_value);
    }
}

static void
sch_function_name (sch_instance *instance, GNode *root, char *path,
                   char *step_path, xpath_node *xnode, cond_result *presult,
                   int depth, int *flags)
{
    GList *iter;
    xpath_node *node;
    char *ptr;
    cond_result result = { };

    presult->result = false;
    if (g_list_length (xnode->arg_list) != 1)
        return;

    iter = g_list_first (xnode->arg_list);
    if (iter)
    {
        node = (xpath_node *) iter->data;
        sch_process_xnode (instance, root, path, step_path, node, &result,
                           depth + 1, flags);
        if (result.result)
        {
            ptr = strrchr (result.value, '/');
            if (ptr)
            {
                presult->value = g_strdup (ptr + 1);
                presult->result = result.result;
            }
        }
        g_free (result.value);
        g_free (result.step_value);
    }
}

static void
sch_process_if_feature (sch_instance *instance, GNode *root, char *path,
                        char *feature, cond_result *presult, int *flags)
{
    sch_node *s_node;
    xmlChar *features = NULL;

    presult->result = false;
    s_node = sch_lookup (instance, path);
    if (!s_node)
        return;

    while (s_node && !features)
    {
        features = xmlGetProp ((xmlNode *) s_node, BAD_CAST "features");
        s_node = sch_node_parent (s_node);
    }

    if (features && features[0] != '\0')
    {
        if (strstr ((char *) features, feature))
            presult->result = true;
    }
    xmlFree (features);
}

static void
sch_function_if_feature (sch_instance *instance, GNode *root, char *path,
                         char *step_path, xpath_node *xnode, cond_result *presult,
                         int depth, int *flags)
{
    GList *iter;
    xpath_node *node;
    cond_result result = { };

    presult->result = false;
    if (g_list_length (xnode->arg_list) != 1)
        return;

    iter = g_list_first (xnode->arg_list);
    if (iter)
    {
        node = (xpath_node *) iter->data;
        *flags |= PROC_F_IF_FEATURE;
        sch_process_xnode (instance, root, path, step_path, node, &result,
                           depth + 1, flags);
        *flags &= ~PROC_F_IF_FEATURE;
        presult->result = result.result;
        g_free (result.value);
        g_free (result.step_value);
    }
}

static void
sch_function_count (sch_instance *instance, GNode *root, char *path, char *step_path,
                    xpath_node *xnode, cond_result *presult, int depth, int *flags)
{
    GList *iter;
    xpath_node *lnode;
    GNode *tree;
    cond_result result = { };
    int match_count = 0;

    presult->result = false;
    iter = g_list_first (xnode->arg_list);
    if (!iter)
        return;

    lnode = (xpath_node *) iter->data;
    sch_process_xnode (instance, root, path, step_path, lnode, &result,
                       depth + 1, flags);
    if (result.value)
    {
        tree = apteryx_get_tree (result.value);
        if (tree)
        {
            match_count += g_node_n_children (tree);
            apteryx_free_tree (tree);
        }
        g_free (result.value);
        g_free (result.step_value);
        presult->value = g_strdup_printf ("%u", match_count);
        presult->result = true;
    }
}

static void
sch_process_xnode (sch_instance *instance, GNode *root, char *path, char *step_path,
                   xpath_node *xnode, cond_result *presult, int depth, int *flags)
{
    char *new_step_path = NULL;
    cond_result result = { };


    presult->result = false;
    if (!xnode)
        return;

    switch (xnode->type)
    {
    case XPATH_TYPE_FUNCTION:
        if (g_strcmp0 (xnode->name, "not") == 0)
        {
            sch_process_arg_list (instance, root, path, step_path, xnode, &result,
                                  depth, flags);
            presult->result = !result.result;
        }
        else if (g_strcmp0 (xnode->name, "count") == 0)
        {
            sch_function_count (instance, root, path, step_path, xnode, &result,
                                depth, flags);
            presult->result = result.result;
            if (result.result)
            {
                presult->value = result.value;
                result.value = NULL;
            }
        }
        else if (g_strcmp0 (xnode->name, "derived-from") == 0)
        {
            sch_function_derived_from (instance, root, path, step_path, xnode,
                                       &result, depth, false, flags);
            presult->result = result.result;
        }
        else if (g_strcmp0 (xnode->name, "derived-from-or-self") == 0)
        {
            sch_function_derived_from (instance, root, path, step_path, xnode,
                                       &result, depth, true, flags);
            presult->result = result.result;
        }
        else if (g_strcmp0 (xnode->name, "current") == 0)
        {
            presult->value = g_strdup (path);
            presult->result = true;
        }
        else if (g_strcmp0 (xnode->name, "name") == 0)
        {
            sch_function_name (instance, root, path, step_path, xnode, &result,
                               depth, flags);
            *presult = result;
            result.value = NULL;
        }
        else if (g_strcmp0 (xnode->name, "if-feature") == 0)
        {
            sch_function_if_feature (instance, root, path, step_path, xnode,
                                     &result, depth, flags);
            *presult = result;
            result.value = NULL;
        }
        else if (g_strcmp0 (xnode->name, "boolean") == 0)
        {
            sch_process_arg_list (instance, root, path, step_path, xnode, &result,
                                  depth, flags);
            presult->result = result.result;
        }
        break;
    case XPATH_TYPE_STEP:
        if (xnode->left)
        {
            new_step_path = g_strdup (step_path ? : path);
            sch_process_xnode (instance, root, path, new_step_path, xnode->left,
                               &result, depth + 1, flags);
            presult->result = result.result;
            if (result.result)
            {
                presult->value = result.value;
                result.value = NULL;
                if (presult->value)
                {
                    g_free (new_step_path);
                    new_step_path = g_strdup (presult->value);
                }
            }
            else
            {
                g_free (result.value);
                result.value = NULL;
            }

            if (xnode->right)
            {
                sch_process_xnode (instance, root, path, new_step_path, xnode->right,
                                   &result, depth + 1, flags);
                presult->result = presult->result & result.result;
                if (result.result)
                {
                    g_free (presult->value);
                    presult->value = result.value;
                    result.value = NULL;
                }
                else
                {
                    g_free (result.value);
                    result.value = NULL;
                }
            }
            g_free (new_step_path);
        }

        if (presult->result)
        {
            /* This tests if a path exists, and gets its value */
            sch_step_exists (instance, root, presult->value, &result);
            presult->result = result.result;
            g_free (presult->step_value);
            presult->step_value = result.value;
            result.value = NULL;
        }
        *flags &= ~PROC_F_FIRST_CHILD;
        break;
    case XPATH_TYPE_STRING:
        presult->value = g_strdup (xnode->string_value);
        presult->result = true;
        break;
    case XPATH_TYPE_NUMBER:
        presult->value = g_strdup (xnode->number);
        presult->result = true;
        break;
    case XPATH_TYPE_OR:
    case XPATH_TYPE_AND:
    case XPATH_TYPE_EQ:
    case XPATH_TYPE_NE:
    case XPATH_TYPE_LT:
    case XPATH_TYPE_LE:
    case XPATH_TYPE_GT:
    case XPATH_TYPE_GE:
    case XPATH_TYPE_PLUS:
    case XPATH_TYPE_MINUS:
    case XPATH_TYPE_MULTIPLY:
    case XPATH_TYPE_DIVIDE:
    case XPATH_TYPE_MODULO:
    case XPATH_TYPE_UNARY_MINUS:
        sch_process_operator (instance, root, path, step_path, xnode, &result,
                              depth, flags);
        presult->result = result.result;
        break;
    case XPATH_TYPE_UNION:
        break;

    case XPATH_TYPE_ANCESTOR:
    case XPATH_TYPE_ANCESTOR_OR_SELF:
    case XPATH_TYPE_ATTRIBUTE:
        break;
    case XPATH_TYPE_CHILD:
        sch_axis_child (instance, path, step_path, xnode->name, xnode->prefix, &result, flags);
        if (result.result && (*flags & PROC_F_IF_FEATURE))
        {
            char *feature = result.value;
            result.value = NULL;
            sch_process_if_feature (instance, root, path, feature, &result, flags);
            g_free (feature);
        }
        *presult = result;
        result.value = NULL;
        break;
    case XPATH_TYPE_DESCENDANT:
    case XPATH_TYPE_DESCENDANT_OR_SELF:
    case XPATH_TYPE_FOLLOWING:
    case XPATH_TYPE_FOLLOWING_SIBLING:
    case XPATH_TYPE_NAMESPACE:
        break;
    case XPATH_TYPE_PARENT:
    case XPATH_TYPE_SELF:
        sch_axis_parent (path, step_path, &result, xnode->type == XPATH_TYPE_SELF, *flags);
        *presult = result;
        result.value = NULL;
        break;
    case XPATH_TYPE_PRECEDING:
    case XPATH_TYPE_PRECEDING_SIBLING:
        break;
    case XPATH_TYPE_ROOT:
        break;

    case XPATH_TYPE_PREDICATE:
        sch_process_predicate (instance, root, path, step_path, xnode, &result,
                               depth + 1, flags);
        *presult = result;
        result.value = NULL;
        break;
    case XPATH_TYPE_VARIABLE:
        break;
    default:
        break;
    }

    g_free (result.value);
}

static void
sch_build_start_build (void)
{
}

static xpath_node *
sch_build_end_build (xpath_node *result)
{
    return result;
}

static xpath_node *
sch_build_string (char *value)
{
    xpath_node *node = sch_xpath_allocate_node ();

    node->type = XPATH_TYPE_STRING;
    node->string_value = g_strdup (value);

    return node;
}

static xpath_node *
sch_build_number (char *value)
{
    xpath_node *node = sch_xpath_allocate_node ();

    node->type = XPATH_TYPE_NUMBER;
    node->number = g_strdup (value);

    return node;
}

static xpath_node *
sch_build_operator (xpath_operator op, xpath_node *left, xpath_node *right)
{
    xpath_node *node = sch_xpath_allocate_node ();
    if (op == XPATH_OPERATOR_UNARY_MINUS)
    {
        node->type = XPATH_TYPE_NEGATE;
        node->op = op;
        node->left = left;
        return node;
    }
    node->type = sch_xpath_op_to_type (op);
    node->op = op;
    node->left = left;
    node->right = right;

    return node;
}

static xpath_node *
sch_build_axis (xpath_axis x_axis, xpath_node_type node_type, char *prefix, char *name)
{
    xpath_node *node = sch_xpath_allocate_node ();
    node->type = sch_xpath_axis_to_type (x_axis);
    node->node_type = sch_xpath_node_type_string (node_type);
    node->prefix = prefix;
    node->name = name;

    return node;
}

static xpath_node *
sch_build_join_step (xpath_node *left, xpath_node *right)
{
    xpath_node *node = sch_xpath_allocate_node ();
    node->type = XPATH_TYPE_STEP;
    node->left = left;
    node->right = right;

    return node;
}

static xpath_node *
sch_build_predicate (xpath_node *left, xpath_node *condition, bool reverse_step)
{
    xpath_node *node = sch_xpath_allocate_node ();
    node->type = XPATH_TYPE_PREDICATE;
    node->left = left;
    node->right = condition;
    node->reverse_step = reverse_step;

    return node;
}

static xpath_node *
sch_build_variable (char *prefix, char *name)
{
    xpath_node *node = sch_xpath_allocate_node ();
    node->type = XPATH_TYPE_VARIABLE;
    node->prefix = prefix;
    node->name = name;

    return node;
}

static xpath_node *
sch_build_function (char *prefix, char *name, GList *args)
{
    xpath_node *node = sch_xpath_allocate_node ();
    node->type = XPATH_TYPE_FUNCTION;
    node->prefix = prefix;
    node->name = name;
    node->arg_list = args;

    return node;
}

/**
 * Process a path and a "if-feature", "when" or "must" YANG condition for validity
 *
 * @param root the edit tree
 * @param path the path of the data to be checked.
 * @param condition the YANG condition to process in respect to the path.
 * @return true when the when processing completes and there are no issues
 * @return false when the condition processing completes and the condition is not valid
 */
bool
sch_process_condition (sch_instance *instance, GNode *root, char *path,
                       char *condition)
{
    xpath_node *xnode;
    cond_result result = { };
    int flags = 0;

    xnode = sch_xpath_parser (condition);
    sch_process_xnode (instance, root, path, NULL, xnode, &result, 0, &flags);
    g_free (result.value);
    g_free (result.step_value);
    sch_xpath_free_xnode_tree (xnode);

    return result.result;
}

static xpath_funcs bld_funcs = {
    .start_build = sch_build_start_build,
    .end_build = sch_build_end_build,
    .string = sch_build_string,
    .number = sch_build_number,
    .operator= sch_build_operator,
    .axis = sch_build_axis,
    .join_step = sch_build_join_step,
    .predicate = sch_build_predicate,
    .variable = sch_build_variable,
    .function = sch_build_function,
};

void
sch_condition_register (gboolean debug, gboolean verbose)
{
    sch_xpath_build_register (&bld_funcs, debug, verbose);
}
