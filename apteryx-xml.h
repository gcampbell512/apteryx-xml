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
sch_instance* sch_load (const char *path);
void sch_free (sch_instance *schema);
sch_node* sch_lookup (sch_instance *schema, const char *path);
bool sch_is_leaf (sch_node *node);
bool sch_is_readable (sch_node *node);
bool sch_is_writable (sch_node *node);
char* sch_name (sch_node *node);
char* sch_translate_to (sch_node *node, char *value);
char* sch_translate_from (sch_node *node, char *value);

#endif /* _APTERYX_XML_H_ */
