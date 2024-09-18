/**
 * @file apteryx-xml.h
 *
 * Libraries for using an XML based schema with Apteryx.
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
#ifndef _APTERYX_XML_H_
#define _APTERYX_XML_H_

/* Thread local error handling */
typedef enum
{
    SCH_E_SUCCESS,
    SCH_E_INTERNAL,
    SCH_E_PATREGEX,
    SCH_E_OUTOFRANGE,
    SCH_E_ENUMINVALID,
    SCH_E_NOSCHEMANODE,
    SCH_E_NOTREADABLE,
    SCH_E_NOTWRITABLE,
    SCH_E_KEYMISSING,
    SCH_E_INVALIDQUERY,
} sch_err;
sch_err sch_last_err (void);
const char * sch_last_errmsg (void);

typedef struct _sch_loaded_model
{
    char *ns_href;
    char *ns_prefix;
    char *model;
    char *organization;
    char *version;
    char *features;
    char *deviations;
} sch_loaded_model;

/* Schema */
typedef struct _sch_instance sch_instance;
typedef void sch_node;
typedef void sch_ns;
sch_instance *sch_load (const char *path);
sch_instance *sch_load_with_model_list_filename (const char *path,
                                                 const char *model_list_filename);
void sch_free (sch_instance * instance);
sch_node *sch_lookup (sch_instance * instance, const char *path);
sch_node *sch_lookup_with_ns (sch_instance * instance, sch_ns *ns, const char *path);
char *sch_dump_xml (sch_instance * instance);
GList *sch_get_loaded_models (sch_instance * instance);

sch_node *sch_child_first (sch_instance *instance);
sch_node *sch_node_parent (sch_node *node);
sch_node *sch_node_child (sch_node *parent, const char *name);
sch_node *sch_node_namespace_child (sch_node * parent, const char *namespace, const char *child);
sch_node *sch_node_by_namespace (sch_instance * instance, const char *namespace,
                                 const char *prefix);
sch_node *sch_node_child_first (sch_node * parent);
sch_node *sch_node_next_sibling (sch_node * node);
sch_node *sch_preorder_next (sch_node *current, sch_node *root);
sch_node *sch_get_root_schema (sch_instance * instance);

sch_ns *sch_node_ns (sch_node * node);
const char *sch_ns_prefix (sch_instance *instance, sch_ns *ns);
const char *sch_ns_href (sch_instance *instance, sch_ns *ns);
bool sch_ns_native (sch_instance *instance, sch_ns *ns);
sch_ns *sch_lookup_ns (sch_instance * instance, sch_node *schema, const char *name, int flags, bool href);
sch_node *sch_ns_node_child (sch_ns *ns, sch_node * parent, const char *child);

char *sch_name (sch_node * node);
char *sch_model (sch_node * node, bool ignore_ancestors);
char *sch_organization (sch_node * node);
char *sch_version (sch_node * node);
char *sch_namespace (sch_node * node);
char *sch_prefix (sch_node * node);
char *sch_default_value (sch_node * node);
char *sch_path (sch_node * node);
bool sch_is_leaf (sch_node * node);
bool sch_is_list (sch_node * node);
bool sch_is_leaf_list (sch_node * node);
char *sch_list_key (sch_node * node);
bool sch_is_readable (sch_node * node);
bool sch_is_writable (sch_node * node);
bool sch_is_executable (sch_node * node);
bool sch_is_hidden (sch_node * node);
bool sch_is_config (sch_node * node);
bool sch_is_proxy (sch_node * node);
bool sch_is_read_only_proxy (sch_node * node);
char *sch_translate_to (sch_node * node, char *value);
char *sch_translate_from (sch_node * node, char *value);
bool sch_validate_pattern (sch_node * node, const char *value);
gboolean sch_match_name (const char *s1, const char *s2);
bool sch_ns_match (sch_node *node, sch_ns *ns);

/* Data translation/manipulation */
typedef enum
{
    SCH_F_DEBUG                 = (1 << 0),  /* Debug processing to stdout */
    SCH_F_STRIP_KEY             = (1 << 1),  /* Strip list keys out of path */
    SCH_F_JSON_ARRAYS           = (1 << 2),  /* Use JSON arrays for list items */
    SCH_F_JSON_TYPES            = (1 << 3),  /* Translate to/from json types */
    SCH_F_XPATH                 = (1 << 4),  /* Path is in xpath format */
    SCH_F_CONFIG                = (1 << 5),  /* Format config-only nodes */
    SCH_F_NS_PREFIX             = (1 << 6),  /* Prefix model name before node names (on change of ns) */
    SCH_F_NS_MODEL_NAME         = (1 << 7),  /* Convert model names to namespaces */
    SCH_F_STRIP_DATA            = (1 << 8),  /* Strip data values from the tree */
    SCH_F_DEPTH_ONE             = (1 << 9),  /* Query is a depth one */
    SCH_F_ADD_DEFAULTS          = (1 << 10), /* Add all default nodes */
    SCH_F_TRIM_DEFAULTS         = (1 << 11), /* Remove all nodes set to default values */
    SCH_F_ADD_MISSING_NULL      = (1 << 12), /* Add missing nodes with NULL values */
    SCH_F_SET_NULL              = (1 << 13), /* Set all nodes to NULL */
    SCH_F_FILTER_RDEPTH         = (1 << 14), /* Set filter based on depth value */
    SCH_F_IDREF_VALUES          = (1 << 15), /* Expand identityref based values to include type information */
    SCH_F_MODIFY_DATA           = (1 << 16), /* The created tree will be used to modify the associated model */
    SCH_F_CONDITIONS            = (1 << 17), /* Check the schema node for any condition attributes */
    SCH_F_DEPTH                 = (1 << 18), /* Query to a specific depth */
} sch_flags;
GNode *sch_path_to_gnode (sch_instance * instance, sch_node * schema, const char * path, int flags, sch_node ** rschema);
bool sch_query_to_gnode (sch_instance * instance, sch_node * schema, GNode *parent, const char * query, int flags,
                         int *rflags, int *param_depth);
bool sch_traverse_tree (sch_instance * instance, sch_node * schema, GNode * node, int flags, int rdepth);
GNode *sch_path_to_query (sch_instance * instance, sch_node * schema, const char * path, int flags); //DEPRECATED
void sch_gnode_sort_children (sch_node * schema, GNode * parent);
void sch_check_condition (sch_node *node, GNode *root, int flags, char **path, char **condition);
bool sch_apply_conditions (sch_instance * instance, sch_node * schema, GNode *node, int flags);
bool sch_trim_tree_by_depth (sch_instance *instance, sch_node *schema, GNode *node, int flags, int rdepth);

#ifdef APTERYX_XML_JSON
#include <jansson.h>
json_t *sch_gnode_to_json (sch_instance * instance, sch_node * schema, GNode * node, int flags);
GNode *sch_json_to_gnode (sch_instance * instance, sch_node * schema, json_t * json, int flags);
#endif

bool sch_process_condition (sch_instance *instance, GNode *root, char *path,
                            char *condition);
void sch_condition_register (gboolean debug, gboolean verbose);

#endif /* _APTERYX_XML_H_ */
