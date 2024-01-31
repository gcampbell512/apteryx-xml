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

typedef void * sch_xml_to_gnode_parms;

/* Schema */
typedef struct _sch_instance sch_instance;
typedef void sch_node;
sch_instance *sch_load (const char *path);
sch_instance *sch_load_with_model_list_filename (const char *path,
                                                 const char *model_list_filename);
void sch_free (sch_instance * instance);
sch_node *sch_lookup (sch_instance * instance, const char *path);
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
bool sch_is_hidden (sch_node * node);
bool sch_is_config (sch_node * node);
bool sch_is_proxy (sch_node * node);
char *sch_translate_to (sch_node * node, char *value);
char *sch_translate_from (sch_node * node, char *value);
bool sch_validate_pattern (sch_node * node, const char *value);
gboolean sch_match_name (const char *s1, const char *s2);
bool sch_ns_match (sch_node *node, void *ns);

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
} sch_flags;
GNode *sch_path_to_gnode (sch_instance * instance, sch_node * schema, const char * path, int flags, sch_node ** rschema);
bool sch_query_to_gnode (sch_instance * instance, sch_node * schema, GNode *parent, const char * query, int flags, int *rflags);
bool sch_traverse_tree (sch_instance * instance, sch_node * schema, GNode * node, int flags, int rdepth);
GNode *sch_path_to_query (sch_instance * instance, sch_node * schema, const char * path, int flags); //DEPRECATED
GNode *sch_translate_input (sch_instance * instance, GNode *node, int flags,
                            void **xlat_data, sch_node **rschema);
GNode *sch_translate_output (sch_instance * instance, GNode *node, GNode *query, int flags, void *xlat_data);
GNode *sch_translate_copy_query (GNode *query);

/*
 * Netconf error handling
 **/

/* Enumeration of <rpc-error> error-type information */
typedef enum _NC_RPC_ERROR_TYPE {
    NC_ERR_TYPE_UNKNOWN = 0,   /* unknown layer */
    NC_ERR_TYPE_TRANSPORT,     /* secure transport layer */
    NC_ERR_TYPE_RPC,           /* rpc layer */
    NC_ERR_TYPE_PROTOCOL,      /* protocol layer */
    NC_ERR_TYPE_APP            /* application layer */
} NC_ERR_TYPE;

/* Enumeration of <rpc-error> error-tag information */
typedef enum _NC_RPC_ERROR_TAG {
    NC_ERR_TAG_UNKNOWN = 0,         /* unknown error */
    NC_ERR_TAG_IN_USE,              /* in-use error */
    NC_ERR_TAG_INVALID_VAL,         /* invalid-value error */
    NC_ERR_TAG_TOO_BIG,             /* too-big error */
    NC_ERR_TAG_MISSING_ATTR,        /* missing-attribute error */
    NC_ERR_TAG_BAD_ATTR,            /* bad-attribute error */
    NC_ERR_TAG_UNKNOWN_ATTR,        /* unknown-attribute error */
    NC_ERR_TAG_MISSING_ELEM,        /* missing-element error */
    NC_ERR_TAG_BAD_ELEM,            /* bad-element error */
    NC_ERR_TAG_UNKNOWN_ELEM,        /* unknown-element error */
    NC_ERR_TAG_UNKNOWN_NS,          /* unknown-namespace error */
    NC_ERR_TAG_ACCESS_DENIED,       /* access-denied error */
    NC_ERR_TAG_LOCK_DENIED,         /* lock-denied error */
    NC_ERR_TAG_RESOURCE_DENIED,     /* resource-denied error */
    NC_ERR_TAG_DATA_EXISTS,         /* data-exists error */
    NC_ERR_TAG_DATA_MISSING,        /* data-missing error */
    NC_ERR_TAG_OPR_NOT_SUPPORTED,   /* operation-not-supported error */
    NC_ERR_TAG_OPR_FAILED,          /* operation-failed error */
    NC_ERR_TAG_MALFORMED_MSG        /* malformed-message error */
} NC_ERR_TAG;

typedef struct _nc_error_parms_s
{
    NC_ERR_TAG tag;
    NC_ERR_TYPE type;
    GHashTable *info;
    GString* msg;
} nc_error_parms;

#define NC_ERROR_PARMS_INIT                                     \
(nc_error_parms)                                                \
{                                                               \
    .tag  = NC_ERR_TAG_UNKNOWN,                                 \
    .type = NC_ERR_TYPE_UNKNOWN,                                \
    .info = g_hash_table_new_full (g_str_hash, g_str_equal,     \
                                   NULL, g_free),               \
    .msg  = g_string_new (NULL)                                 \
};

#ifdef APTERYX_XML_LIBXML2
#include <libxml/tree.h>
xmlNode *sch_gnode_to_xml (sch_instance * instance, sch_node * schema, GNode * node, int flags);
sch_xml_to_gnode_parms sch_xml_to_gnode (sch_instance * instance, sch_node * schema,
                                         xmlNode * xml, int flags, char * def_op,
                                         bool is_edit, sch_node **rschema);
GNode *sch_parm_tree (sch_xml_to_gnode_parms parms);
nc_error_parms sch_parm_error (sch_xml_to_gnode_parms parms);
GList *sch_parm_deletes (sch_xml_to_gnode_parms parms);
GList *sch_parm_removes (sch_xml_to_gnode_parms parms);
GList *sch_parm_creates (sch_xml_to_gnode_parms parms);
GList *sch_parm_replaces (sch_xml_to_gnode_parms parms);
void sch_parm_free (sch_xml_to_gnode_parms parms);
#endif
#ifdef APTERYX_XML_JSON
#include <jansson.h>
json_t *sch_gnode_to_json (sch_instance * instance, sch_node * schema, GNode * node, int flags);
GNode *sch_json_to_gnode (sch_instance * instance, sch_node * schema, json_t * json, int flags);
#endif
#endif /* _APTERYX_XML_H_ */
