/* $Id: utils.c,v 1.1 2006/05/31 14:59:12 andrew Exp $ */
/* 
 * Copyright (C) 2004 Andrew Beekhof <andrew@beekhof.net>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include <portability.h>
#include <crm/crm.h>
#include <crm/cib.h>
#include <crm/msg_xml.h>
#include <crm/common/xml.h>
#include <crm/common/util.h>

#include <glib.h>

#include <pengine.h>
#include <lib/crm/pengine/rules.h>
#include <utils.h>

void print_str_str(gpointer key, gpointer value, gpointer user_data);
gboolean ghash_free_str_str(gpointer key, gpointer value, gpointer user_data);
void unpack_operation(
	action_t *action, crm_data_t *xml_obj, pe_working_set_t* data_set);

void
pe_free_shallow(GListPtr alist)
{
	pe_free_shallow_adv(alist, TRUE);
}

void
pe_free_shallow_adv(GListPtr alist, gboolean with_data)
{
	GListPtr item;
	GListPtr item_next = alist;
	while(item_next != NULL) {
		item = item_next;
		item_next = item_next->next;
		
		if(with_data) {
/*			crm_debug_5("freeing %p", item->data); */
			crm_free(item->data);
		}
		
		item->data = NULL;
		item->next = NULL;
		g_list_free(item);
	}
}

node_t *
pe_find_node_id(GListPtr nodes, const char *id)
{
	unsigned lpc = 0;
	node_t *node = NULL;
  
	for(lpc = 0; lpc < g_list_length(nodes); lpc++) {
		node = g_list_nth_data(nodes, lpc);
		if(safe_str_eq(node->details->id, id)) {
			return node;
		}
	}
	/* error */
	return NULL;
}

node_t *
node_copy(node_t *this_node) 
{
	node_t *new_node  = NULL;

	CRM_CHECK(this_node != NULL, return NULL);
	crm_malloc0(new_node, sizeof(node_t));

	CRM_CHECK(new_node != NULL, return NULL);
	
	crm_debug_5("Copying %p (%s) to %p",
		  this_node, this_node->details->uname, new_node);
	new_node->weight  = this_node->weight; 
	new_node->fixed   = this_node->fixed;
	new_node->details = this_node->details;	
	
	return new_node;
}

/* are the contents of list1 and list2 equal 
 * nodes with weight < 0 are ignored if filter == TRUE
 *
 * slow but linear
 *
 */
gboolean
node_list_eq(GListPtr list1, GListPtr list2, gboolean filter)
{
	node_t *other_node;

	GListPtr lhs = list1;
	GListPtr rhs = list2;
	
	slist_iter(
		node, node_t, lhs, lpc,

		if(node == NULL || (filter && node->weight < 0)) {
			continue;
		}

		other_node = (node_t*)
			pe_find_node_id(rhs, node->details->id);

		if(other_node == NULL || other_node->weight < 0) {
			return FALSE;
		}
		);
	
	lhs = list2;
	rhs = list1;

	slist_iter(
		node, node_t, lhs, lpc,

		if(node == NULL || (filter && node->weight < 0)) {
			continue;
		}

		other_node = (node_t*)
			pe_find_node_id(rhs, node->details->id);

		if(other_node == NULL || other_node->weight < 0) {
			return FALSE;
		}
		);
  
	return TRUE;
}

/* the intersection of list1 and list2 
 */
GListPtr
node_list_and(GListPtr list1, GListPtr list2, gboolean filter)
{
	GListPtr result = NULL;
	unsigned lpc = 0;

	for(lpc = 0; lpc < g_list_length(list1); lpc++) {
		node_t *node = (node_t*)g_list_nth_data(list1, lpc);
		node_t *other_node = pe_find_node_id(list2, node->details->id);
		node_t *new_node = NULL;

		if(other_node != NULL) {
			new_node = node_copy(node);
		}
		
		if(new_node != NULL) {
			crm_debug_4("%s: %d + %d", node->details->uname, 
				    other_node->weight, new_node->weight);
			new_node->weight = merge_weights(
				new_node->weight, other_node->weight);

			crm_debug_3("New node weight for %s: %d",
				 new_node->details->uname, new_node->weight);
			
			if(filter && new_node->weight < 0) {
				crm_free(new_node);
				new_node = NULL;
			}
		}
		
		if(new_node != NULL) {
			result = g_list_append(result, new_node);
		}
	}

	return result;
}


/* list1 - list2 */
GListPtr
node_list_minus(GListPtr list1, GListPtr list2, gboolean filter)
{
	GListPtr result = NULL;

	slist_iter(
		node, node_t, list1, lpc,
		node_t *other_node = pe_find_node_id(list2, node->details->id);
		node_t *new_node = NULL;
		
		if(node == NULL || other_node != NULL
		   || (filter && node->weight < 0)) {
			continue;
			
		}
		new_node = node_copy(node);
		result = g_list_append(result, new_node);
		);
  
	crm_debug_3("Minus result len: %d", g_list_length(result));

	return result;
}

/* list1 + list2 - (intersection of list1 and list2) */
GListPtr
node_list_xor(GListPtr list1, GListPtr list2, gboolean filter)
{
	GListPtr result = NULL;
	
	slist_iter(
		node, node_t, list1, lpc,
		node_t *new_node = NULL;
		node_t *other_node = (node_t*)
			pe_find_node_id(list2, node->details->id);

		if(node == NULL || other_node != NULL
		   || (filter && node->weight < 0)) {
			continue;
		}
		new_node = node_copy(node);
		result = g_list_append(result, new_node);
		);
	
 
	slist_iter(
		node, node_t, list2, lpc,
		node_t *new_node = NULL;
		node_t *other_node = (node_t*)
			pe_find_node_id(list1, node->details->id);

		if(node == NULL || other_node != NULL
		   || (filter && node->weight < 0)) {
			continue;
		}
		new_node = node_copy(node);
		result = g_list_append(result, new_node);
		);
  
	crm_debug_3("Xor result len: %d", g_list_length(result));
	return result;
}

GListPtr
node_list_or(GListPtr list1, GListPtr list2, gboolean filter)
{
	node_t *other_node = NULL;
	GListPtr result = NULL;
	gboolean needs_filter = FALSE;

	result = node_list_dup(list1, FALSE, filter);

	slist_iter(
		node, node_t, list2, lpc,

		if(node == NULL) {
			continue;
		}

		other_node = (node_t*)pe_find_node_id(
			result, node->details->id);

		if(other_node != NULL) {
			crm_debug_4("%s + %s: %d + %d",
				    node->details->uname, 
				    other_node->details->uname, 
				    node->weight, other_node->weight);
			other_node->weight = merge_weights(
				other_node->weight, node->weight);
			
			if(filter && node->weight < 0) {
				needs_filter = TRUE;
			}

		} else if(filter == FALSE || node->weight >= 0) {
			node_t *new_node = node_copy(node);
			result = g_list_append(result, new_node);
		}
		);

	/* not the neatest way, but the most expedient for now */
	if(filter && needs_filter) {
		GListPtr old_result = result;
		result = node_list_dup(old_result, FALSE, filter);
		pe_free_shallow_adv(old_result, TRUE);
	}
	

	return result;
}

GListPtr 
node_list_dup(GListPtr list1, gboolean reset, gboolean filter)
{
	GListPtr result = NULL;

	slist_iter(
		this_node, node_t, list1, lpc,
		node_t *new_node = NULL;
		if(filter && this_node->weight < 0) {
			continue;
		}
		
		new_node = node_copy(this_node);
		if(reset) {
			new_node->weight = 0;
		}
		if(new_node != NULL) {
			result = g_list_append(result, new_node);
		}
		);

	return result;
}

/* only for rsc_colocation constraints */
rsc_colocation_t *
invert_constraint(rsc_colocation_t *constraint) 
{
	rsc_colocation_t *inverted_con = NULL;

	crm_debug_3("Inverting constraint");
	if(constraint == NULL) {
		pe_err("Cannot invert NULL constraint");
		return NULL;
	}

	crm_malloc0(inverted_con, sizeof(rsc_colocation_t));

	if(inverted_con == NULL) {
		return NULL;
	}
	
	inverted_con->id = constraint->id;
	inverted_con->strength = constraint->strength;

	/* swap the direction */
	inverted_con->rsc_lh = constraint->rsc_rh;
	inverted_con->rsc_rh = constraint->rsc_lh;
	inverted_con->state_lh = constraint->state_rh;
	inverted_con->state_rh = constraint->state_lh;

	crm_action_debug_3(
		print_rsc_colocation("Inverted constraint", inverted_con, FALSE));
	
	return inverted_con;
}

/*
 * Create a new color with the contents of "nodes" as the list of
 *  possible nodes that resources with this color can be run on.
 *
 * Typically, when creating a color you will provide the node list from
 *  the resource you will first assign the color to.
 *
 * If "colors" != NULL, it will be added to that list
 * If "resources" != NULL, it will be added to every provisional resource
 *  in that list
 */
color_t *
create_color(
	pe_working_set_t *data_set, resource_t *resource, GListPtr node_list)
{
	color_t *new_color = NULL;
	
	crm_debug_5("Creating color");
	crm_malloc0(new_color, sizeof(color_t));
	if(new_color == NULL) {
		return NULL;
	}
	
	new_color->id           = data_set->color_id++;
	new_color->local_weight = 1.0;
	
	crm_debug_5("Creating color details");
	crm_malloc0(new_color->details, sizeof(struct color_shared_s));

	if(new_color->details == NULL) {
		crm_free(new_color);
		return NULL;
	}
		
	new_color->details->id                  = new_color->id;
	new_color->details->highest_priority    = -1;
	new_color->details->chosen_node         = NULL;
	new_color->details->candidate_nodes     = NULL;
	new_color->details->allocated_resources = NULL;
	new_color->details->pending             = TRUE;
	
	if(resource != NULL) {
		crm_debug_5("populating node list");
		new_color->details->highest_priority = resource->priority;
		new_color->details->candidate_nodes  =
			node_list_dup(node_list, TRUE, TRUE);
	}
	
	crm_action_debug_3(print_color("Created color", new_color, TRUE));

	CRM_CHECK(data_set != NULL, return NULL);
	data_set->colors = g_list_append(data_set->colors, new_color);
	return new_color;
}

color_t *
copy_color(color_t *a_color) 
{
	color_t *color_copy = NULL;

	if(a_color == NULL) {
		pe_err("Cannot copy NULL");
		return NULL;
	}
	
	crm_malloc0(color_copy, sizeof(color_t));
	if(color_copy != NULL) {
		color_copy->id      = a_color->id;
		color_copy->details = a_color->details;
		color_copy->local_weight = 1.0;
	}
	return color_copy;
}

gint gslist_color_compare(gconstpointer a, gconstpointer b);
color_t *
find_color(GListPtr candidate_colors, color_t *other_color)
{
	GListPtr tmp = g_list_find_custom(candidate_colors, other_color,
					    gslist_color_compare);
	if(tmp != NULL) {
		return (color_t *)tmp->data;
	}
	return NULL;
}


gint gslist_color_compare(gconstpointer a, gconstpointer b)
{
	const color_t *color_a = (const color_t*)a;
	const color_t *color_b = (const color_t*)b;

/*	crm_debug_5("%d vs. %d", a?color_a->id:-2, b?color_b->id:-2); */
	if(a == b) {
		return 0;
	} else if(a == NULL || b == NULL) {
		return 1;
	} else if(color_a->id == color_b->id) {
		return 0;
	}
	return 1;
}

gint sort_rsc_priority(gconstpointer a, gconstpointer b)
{
	const resource_t *resource1 = (const resource_t*)a;
	const resource_t *resource2 = (const resource_t*)b;

	if(a == NULL && b == NULL) { return 0; }
	if(a == NULL) { return 1; }
	if(b == NULL) { return -1; }
  
	if(resource1->priority > resource2->priority) {
		return -1;
	}
	
	if(resource1->priority < resource2->priority) {
		return 1;
	}

	return 0;
}

gint sort_rsc_node_weight(gconstpointer a, gconstpointer b)
{
	const resource_t *resource1 = (const resource_t*)a;
	const resource_t *resource2 = (const resource_t*)b;

	const color_t *color1 = NULL;
	const color_t *color2 = NULL;

	const node_t *node1 = NULL;
	const node_t *node2 = NULL;

	CRM_ASSERT(resource1 != NULL);
	CRM_ASSERT(resource2 != NULL);

	color1 = resource1->color;
	color2 = resource2->color;
	
	CRM_CHECK(color1 != NULL, return 0);
	CRM_CHECK(color2 != NULL, return 0);
	node1 = color1->details->chosen_node;
	node2 = color2->details->chosen_node;

	if(node1 == NULL && node2 == NULL) { return 0; }
	if(node1 == NULL) { return 1; }
	if(node2 == NULL) { return -1; }

	CRM_ASSERT(node1 != NULL);
	CRM_ASSERT(node2 != NULL);
	if(node1->weight > node2->weight) {
		crm_debug("%s (%d) > %s (%d) : %s vs. %s",
			  node1->details->id, node1->weight,
			  node2->details->id, node2->weight,
			  resource1->id, resource2->id);
		return -1;
	}
	
	if(node1->weight < node2->weight) {
		crm_debug("%s (%d) < %s (%d) : %s vs. %s",
			  node1->details->id, node1->weight,
			  node2->details->id, node2->weight,
			  resource1->id, resource2->id);
		return 1;
	}
	crm_debug("%s (%d) == %s (%d) : %s vs. %s",
		  node1->details->id, node1->weight,
		  node2->details->id, node2->weight,
		  resource1->id, resource2->id);

	return 0;
}

/* lowest to highest */
gint sort_action_id(gconstpointer a, gconstpointer b)
{
	const action_wrapper_t *action_wrapper2 = (const action_wrapper_t*)a;
	const action_wrapper_t *action_wrapper1 = (const action_wrapper_t*)b;

	if(a == NULL) { return 1; }
	if(b == NULL) { return -1; }
  
	if(action_wrapper1->action->id > action_wrapper2->action->id) {
		return -1;
	}
	
	if(action_wrapper1->action->id < action_wrapper2->action->id) {
		return 1;
	}
	return 0;
}

gint sort_cons_strength(gconstpointer a, gconstpointer b)
{
	const rsc_colocation_t *rsc_constraint1 = (const rsc_colocation_t*)a;
	const rsc_colocation_t *rsc_constraint2 = (const rsc_colocation_t*)b;

	if(a == NULL) { return 1; }
	if(b == NULL) { return -1; }
  
	if(rsc_constraint1->strength > rsc_constraint2->strength) {
		return 1;
	}
	
	if(rsc_constraint1->strength < rsc_constraint2->strength) {
		return -1;
	}
	return 0;
}

gint sort_color_weight(gconstpointer a, gconstpointer b)
{
	const color_t *color1 = (const color_t*)a;
	const color_t *color2 = (const color_t*)b;

	if(a == NULL) { return 1; }
	if(b == NULL) { return -1; }
  
	if(color1->local_weight > color2->local_weight) {
		return -1;
	}
	
	if(color1->local_weight < color2->local_weight) {
		return 1;
	}
	
	return 0;
}

/* return -1 if 'a' is more preferred
 * return  1 if 'b' is more preferred
 */
gint sort_node_weight(gconstpointer a, gconstpointer b)
{
	const node_t *node1 = (const node_t*)a;
	const node_t *node2 = (const node_t*)b;

	int node1_weight = 0;
	int node2_weight = 0;
	
	if(a == NULL) { return 1; }
	if(b == NULL) { return -1; }

	node1_weight = node1->weight;
	node2_weight = node2->weight;
	
	if(node1->details->unclean || node1->details->shutdown) {
		node1_weight  = -INFINITY; 
	}
	if(node2->details->unclean || node2->details->shutdown) {
		node2_weight  = -INFINITY; 
	}

	if(node1_weight > node2_weight) {
		crm_debug_3("%s (%d) > %s (%d) : weight",
			    node1->details->uname, node1_weight,
			    node2->details->uname, node2_weight);
		return -1;
	}
	
	if(node1_weight < node2_weight) {
		crm_debug_3("%s (%d) < %s (%d) : weight",
			    node1->details->uname, node1_weight,
			    node2->details->uname, node2_weight);
		return 1;
	}

	crm_debug_3("%s (%d) == %s (%d) : weight",
		    node1->details->uname, node1_weight,
		    node2->details->uname, node2_weight);
	
	/* now try to balance resources across the cluster */
	if(node1->details->num_resources
	   < node2->details->num_resources) {
		crm_debug_3("%s (%d) < %s (%d) : resources",
			    node1->details->uname, node1->details->num_resources,
			    node2->details->uname, node2->details->num_resources);
		return -1;
		
	} else if(node1->details->num_resources
		  > node2->details->num_resources) {
		crm_debug_3("%s (%d) > %s (%d) : resources",
			    node1->details->uname, node1->details->num_resources,
			    node2->details->uname, node2->details->num_resources);
		return 1;
	}
	
	crm_debug_4("%s = %s", node1->details->uname, node2->details->uname);
	return 0;
}

action_t *
custom_action(resource_t *rsc, char *key, const char *task,
	      node_t *on_node, gboolean optional, gboolean save_action,
	      pe_working_set_t *data_set)
{
	action_t *action = NULL;
	GListPtr possible_matches = NULL;
	CRM_CHECK(key != NULL, return NULL);
	CRM_CHECK(task != NULL, return NULL);

	if(save_action && rsc != NULL) {
		possible_matches = find_actions(rsc->actions, key, on_node);
	}
	
	if(possible_matches != NULL) {
		crm_free(key);
		
		if(g_list_length(possible_matches) > 1) {
			pe_warn("Action %s for %s on %s exists %d times",
				task, rsc?rsc->id:"<NULL>",
				on_node?on_node->details->uname:"<NULL>",
				g_list_length(possible_matches));
		}
		
		action = g_list_nth_data(possible_matches, 0);
		crm_debug_4("Found existing action (%d) %s for %s on %s",
			  action->id, task, rsc?rsc->id:"<NULL>",
			  on_node?on_node->details->uname:"<NULL>");
	}

	if(action == NULL) {
		if(save_action) {
			crm_debug_2("Creating%s action %d: %s for %s on %s",
				    optional?"":" manditory", data_set->action_id, key, rsc?rsc->id:"<NULL>",
				    on_node?on_node->details->uname:"<NULL>");
		}
		
		crm_malloc0(action, sizeof(action_t));
		if(action != NULL) {
			if(save_action) {
				action->id   = data_set->action_id++;
			} else {
				action->id = 0;
			}
			action->rsc  = rsc;
			action->task = task;
			action->node = on_node;
			
			action->actions_before   = NULL;
			action->actions_after    = NULL;
			action->failure_is_fatal = TRUE;
			
			action->pseudo     = FALSE;
			action->dumped     = FALSE;
			action->runnable   = TRUE;
			action->processed  = FALSE;
			action->optional   = optional;
			action->seen_count = 0;
			
			action->extra = g_hash_table_new_full(
				g_str_hash, g_str_equal,
				g_hash_destroy_str, g_hash_destroy_str);

			action->meta = g_hash_table_new_full(
				g_str_hash, g_str_equal,
				g_hash_destroy_str, g_hash_destroy_str);

			if(save_action) {
				data_set->actions = g_list_append(
					data_set->actions, action);
			}
			
			action->uuid = key;

			if(rsc != NULL) {
				action->op_entry = find_rsc_op_entry(rsc, key);
				
				unpack_operation(
					action, action->op_entry, data_set);

				if(save_action) {
					rsc->actions = g_list_append(
						rsc->actions, action);
				}
			}
			if(save_action) {
				crm_debug_4("Action %d created", action->id);
			}
		}
	}

	if(optional == FALSE && action->optional) {
		crm_debug_2("Action %d (%s) marked manditory",
			    action->id, action->uuid);
		action->optional = FALSE;
	}
	
	if(rsc != NULL) {
		enum action_tasks a_task = text2task(action->task);
		int warn_level = LOG_DEBUG_3;
		if(save_action) {
			warn_level = LOG_WARNING;
		}

		if(action->node != NULL && action->op_entry != NULL) {
			unpack_instance_attributes(
				action->op_entry, XML_TAG_ATTR_SETS,
				action->node->details->attrs,
				action->extra, NULL, data_set->now);
		}

		if(action->node == NULL) {
			action->runnable = FALSE;

		} else if(rsc->is_managed == FALSE) {
			crm_log_maybe(warn_level, "Action %s %s is for %s (unmanaged)",
				 action->uuid, task, rsc->id);
			action->optional = TRUE;
/*   			action->runnable = FALSE; */

#if 0
		} else if(action->node->details->unclean) {
			crm_log_maybe(warn_level, "Action %s on %s is unrunnable (unclean)",
				 action->uuid, action->node?action->node->details->uname:"<none>");

			action->runnable = FALSE;
#endif	
		} else if(action->node->details->online == FALSE) {
			action->runnable = FALSE;
			crm_log_maybe(warn_level, "Action %s on %s is unrunnable (offline)",
				 action->uuid, action->node->details->uname);
			if(action->rsc->is_managed
			   && save_action
			   && a_task == stop_rsc) {
				crm_log_maybe(warn_level, "Marking node %s unclean",
					 action->node->details->uname);
				action->node->details->unclean = TRUE;
			}
			
		} else if(action->needs == rsc_req_nothing) {
			crm_debug_3("Action %s doesnt require anything",
				  action->uuid);
			action->runnable = TRUE;
#if 0
			/*
			 * No point checking this
			 * - if we dont have quorum we cant stonith anyway
			 */
		} else if(action->needs == rsc_req_stonith) {
			crm_debug_3("Action %s requires only stonith", action->uuid);
			action->runnable = TRUE;
#endif
		} else if(data_set->have_quorum == FALSE
			&& data_set->no_quorum_policy == no_quorum_stop) {
			action->runnable = FALSE;
			crm_debug("%s\t%s %s (cancelled : quorum)",
				  action->node->details->uname,
				  action->task, rsc->id);
			
		} else if(data_set->have_quorum == FALSE
			&& data_set->no_quorum_policy == no_quorum_freeze) {
			crm_debug_3("Check resource is already active");
			if(rsc->fns->active(rsc, TRUE) == FALSE) {
				action->runnable = FALSE;
				crm_debug("%s\t%s %s (cancelled : quorum freeze)",
					  action->node->details->uname,
					  action->task, rsc->id);
			}

		} else {
			crm_debug_3("Action %s is runnable", action->uuid);
			action->runnable = TRUE;
		}

		if(save_action) {
			switch(a_task) {
				case stop_rsc:
					rsc->stopping = TRUE;
					break;
				case start_rsc:
					rsc->starting = FALSE;
					if(action->runnable) {
					rsc->starting = TRUE;
					}
					break;
				default:
					break;
			}
		}
	}
	return action;
}

void
unpack_operation(
	action_t *action, crm_data_t *xml_obj, pe_working_set_t* data_set)
{
	int lpc = 0;
	const char *value = NULL;
	const char *fields[] = {
		XML_LRM_ATTR_INTERVAL,
		"timeout",
		"start_delay",
	};

	CRM_CHECK(action->rsc != NULL, return);
	
	if(xml_obj != NULL) {
		value = crm_element_value(xml_obj, "prereq");
	}
	if(value == NULL && safe_str_eq(action->task, CRMD_ACTION_START)) {
		value = g_hash_table_lookup(
			action->rsc->meta, "start_prereq");
	}
	
	if(value == NULL && safe_str_neq(action->task, CRMD_ACTION_START)) {
		/* todo: integrate stop as an option? */
		action->needs = rsc_req_nothing;
		value = "nothing (default)";

	} else if(safe_str_eq(value, "nothing")) {
		action->needs = rsc_req_nothing;

	} else if(safe_str_eq(value, "quorum")) {
		action->needs = rsc_req_quorum;

	} else if(safe_str_eq(value, "fencing")) {
		action->needs = rsc_req_stonith;
		
	} else if(data_set->no_quorum_policy == no_quorum_ignore) {
		action->needs = rsc_req_nothing;
		value = "nothing (default)";
		
	} else if(data_set->no_quorum_policy == no_quorum_freeze
		  && data_set->stonith_enabled) {
		action->needs = rsc_req_stonith;
		value = "fencing (default)";

	} else {
		action->needs = rsc_req_quorum;
		value = "quorum (default)";
	}
	crm_debug_3("\tAction %s requires: %s", action->task, value);

	value = NULL;
	if(xml_obj != NULL) {
		value = crm_element_value(xml_obj, "on_fail");
	}
	if(value == NULL && safe_str_eq(action->task, CRMD_ACTION_STOP)) {
		value = g_hash_table_lookup(
			action->rsc->meta, "on_stopfail");
		if(value != NULL) {
#if CRM_DEPRECATED_SINCE_2_0_2
			pe_config_err("The \"on_stopfail\" attribute used in"
				      " %s has been deprecated since 2.0.2",
				      action->rsc->id);
#else
			pe_config_err("The \"on_stopfail\" attribute used in"
				      " %s has been deprecated since 2.0.2"
				      " and is now disabled", action->rsc->id);
			value = NULL;
#endif
			pe_config_err("Please use specify the \"on_fail\""
				      " attribute on the \"stop\" operation"
				      " instead");
		}
	}
	if(value == NULL) {

	} else if(safe_str_eq(value, "block")) {
		action->on_fail = action_fail_block;

	} else if(safe_str_eq(value, "fence")) {
		action->on_fail = action_fail_fence;
		value = "node fencing";

	} else if(safe_str_eq(value, "ignore")) {
		action->on_fail = action_fail_ignore;
		value = "ignore";

	} else if(safe_str_eq(value, "migrate")) {
		action->on_fail = action_fail_migrate;
		value = "force migration";
		
	} else if(safe_str_eq(value, "stop")) {
		action->fail_role = RSC_ROLE_STOPPED;
		value = "stop resource";
		
	} else if(safe_str_eq(value, "restart")
		|| safe_str_eq(value, "nothing")) {
		action->on_fail = action_fail_recover;
		value = "restart (and possibly migrate)";
		
	} else {
		pe_err("Resource %s: Unknown failure type (%s)",
		       action->rsc->id, value);
		value = NULL;
	}
	
	/* defaults */
	if(value == NULL && safe_str_eq(action->task, CRMD_ACTION_STOP)) {
		if(data_set->stonith_enabled) {
			action->on_fail = action_fail_fence;		
			value = "resource fence (default)";
			
		} else {
			action->on_fail = action_fail_block;		
			value = "resource block (default)";
		}
		
	} else if(value == NULL) {
		action->on_fail = action_fail_recover;		
		value = "restart (and possibly migrate) (default)";
	}
	
	crm_debug_3("\t%s failure handling: %s", action->task, value);

	value = NULL;
	if(xml_obj != NULL) {
		value = crm_element_value(xml_obj, "role_after_failure");
	}
	if(value != NULL && action->fail_role == RSC_ROLE_UNKNOWN) {
		action->fail_role = text2role(value);
	}
	/* defaults */
	if(action->fail_role == RSC_ROLE_UNKNOWN) {
		if(safe_str_eq(action->task, CRMD_ACTION_PROMOTE)) {
			action->fail_role = RSC_ROLE_SLAVE;
		} else {
			action->fail_role = RSC_ROLE_STARTED;
		}
	}
	crm_debug_3("\t%s failure results in: %s",
		    action->task, role2text(action->fail_role));
	
	if(xml_obj == NULL) {
		return;
	}

	xml_prop_iter(xml_obj, p_name, p_value,
		if(p_value != NULL) {
			g_hash_table_insert(action->meta, crm_strdup(p_name),
					    crm_strdup(p_value));
		}
		);

	unpack_instance_attributes(xml_obj, XML_TAG_META_SETS,
				   NULL, action->meta, NULL, data_set->now);
	
	unpack_instance_attributes(xml_obj, XML_TAG_ATTR_SETS,
				   NULL, action->meta, NULL, data_set->now);

	for(;lpc < DIMOF(fields); lpc++) {
		value = g_hash_table_lookup(action->meta, fields[lpc]);
		if(value != NULL) {
			char *tmp_ms = NULL;
			int tmp_i = crm_get_msec(value);
			if(tmp_i < 0) {
				tmp_i = 0;
			}
			tmp_ms = crm_itoa(tmp_i);
			g_hash_table_replace(
				action->meta, crm_strdup(fields[lpc]), tmp_ms);
		}
	}
	
/* 	if(safe_str_eq(native_data->agent->class, "stonith")) { */
/* 		if(rsc->start_needs == rsc_req_stonith) { */
/* 			pe_err("Stonith resources (eg. %s) cannot require" */
/* 			       " fencing to start", rsc->id); */
/* 		} */
/* 		rsc->start_needs = rsc_req_quorum; */
/* 	} */

}

crm_data_t *
find_rsc_op_entry(resource_t *rsc, const char *key) 
{
	const char *name = NULL;
	const char *value = NULL;
	const char *interval = NULL;
	char *match_key = NULL;
	crm_data_t *op = NULL;
	
	xml_child_iter_filter(
		rsc->ops_xml, operation, "op",

		name = crm_element_value(operation, "name");
		interval = crm_element_value(operation, XML_LRM_ATTR_INTERVAL);
		value = crm_element_value(operation, "disabled");
		if(crm_is_true(value)) {
			crm_debug_2("%s disabled", ID(operation));
			continue;
		}
		
		match_key = generate_op_key(
			rsc->id, name, crm_get_msec(interval));

		if(safe_str_eq(key, match_key)) {
			op = operation;
		}
		crm_free(match_key);

		if(op != NULL) {
			return op;
		}
		);
	crm_debug_3("No match for %s", key);
	return op;
}

void
print_node(const char *pre_text, node_t *node, gboolean details)
{ 
	if(node == NULL) {
		crm_debug_4("%s%s: <NULL>",
		       pre_text==NULL?"":pre_text,
		       pre_text==NULL?"":": ");
		return;
	}

	crm_debug_4("%s%s%sNode %s: (weight=%d, fixed=%s)",
	       pre_text==NULL?"":pre_text,
	       pre_text==NULL?"":": ",
	       node->details==NULL?"error ":node->details->online?"":"Unavailable/Unclean ",
	       node->details->uname, 
	       node->weight,
	       node->fixed?"True":"False"); 

	if(details && node != NULL && node->details != NULL) {
		char *pe_mutable = crm_strdup("\t\t");
		crm_debug_4("\t\t===Node Attributes");
		g_hash_table_foreach(node->details->attrs,
				     print_str_str, pe_mutable);
		crm_free(pe_mutable);

		crm_debug_4("\t\t=== Resources");
		slist_iter(
			rsc, resource_t, node->details->running_rsc, lpc,
			print_resource(LOG_DEBUG_4, "\t\t", rsc, FALSE);
			);
	}
}

/*
 * Used by the HashTable for-loop
 */
void print_str_str(gpointer key, gpointer value, gpointer user_data)
{
	crm_debug_4("%s%s %s ==> %s",
	       user_data==NULL?"":(char*)user_data,
	       user_data==NULL?"":": ",
	       (char*)key,
	       (char*)value);
}

void
print_color_details(const char *pre_text,
		    struct color_shared_s *color,
		    gboolean details)
{ 
	if(color == NULL) {
		crm_debug_4("%s%s: <NULL>",
		       pre_text==NULL?"":pre_text,
		       pre_text==NULL?"":": ");
		return;
	}
	crm_debug_4("%s%sColor %d: node=%s (from %d candidates)",
	       pre_text==NULL?"":pre_text,
	       pre_text==NULL?"":": ",
	       color->id, 
	       color->chosen_node==NULL?"<unset>":color->chosen_node->details->uname,
	       g_list_length(color->candidate_nodes)); 
	if(details) {
		slist_iter(node, node_t, color->candidate_nodes, lpc,
			   print_node("\t", node, FALSE));
	}
}

void
print_color(const char *pre_text, color_t *color, gboolean details)
{ 
	if(color == NULL) {
		crm_debug_4("%s%s: <NULL>",
		       pre_text==NULL?"":pre_text,
		       pre_text==NULL?"":": ");
		return;
	}
	crm_debug_4("%s%sColor %d: (weight=%d, node=%s, possible=%d)",
		    pre_text==NULL?"":pre_text,
		    pre_text==NULL?"":": ",
		    color->id, 
		    color->local_weight,
		    safe_val5("<unset>",color,details,chosen_node,details,uname),
		    g_list_length(color->details->candidate_nodes)); 
	if(details) {
		print_color_details("\t", color->details, details);
	}
}

void
print_rsc_to_node(const char *pre_text, rsc_to_node_t *cons, gboolean details)
{ 
	if(cons == NULL) {
		crm_debug_4("%s%s: <NULL>",
		       pre_text==NULL?"":pre_text,
		       pre_text==NULL?"":": ");
		return;
	}
	crm_debug_4("%s%s%s Constraint %s (%p) - %d nodes:",
		    pre_text==NULL?"":pre_text,
		    pre_text==NULL?"":": ",
		    "rsc_to_node",
		    cons->id, cons,
		    g_list_length(cons->node_list_rh));

	if(details == FALSE) {
		crm_debug_4("\t%s (node placement rule)",
			  safe_val3(NULL, cons, rsc_lh, id));

		slist_iter(
			node, node_t, cons->node_list_rh, lpc,
			print_node("\t\t-->", node, FALSE)
			);
	}
}

void
print_rsc_colocation(const char *pre_text, rsc_colocation_t *cons, gboolean details)
{ 
	if(cons == NULL) {
		crm_debug_4("%s%s: <NULL>",
		       pre_text==NULL?"":pre_text,
		       pre_text==NULL?"":": ");
		return;
	}
	crm_debug_4("%s%s%s Constraint %s (%p):",
	       pre_text==NULL?"":pre_text,
	       pre_text==NULL?"":": ",
	       XML_CONS_TAG_RSC_DEPEND, cons->id, cons);

	if(details == FALSE) {

		crm_debug_4("\t%s --> %s, %s",
			  safe_val3(NULL, cons, rsc_lh, id), 
			  safe_val3(NULL, cons, rsc_rh, id), 
			  strength2text(cons->strength));
	}
} 

void
print_resource(
	int log_level, const char *pre_text, resource_t *rsc, gboolean details)
{
	long options = pe_print_log;
	
	if(rsc == NULL) {
		crm_log_maybe(log_level-1, "%s%s: <NULL>",
			      pre_text==NULL?"":pre_text,
			      pre_text==NULL?"":": ");
		return;
	}
	if(details) {
		options |= pe_print_details;
	}
	rsc->fns->print(rsc, pre_text, options, &log_level);
}

void
log_action(unsigned int log_level, const char *pre_text, action_t *action, gboolean details)
{
	const char *node_uname = NULL;
	const char *node_uuid = NULL;
	
	if(action == NULL) {

		crm_log_maybe(log_level, "%s%s: <NULL>",
			      pre_text==NULL?"":pre_text,
			      pre_text==NULL?"":": ");
		return;
	}


	if(action->pseudo) {
		node_uname = NULL;
		node_uuid = NULL;
		
	} else if(action->node != NULL) {
		node_uname = action->node->details->uname;
		node_uuid = action->node->details->id;
	} else {
		node_uname = "<none>";
		node_uuid = NULL;
	}
	
	switch(text2task(action->task)) {
		case stonith_node:
		case shutdown_crm:
			crm_log_maybe(log_level,
				      "%s%s%sAction %d: %s%s%s%s%s%s",
				      pre_text==NULL?"":pre_text,
				      pre_text==NULL?"":": ",
				      action->pseudo?"Pseduo ":action->optional?"Optional ":action->runnable?action->processed?"":"(Provisional) ":"!!Non-Startable!! ",
				      action->id, action->uuid,
				      node_uname?"\ton ":"",
				      node_uname?node_uname:"",
				      node_uuid?"\t\t(":"",
				      node_uuid?node_uuid:"",
				      node_uuid?")":"");
			break;
		default:
			crm_log_maybe(log_level,
				      "%s%s%sAction %d: %s %s%s%s%s%s%s",
				      pre_text==NULL?"":pre_text,
				      pre_text==NULL?"":": ",
				      action->optional?"Optional ":action->pseudo?"Pseduo ":action->runnable?action->processed?"":"(Provisional) ":"!!Non-Startable!! ",
				      action->id, action->uuid,
				      safe_val3("<none>", action, rsc, id),
				      node_uname?"\ton ":"",
				      node_uname?node_uname:"",
				      node_uuid?"\t\t(":"",
				      node_uuid?node_uuid:"",
				      node_uuid?")":"");
			
			break;
	}

	if(details) {
		crm_log_maybe(log_level+1, "\t\t====== Preceeding Actions");
		slist_iter(
			other, action_wrapper_t, action->actions_before, lpc,
			log_action(log_level+1, "\t\t", other->action, FALSE);
			);
#if 1
		crm_log_maybe(log_level+1, "\t\t====== Subsequent Actions");
		slist_iter(
			other, action_wrapper_t, action->actions_after, lpc,
			log_action(log_level+1, "\t\t", other->action, FALSE);
			);		
#endif
		crm_log_maybe(log_level+1, "\t\t====== End");

	} else {
		crm_log_maybe(log_level, "\t\t(seen=%d, before=%d, after=%d)",
			      action->seen_count,
			      g_list_length(action->actions_before),
			      g_list_length(action->actions_after));
	}
}


void
pe_free_nodes(GListPtr nodes)
{
	GListPtr iterator = nodes;
	while(iterator != NULL) {
		node_t *node = (node_t*)iterator->data;
		struct node_shared_s *details = node->details;
		iterator = iterator->next;

		crm_debug_5("deleting node");
		crm_debug_5("%s is being deleted", details->uname);
		print_node("delete", node, FALSE);
		
		if(details != NULL) {
			if(details->attrs != NULL) {
				g_hash_table_destroy(details->attrs);
			}
			pe_free_shallow_adv(details->running_rsc, FALSE);
			crm_free(details);
		}
		crm_free(node);
	}
	if(nodes != NULL) {
		g_list_free(nodes);
	}
}

void
pe_free_colors(GListPtr colors)
{
	GListPtr iterator = colors;
	while(iterator != NULL) {
		color_t *color = (color_t *)iterator->data;
		struct color_shared_s *details = color->details;
		iterator = iterator->next;
		
		if(details != NULL) {
			pe_free_shallow(details->candidate_nodes);
			pe_free_shallow_adv(details->allocated_resources, FALSE);
			crm_free(details->chosen_node);
			crm_free(details);
		}
		crm_free(color);
	}
	if(colors != NULL) {
		g_list_free(colors);
	}
}


void
pe_free_resources(GListPtr resources)
{ 
	resource_t *rsc = NULL;
	GListPtr iterator = resources;
	while(iterator != NULL) {
		iterator = iterator;
		rsc = (resource_t *)iterator->data;
		iterator = iterator->next;
		rsc->fns->free(rsc);
	}
	if(resources != NULL) {
		g_list_free(resources);
	}
}

void
pe_free_action(action_t *action) 
{
	if(action == NULL) {
		return;
	}
	pe_free_shallow(action->actions_before);/* action_warpper_t* */
	pe_free_shallow(action->actions_after); /* action_warpper_t* */
	g_hash_table_destroy(action->extra);
	g_hash_table_destroy(action->meta);
	crm_free(action->uuid);
	crm_free(action);
}

void
pe_free_actions(GListPtr actions) 
{
	GListPtr iterator = actions;
	while(iterator != NULL) {
		pe_free_action(iterator->data);
		iterator = iterator->next;
	}
	if(actions != NULL) {
		g_list_free(actions);
	}
}

void
pe_free_ordering(GListPtr constraints) 
{
	GListPtr iterator = constraints;
	while(iterator != NULL) {
		order_constraint_t *order = iterator->data;
		iterator = iterator->next;

		crm_free(order->lh_action_task);
		crm_free(order->rh_action_task);
		crm_free(order);
	}
	if(constraints != NULL) {
		g_list_free(constraints);
	}
}


void
pe_free_rsc_colocation(rsc_colocation_t *cons)
{ 
	if(cons != NULL) {
		crm_debug_4("Freeing constraint %s (%p)", cons->id, cons);
		crm_free(cons);
	}
}

void
pe_free_rsc_to_node(rsc_to_node_t *cons)
{
	if(cons != NULL) {
		pe_free_shallow(cons->node_list_rh);
		crm_free(cons);
	}
}

GListPtr
find_recurring_actions(GListPtr input, node_t *not_on_node)
{
	const char *value = NULL;
	GListPtr result = NULL;
	CRM_CHECK(input != NULL, return NULL);
	
	slist_iter(
		action, action_t, input, lpc,
		value = g_hash_table_lookup(action->meta, XML_LRM_ATTR_INTERVAL);
		if(value == NULL) {
			/* skip */
		} else if(safe_str_eq(CRMD_ACTION_CANCEL, action->task)) {
			/* skip */
		} else if(not_on_node == NULL) {
			crm_debug_5("(null) Found: %s", action->uuid);
			result = g_list_append(result, action);
			
		} else if(action->node == NULL) {
			/* skip */
		} else if(action->node->details != not_on_node->details) {
			crm_debug_5("Found: %s", action->uuid);
			result = g_list_append(result, action);
		}
		);

	return result;
}

GListPtr
find_actions(GListPtr input, const char *key, node_t *on_node)
{
	GListPtr result = NULL;
	CRM_CHECK(key != NULL, return NULL);
	
	slist_iter(
		action, action_t, input, lpc,
		crm_debug_5("Matching %s against %s", key, action->uuid);
		if(safe_str_neq(key, action->uuid)) {
			continue;
			
		} else if(on_node == NULL) {
			result = g_list_append(result, action);
			
		} else if(action->node == NULL) {
			/* skip */
			crm_debug_2("While looking for %s action on %s, "
				    "found an unallocated one.  Assigning"
				    " it to the requested node...",
				    key, on_node->details->uname);

			action->node = on_node;
			result = g_list_append(result, action);
			
		} else if(safe_str_eq(on_node->details->id,
				      action->node->details->id)) {
			result = g_list_append(result, action);
		}
		);

	return result;
}


GListPtr
find_actions_exact(GListPtr input, const char *key, node_t *on_node)
{
	GListPtr result = NULL;
	CRM_CHECK(key != NULL, return NULL);
	
	slist_iter(
		action, action_t, input, lpc,
		crm_debug_5("Matching %s against %s", key, action->uuid);
		if(safe_str_neq(key, action->uuid)) {
			crm_debug_3("Key mismatch: %s vs. %s",
				    key, action->uuid);
			continue;
			
		} else if(on_node == NULL  || action->node == NULL) {
			crm_debug_3("on_node=%p, action->node=%p",
				    on_node, action->node);
			continue;

		} else if(safe_str_eq(on_node->details->id,
				      action->node->details->id)) {
			result = g_list_append(result, action);
		}
		crm_debug_2("Node mismatch: %s vs. %s",
			    on_node->details->id, action->node->details->id);
		);

	return result;
}

void
set_id(crm_data_t * xml_obj, const char *prefix, int child) 
{
	int id_len = 0;
	gboolean use_prefix = TRUE;
	gboolean use_child = TRUE;

	char *new_id   = NULL;
	const char *id = crm_element_value(xml_obj, XML_ATTR_ID);
	
	id_len = 1 + strlen(id);

	if(child > 999) {
		pe_err("Are you insane?!?"
			" The CRM does not support > 1000 children per resource");
		return;
		
	} else if(child < 0) {
		use_child = FALSE;
		
	} else {
		id_len += 4; /* child */
	}
	
	if(prefix == NULL || safe_str_eq(id, prefix)) {
		use_prefix = FALSE;
	} else {
		id_len += (1 + strlen(prefix));
	}
	
	crm_malloc0(new_id, id_len);

	if(use_child) {
		snprintf(new_id, id_len, "%s%s%s:%d",
			 use_prefix?prefix:"", use_prefix?":":"", id, child);
	} else {
		snprintf(new_id, id_len, "%s%s%s",
			 use_prefix?prefix:"", use_prefix?":":"", id);
	}
	
	crm_xml_add(xml_obj, XML_ATTR_ID, new_id);
	crm_free(new_id);
}

rsc_to_node_t *
rsc2node_new(const char *id, resource_t *rsc,
	     int node_weight, node_t *foo_node, pe_working_set_t *data_set)
{
	rsc_to_node_t *new_con = NULL;

	if(rsc == NULL || id == NULL) {
		pe_err("Invalid constraint %s for rsc=%p", crm_str(id), rsc);
		return NULL;
	}

	crm_malloc0(new_con, sizeof(rsc_to_node_t));
	if(new_con != NULL) {
		new_con->id           = id;
		new_con->rsc_lh       = rsc;
		new_con->node_list_rh = NULL;
		new_con->role_filter = RSC_ROLE_UNKNOWN;
		
		if(foo_node != NULL) {
			node_t *copy = node_copy(foo_node);
			copy->weight = node_weight;
			new_con->node_list_rh = g_list_append(NULL, copy);
		} else {
			CRM_CHECK(node_weight == 0, return NULL);
		}
		
		data_set->placement_constraints = g_list_append(
			data_set->placement_constraints, new_con);
		rsc->rsc_location = g_list_append(
			rsc->rsc_location, new_con);
	}
	
	return new_con;
}



const char *
strength2text(enum con_strength strength)
{
	const char *result = "<unknown>";
	switch(strength)
	{
		case pecs_ignore:
			result = "ignore";
			break;
		case pecs_must:
			result = XML_STRENGTH_VAL_MUST;
			break;
		case pecs_must_not:
			result = XML_STRENGTH_VAL_MUSTNOT;
			break;
		case pecs_startstop:
			result = "start/stop";
			break;
	}
	return result;
}

const char *
ordering_type2text(enum pe_ordering type)
{
	const char *result = "<unknown>";
	switch(type)
	{
		case pe_ordering_manditory:
			result = "manditory";
			break;
		case pe_ordering_restart:
			result = "restart";
			break;
		case pe_ordering_recover:
			result = "recover";
			break;
		case pe_ordering_optional:
			result = "optional";
			break;
		case pe_ordering_postnotify:
			result = "post_notify";
			break;
	}
	return result;
}