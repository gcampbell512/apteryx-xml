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
    SCH_E_PATREGEX,
    SCH_E_ENUMINVALID,
    SCH_E_NOSCHEMANODE,
    SCH_E_NOTREADABLE,
    SCH_E_NOTWRITABLE,
    SCH_E_KEYMISSING,
    SCH_E_INVALIDQUERY,
} sch_err;
sch_err sch_last_err (void);
const char * sch_last_errmsg (void);

/* Schema */
typedef void sch_instance;
typedef void sch_node;
sch_instance *sch_load (const char *path);
void sch_free (sch_instance * schema);
sch_node *sch_lookup (sch_instance * schema, const char *path);
char *sch_dump_xml (sch_instance * schema);

sch_node *sch_node_child (sch_node *parent, const char *name);
sch_node *sch_node_child_first (sch_node * parent);
sch_node *sch_node_next_sibling (sch_node * node);
sch_node *sch_preorder_next (sch_node *current, sch_node *root);

char *sch_name (sch_node * node);
char *sch_model (sch_node * node, bool ignore_ancestors);
char *sch_organization (sch_node * node);
char *sch_version (sch_node * node);
char *sch_default_value (sch_node * node);
char *sch_path (sch_node * node);
bool sch_is_leaf (sch_node * node);
bool sch_is_list (sch_node * node);
char *sch_list_key (sch_node * node);
bool sch_is_readable (sch_node * node);
bool sch_is_writable (sch_node * node);
bool sch_is_config (sch_node * node);
char *sch_translate_to (sch_node * node, char *value);
char *sch_translate_from (sch_node * node, char *value);
bool sch_validate_pattern (sch_node * node, const char *value);

/* Data translation/manipulation */
typedef enum
{
    SCH_F_DEBUG = 0x1,
    SCH_F_STRIP_KEY = 0x2,
    SCH_F_JSON_ARRAYS = 0x4,
    SCH_F_JSON_TYPES = 0x8,
    SCH_F_XPATH = 0x10,
    SCH_F_CONFIG = 0x20,
} sch_flags;
GNode *sch_path_to_query (sch_instance * instance, sch_node * schema, const char *path, int flags);
#ifdef APTERYX_XML_LIBXML2
#include <libxml/tree.h>
xmlNode *sch_gnode_to_xml (sch_instance * instance, sch_node * schema, GNode * node, int flags);
GNode *sch_xml_to_gnode (sch_instance * instance, sch_node * schema, xmlNode * xml, int flags);
#endif
#ifdef APTERYX_XML_JSON
#include <jansson.h>
json_t *sch_gnode_to_json (sch_instance * instance, sch_node * schema, GNode * node, int flags);
GNode *sch_json_to_gnode (sch_instance * instance, sch_node * schema, json_t * json, int flags);
#endif

#endif /* _APTERYX_XML_H_ */
