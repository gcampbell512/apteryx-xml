/**
 * @file sch_xpath.h
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
#ifndef _SCH_XPATH_H_
#define _SCH_XPATH_H_
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <glib-unix.h>
#include <ctype.h>
#include <syslog.h>


typedef enum
{
    XPATH_AXIS_UNKNOWN,
    XPATH_AXIS_ANCESTOR,
    XPATH_AXIS_ANCESTOR_OR_SELF,
    XPATH_AXIS_ATTRIBUTE,
    XPATH_AXIS_CHILD,
    XPATH_AXIS_DESCENDANT,
    XPATH_AXIS_DESCENDANT_OR_SELF,
    XPATH_AXIS_FOLLOWING,
    XPATH_AXIS_FOLLOWING_SIBLING,
    XPATH_AXIS_NAMESPACE,
    XPATH_AXIS_PARENT,
    XPATH_AXIS_PRECEDING,
    XPATH_AXIS_PRECEDING_SIBLING,
    XPATH_AXIS_SELF,
    XPATH_AXIS_ROOT,
} xpath_axis;

typedef enum
{
    XPATH_OPERATOR_UNKNOWN,
    XPATH_OPERATOR_OR,
    XPATH_OPERATOR_AND,
    XPATH_OPERATOR_EQ,
    XPATH_OPERATOR_NE,
    XPATH_OPERATOR_LT,
    XPATH_OPERATOR_LE,
    XPATH_OPERATOR_GT,
    XPATH_OPERATOR_GE,
    XPATH_OPERATOR_PLUS,
    XPATH_OPERATOR_MINUS,
    XPATH_OPERATOR_MULTIPLY,
    XPATH_OPERATOR_DIVIDE,
    XPATH_OPERATOR_MODULO,
    XPATH_OPERATOR_UNARY_MINUS,
    XPATH_OPERATOR_UNION,
} xpath_operator;

typedef enum
{
    XPATH_NODE_TYPE_UNKNOWN,
    XPATH_NODE_TYPE_ALL,
    XPATH_NODE_TYPE_TEXT,
    XPATH_NODE_TYPE_INSTR,
    XPATH_NODE_TYPE_COMMENT,
    XPATH_NODE_TYPE_ATTRIBUTE,
    XPATH_NODE_TYPE_NAMESPACE,
} xpath_node_type;

typedef enum
{
    XPATH_TYPE_UNKNOWN,
    XPATH_TYPE_STRING,
    XPATH_TYPE_NUMBER,
    XPATH_TYPE_NEGATE,
    XPATH_TYPE_OR,
    XPATH_TYPE_AND,
    XPATH_TYPE_EQ,
    XPATH_TYPE_NE,
    XPATH_TYPE_LT,
    XPATH_TYPE_LE,
    XPATH_TYPE_GT,
    XPATH_TYPE_GE,
    XPATH_TYPE_PLUS,
    XPATH_TYPE_MINUS,
    XPATH_TYPE_MULTIPLY,
    XPATH_TYPE_DIVIDE,
    XPATH_TYPE_MODULO,
    XPATH_TYPE_UNARY_MINUS,
    XPATH_TYPE_UNION,
    XPATH_TYPE_ANCESTOR,
    XPATH_TYPE_ANCESTOR_OR_SELF,
    XPATH_TYPE_ATTRIBUTE,
    XPATH_TYPE_CHILD,
    XPATH_TYPE_DESCENDANT,
    XPATH_TYPE_DESCENDANT_OR_SELF,
    XPATH_TYPE_FOLLOWING,
    XPATH_TYPE_FOLLOWING_SIBLING,
    XPATH_TYPE_NAMESPACE,
    XPATH_TYPE_PARENT,
    XPATH_TYPE_PRECEDING,
    XPATH_TYPE_PRECEDING_SIBLING,
    XPATH_TYPE_SELF,
    XPATH_TYPE_ROOT,
    XPATH_TYPE_STEP,
    XPATH_TYPE_PREDICATE,
    XPATH_TYPE_VARIABLE,
    XPATH_TYPE_FUNCTION,
} xpth_type;

typedef struct _xpath_node
{
    struct _xpath_node *left;
    struct _xpath_node *right;
    int result;
    int op;
    int op_prec;
    bool reverse_step;
    // char *type;
    int type;
    char *node_type;
    char *string_value;
    char *number;
    char *prefix;
    char *name;
    char *axis;
    GList *arg_list;
} xpath_node;

typedef struct _xpath_funcs
{
    // Should be called once per build
    void (*start_build) (void);

    // Should be called after build for result tree post-processing
    xpath_node * (*end_build) (xpath_node *result);

    xpath_node * (*string) (char *value);

    xpath_node * (*number) (char *value);

    xpath_node * (*operator) (xpath_operator op, xpath_node *left, xpath_node *right);

    xpath_node * (*axis) (xpath_axis x_axis, xpath_node_type node_type, char *prefix, char *name);

    xpath_node * (*join_step) (xpath_node *left, xpath_node *right);

    // http://www.w3.org/TR/xquery-semantics/#id-axis-steps
    // reverseStep is how parser comunicates to builder diference between "ansestor[1]" and "(ansestor)[1]"
    xpath_node * (*predicate) (xpath_node *node, xpath_node *condition, bool reverse_step);

    xpath_node * (*variable) (char *prefix, char *name);

    xpath_node * (*function) (char *prefix, char *name, GList *args);
} xpath_funcs;

xpath_node *sch_xpath_allocate_node (void);
char *sch_xpath_node_type_string (int nt);
int sch_xpath_axis_to_type (int axis);
int sch_xpath_op_to_type (int op);
xpath_node *sch_xpath_parser (char *expr);
void sch_xpath_free_xnode_tree (xpath_node *xnode);
void sch_xpath_build_register (xpath_funcs *funcs, bool debug, bool verbose);

#endif /* _SCH_XPATH_H_ */
