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

/* Schema */
typedef void sch_instance;
typedef void sch_node;
sch_instance *sch_load (const char *path);
void sch_free (sch_instance * schema);
sch_node *sch_lookup (sch_instance * schema, const char *path);
sch_node *sch_ns_lookup (sch_instance * schema, const char *namespace, const char *path);
char *sch_dump_xml (sch_instance * schema);

sch_node *sch_node_child (sch_node * parent, const char *name);
sch_node *sch_node_child_first (sch_node * parent);
sch_node *sch_node_child_next (sch_node * parent, sch_node * node);

char *sch_name (sch_node * node);
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
    SCH_F_STRIP_KEY = 0x1,
    SCH_F_JSON_ARRAYS = 0x2,
    SCH_F_JSON_TYPES = 0x4,
} sch_flags;
#ifdef APTERYX_XML_LIBXML2
#include <libxml/tree.h>
xmlNode *sch_gnode_to_xml (sch_instance * instance, sch_node * schema, GNode * node, int flags);
GNode *sch_xml_to_gnode (sch_instance * instance, sch_node * schema, xmlNode * xml, int flags);
#endif
#ifdef APTERYX_XML_JSON
#include <jansson.h>
json_t *sch_gnode_to_json (sch_instance * instance, sch_node * schema, GNode * node, int flags);
#endif

#endif /* _APTERYX_XML_H_ */
