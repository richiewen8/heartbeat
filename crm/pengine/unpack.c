/* $Id: unpack.c,v 1.143 2005/10/26 11:34:49 andrew Exp $ */
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
#include <crm/common/msg.h>
#include <clplumbing/cl_misc.h>

#include <lrm/lrm_api.h>

#include <glib.h>

#include <heartbeat.h> /* for ONLINESTATUS */

#include <pengine.h>
#include <pe_utils.h>
#include <pe_rules.h>

gint sort_op_by_callid(gconstpointer a, gconstpointer b);

gboolean unpack_rsc_to_attr(crm_data_t *xml_obj, pe_working_set_t *data_set);

gboolean unpack_rsc_to_node(crm_data_t *xml_obj, pe_working_set_t *data_set);

gboolean unpack_rsc_order(crm_data_t *xml_obj, pe_working_set_t *data_set);

gboolean unpack_rsc_colocation(crm_data_t *xml_obj, pe_working_set_t *data_set);

gboolean unpack_rsc_location(crm_data_t *xml_obj, pe_working_set_t *data_set);

gboolean unpack_lrm_rsc_state(
	node_t *node, crm_data_t * lrm_state, pe_working_set_t *data_set);

gboolean add_node_attrs(
	crm_data_t * attrs, node_t *node, pe_working_set_t *data_set);

gboolean unpack_rsc_op(
	resource_t *rsc, node_t *node, crm_data_t *xml_op,
	int *max_call_id, pe_working_set_t *data_set);

gboolean determine_online_status(
	crm_data_t * node_state, node_t *this_node, pe_working_set_t *data_set);

gboolean rsc_colocation_new(
	const char *id, enum con_strength strength,
	resource_t *rsc_lh, resource_t *rsc_rh,
	const char *state_lh, const char *state_rh);

gboolean create_ordering(
	const char *id, enum con_strength strength,
	resource_t *rsc_lh, resource_t *rsc_rh, pe_working_set_t *data_set);

const char *param_value(
	GHashTable *hash, crm_data_t * parent, const char *name);

rsc_to_node_t *generate_location_rule(
	resource_t *rsc, crm_data_t *location_rule, pe_working_set_t *data_set);

#define get_cluster_pref(pref) value = g_hash_table_lookup(config_hash, pref); \
	if(value == NULL) {						\
		pe_config_warn("No value specified for cluster preference: %s", pref); \
	}

gboolean
unpack_config(crm_data_t * config, pe_working_set_t *data_set)
{
/* 	const char *attr_filter[] = { */
/* 		"default_resource_stickiness", */
/* 		"transition_idle_timeout", */
/* 		"stonith_enabled", */
/* 		"symmetric_cluster" */
/* 	}; */
	
	const char *value = NULL;
	GHashTable *config_hash = g_hash_table_new_full(
		g_str_hash,g_str_equal, g_hash_destroy_str,g_hash_destroy_str);

	data_set->config_hash = config_hash;	

	unpack_instance_attributes(
		config, "cluster_property_set", NULL, config_hash,
		NULL, 0, data_set);

#if CRM_DEPRECATED_SINCE_2_0_1
	param_value(config_hash, config, "transition_idle_timeout");
	param_value(config_hash, config, "default_resource_stickiness");
	param_value(config_hash, config, "stonith_enabled");
	param_value(config_hash, config, "symmetric_cluster");
	param_value(config_hash, config, "no_quorum_policy");
	param_value(config_hash, config, "stop_orphan_resources");
	param_value(config_hash, config, "stop_orphan_actions");
	param_value(config_hash, config, "remove_after_stop");
	param_value(config_hash, config, "is_managed_default");
#endif
	get_cluster_pref("transition_idle_timeout");
	if(value != NULL) {
		long tmp = crm_get_msec(value);
		if(tmp > 0) {
			crm_free(data_set->transition_idle_timeout);
			data_set->transition_idle_timeout = crm_strdup(value);
		} else {
			crm_err("Invalid value for transition_idle_timeout: %s",
				value);
		}
	}
	
	crm_debug_4("%s set to: %s",
		 "transition_idle_timeout", data_set->transition_idle_timeout);

	get_cluster_pref("default_resource_stickiness");
	data_set->default_resource_stickiness = char2score(value);
	
	get_cluster_pref("stonith_enabled");
	if(value != NULL) {
		cl_str_to_boolean(value, &data_set->stonith_enabled);
	}
	crm_info("STONITH of failed nodes is %s",
		 data_set->stonith_enabled?"enabled":"disabled");	
	
	get_cluster_pref("symmetric_cluster");
	if(value != NULL) {
		cl_str_to_boolean(value, &data_set->symmetric_cluster);
	}
	if(data_set->symmetric_cluster) {
		crm_info("Cluster is symmetric"
			 " - resources can run anywhere by default");
	}

	get_cluster_pref("no_quorum_policy");
	if(safe_str_eq(value, "ignore")) {
		data_set->no_quorum_policy = no_quorum_ignore;
		
	} else if(safe_str_eq(value, "freeze")) {
		data_set->no_quorum_policy = no_quorum_freeze;

	} else {
		data_set->no_quorum_policy = no_quorum_stop;
	}
	
	switch (data_set->no_quorum_policy) {
		case no_quorum_freeze:
			crm_info("On loss of CCM Quorum: Freeze resources");
			break;
		case no_quorum_stop:
			crm_info("On loss of CCM Quorum: Stop ALL resources");
			break;
		case no_quorum_ignore:
			crm_notice("On loss of CCM Quorum: Ignore");
			break;
	}

	get_cluster_pref("stop_orphan_resources");
	if(value != NULL) {
		cl_str_to_boolean(value, &data_set->stop_rsc_orphans);
	}
	crm_info("Orphan resources are %s",
		 data_set->stop_rsc_orphans?"stopped":"ignored");	
	
	get_cluster_pref("stop_orphan_actions");
	if(value != NULL) {
		cl_str_to_boolean(value, &data_set->stop_action_orphans);
	}
	crm_info("Orphan resource actions are %s",
		 data_set->stop_action_orphans?"stopped":"ignored");	

	get_cluster_pref("remove_after_stop");
	if(value != NULL) {
		cl_str_to_boolean(value, &data_set->remove_after_stop);
	}
	crm_info("Orphan resource actions are %s",
		 data_set->remove_after_stop?"true":"false");	
	
	get_cluster_pref("is_managed_default");
	if(value != NULL) {
		cl_str_to_boolean(value, &data_set->is_managed_default);
	}
	crm_info("By default resources are %smanaged",
		 data_set->is_managed_default?"":"not ");

	return TRUE;
}

const char *
param_value(GHashTable *hash, crm_data_t * parent, const char *name) 
{
	const char *value = NULL;
	crm_data_t * a_default = NULL;

	if(parent != NULL) {
		a_default = find_entity(parent, XML_CIB_TAG_NVPAIR, name);
	}
	
	if(a_default == NULL) {
		return NULL;
	}
	
	value = crm_element_value(a_default, XML_NVPAIR_ATTR_VALUE);
	if(value && hash) {
		if(g_hash_table_lookup(hash, name) == NULL) {
			g_hash_table_insert(
				hash, crm_strdup(name), crm_strdup(value));
		}
	}
	return value;
}

gboolean
unpack_nodes(crm_data_t * xml_nodes, pe_working_set_t *data_set)
{
	node_t *new_node   = NULL;
	const char *id     = NULL;
	const char *uname  = NULL;
	const char *type   = NULL;

	crm_debug("Begining unpack... %s",
		    xml_nodes?crm_element_name(xml_nodes):"<none>");
	xml_child_iter(
		xml_nodes, xml_obj, XML_CIB_TAG_NODE,

		new_node = NULL;

		id     = crm_element_value(xml_obj, XML_ATTR_ID);
		uname  = crm_element_value(xml_obj, XML_ATTR_UNAME);
		type   = crm_element_value(xml_obj, XML_ATTR_TYPE);
		crm_debug_3("Processing node %s/%s", uname, id);

		if(id == NULL) {
			pe_config_err("Must specify id tag in <node>");
			continue;
		}
		if(type == NULL) {
			pe_config_err("Must specify type tag in <node>");
			continue;
		}
		crm_malloc0(new_node, sizeof(node_t));
		if(new_node == NULL) {
			return FALSE;
		}
		
		new_node->weight = 0;
		new_node->fixed  = FALSE;
		crm_malloc0(new_node->details,
			   sizeof(struct node_shared_s));

		if(new_node->details == NULL) {
			crm_free(new_node);
			return FALSE;
		}

		crm_debug_3("Creaing node for entry %s/%s", uname, id);
		new_node->details->id		= id;
		new_node->details->uname	= uname;
		new_node->details->type		= node_ping;
		new_node->details->online	= FALSE;
		new_node->details->shutdown	= FALSE;
		new_node->details->running_rsc	= NULL;
		new_node->details->attrs        = g_hash_table_new_full(
			g_str_hash, g_str_equal,
			g_hash_destroy_str, g_hash_destroy_str);
		
/* 		if(data_set->have_quorum == FALSE */
/* 		   && data_set->no_quorum_policy == no_quorum_stop) { */
/* 			/\* start shutting resources down *\/ */
/* 			new_node->weight = -INFINITY; */
/* 		} */
		
		if(data_set->stonith_enabled) {
			/* all nodes are unclean until we've seen their
			 * status entry
			 */
			new_node->details->unclean = TRUE;
		} else {
			/* blind faith... */
			new_node->details->unclean = FALSE; 
		}
		
		
		if(safe_str_eq(type, "member")) {
			new_node->details->type = node_member;
		}

		add_node_attrs(xml_obj, new_node, data_set);

		if(crm_is_true(g_hash_table_lookup(
				       new_node->details->attrs, "standby"))) {
			crm_info("Node %s is in standby-mode",
				 new_node->details->uname);
			new_node->weight = -INFINITY;
			new_node->details->standby = TRUE;
		}
		
		data_set->nodes = g_list_append(data_set->nodes, new_node);    
		crm_debug_3("Done with node %s",
			    crm_element_value(xml_obj, XML_ATTR_UNAME));

		crm_action_debug_3(print_node("Added", new_node, FALSE));
		);
  
/* 	data_set->nodes = g_list_sort(data_set->nodes, sort_node_weight); */

	return TRUE;
}

gboolean 
unpack_resources(crm_data_t * xml_resources, pe_working_set_t *data_set)
{
	crm_debug("Begining unpack... %s",
		    xml_resources?crm_element_name(xml_resources):"<none>");
	xml_child_iter(
		xml_resources, xml_obj, NULL,

		resource_t *new_rsc = NULL;
		crm_debug_2("Begining unpack... %s",
			    xml_obj?crm_element_name(xml_obj):"<none>");
		if(common_unpack(xml_obj, &new_rsc, NULL, data_set)) {
			data_set->resources = g_list_append(
				data_set->resources, new_rsc);
			
			print_resource(LOG_DEBUG_3, "Added", new_rsc, FALSE);

		} else {
			pe_config_err("Failed unpacking %s %s",
				      crm_element_name(xml_obj),
				      crm_element_value(xml_obj, XML_ATTR_ID));
		}
		);
	
	data_set->resources = g_list_sort(
		data_set->resources, sort_rsc_priority);

	return TRUE;
}

gboolean 
unpack_constraints(crm_data_t * xml_constraints, pe_working_set_t *data_set)
{
	crm_data_t *lifetime = NULL;
	crm_debug("Begining unpack... %s",
		    xml_constraints?crm_element_name(xml_constraints):"<none>");
	xml_child_iter(
		xml_constraints, xml_obj, NULL,

		const char *id = crm_element_value(xml_obj, XML_ATTR_ID);
		if(id == NULL) {
			pe_config_err("Constraint <%s...> must have an id",
				crm_element_name(xml_obj));
			continue;
		}

		crm_debug_3("Processing constraint %s %s",
			    crm_element_name(xml_obj),id);

		lifetime = cl_get_struct(xml_obj, "lifetime");

		if(test_ruleset(lifetime, NULL, data_set) == FALSE) {
			crm_info("Constraint %s %s is not active",
				 crm_element_name(xml_obj), id);

		} else if(safe_str_eq(XML_CONS_TAG_RSC_ORDER,
				      crm_element_name(xml_obj))) {
			unpack_rsc_order(xml_obj, data_set);

		} else if(safe_str_eq(XML_CONS_TAG_RSC_DEPEND,
				      crm_element_name(xml_obj))) {
			unpack_rsc_colocation(xml_obj, data_set);

		} else if(safe_str_eq(XML_CONS_TAG_RSC_LOCATION,
				      crm_element_name(xml_obj))) {
			unpack_rsc_location(xml_obj, data_set);

		} else {
			pe_err("Unsupported constraint type: %s",
				crm_element_name(xml_obj));
		}
		);

	return TRUE;
}





/* remove nodes that are down, stopping */
/* create +ve rsc_to_node constraints between resources and the nodes they are running on */
/* anything else? */
gboolean
unpack_status(crm_data_t * status, pe_working_set_t *data_set)
{
	const char *id    = NULL;
	const char *uname = NULL;

	crm_data_t * lrm_rsc    = NULL;
	crm_data_t * attrs      = NULL;
	node_t    *this_node  = NULL;
	
	crm_debug_3("Begining unpack");
	xml_child_iter(
		status, node_state, XML_CIB_TAG_STATE,

		id         = crm_element_value(node_state, XML_ATTR_ID);
		uname = crm_element_value(node_state,    XML_ATTR_UNAME);
		attrs = find_xml_node(
			node_state, XML_TAG_TRANSIENT_NODEATTRS, FALSE);

		lrm_rsc = find_xml_node(node_state, XML_CIB_TAG_LRM, FALSE);
		lrm_rsc = find_xml_node(lrm_rsc, XML_LRM_TAG_RESOURCES, FALSE);

		crm_debug_3("Processing node %s", uname);
		this_node = pe_find_node_id(data_set->nodes, id);

		if(uname == NULL) {
			/* error */
			continue;

		} else if(this_node == NULL) {
			pe_config_warn("Node %s in status section no longer exists",
				       uname);
			continue;
		}

		/* Mark the node as provisionally clean
		 * - at least we have seen it in the current cluster's lifetime
		 */
		this_node->details->unclean = FALSE;
		
		crm_debug_3("Adding runtime node attrs");
		add_node_attrs(attrs, this_node, data_set);

		crm_debug_3("determining node state");
		determine_online_status(node_state, this_node, data_set);

		if(this_node->details->online || data_set->stonith_enabled) {
			/* offline nodes run no resources...
			 * unless stonith is enabled in which case we need to
			 *   make sure rsc start events happen after the stonith
			 */
			crm_debug_3("Processing lrm resource entries");
			unpack_lrm_rsc_state(this_node, lrm_rsc, data_set);
		}
		
		);

	return TRUE;
	
}

gboolean
determine_online_status(
	crm_data_t * node_state, node_t *this_node, pe_working_set_t *data_set)
{
	gboolean online = FALSE;
	const char *uname      = crm_element_value(node_state,XML_ATTR_UNAME);
	const char *exp_state  =
		crm_element_value(node_state, XML_CIB_ATTR_EXPSTATE);
	const char *join_state =
		crm_element_value(node_state, XML_CIB_ATTR_JOINSTATE);
	const char *crm_state  =
		crm_element_value(node_state, XML_CIB_ATTR_CRMDSTATE);
	const char *ccm_state  =
		crm_element_value(node_state, XML_CIB_ATTR_INCCM);
	const char *ha_state   =
		crm_element_value(node_state, XML_CIB_ATTR_HASTATE);
	const char *shutdown   =
		crm_element_value(node_state, XML_CIB_ATTR_SHUTDOWN);

	if(this_node == NULL) {
		pe_config_err("No node to check");
		return online;
	}

	if(safe_str_eq(exp_state, CRMD_JOINSTATE_MEMBER)) {
		this_node->details->expected_up = TRUE;
	}
	if(shutdown != NULL) {
		this_node->details->shutdown = TRUE;
#if 0
		this_node->details->expected_up = FALSE;
#endif
	}

	if(data_set->stonith_enabled == FALSE) {
		if(!crm_is_true(ccm_state) || safe_str_eq(ha_state,DEADSTATUS)){
			crm_debug_2("Node is down: ha_state=%s, ccm_state=%s",
				  crm_str(ha_state), crm_str(ccm_state));
			
		} else if(!crm_is_true(ccm_state)
			  || safe_str_eq(ha_state, DEADSTATUS)) {
			
		} else if(safe_str_neq(join_state, CRMD_JOINSTATE_DOWN)
			  && safe_str_eq(crm_state, ONLINESTATUS)) {
			online = TRUE;
			
		} else if(this_node->details->expected_up == FALSE) {
			crm_debug_2("CRMd is down: ha_state=%s, ccm_state=%s",
				  crm_str(ha_state), crm_str(ccm_state));
			crm_debug_2("\tcrm_state=%s, join_state=%s, expected=%s",
				  crm_str(crm_state), crm_str(join_state),
				  crm_str(exp_state));
			
		} else {
			/* mark it unclean */
			this_node->details->unclean = TRUE;
			
			pe_err("Node %s is partially & un-expectedly down",
				uname);
			crm_info("\tha_state=%s, ccm_state=%s,"
				 " crm_state=%s, join_state=%s, expected=%s",
				 crm_str(ha_state), crm_str(ccm_state),
				 crm_str(crm_state), crm_str(join_state),
				 crm_str(exp_state));
		}
	} else {
		if(crm_is_true(ccm_state)
		   && (ha_state == NULL || safe_str_eq(ha_state, ACTIVESTATUS))
		   && safe_str_eq(crm_state, ONLINESTATUS)
		   && safe_str_neq(join_state, CRMD_JOINSTATE_DOWN)) {
			online = TRUE;

		} else if(this_node->details->expected_up == FALSE) {
			crm_debug_2("CRMd on %s is down: ha_state=%s, ccm_state=%s",
				  uname, crm_str(ha_state), crm_str(ccm_state));
			crm_debug_2("\tcrm_state=%s, join_state=%s, expected=%s",
				  crm_str(crm_state), crm_str(join_state),
				  crm_str(exp_state));
			
		} else {
			/* mark it unclean */
			this_node->details->unclean = TRUE;
			
			pe_err("Node %s is un-expectedly down", uname);
			crm_info("\tha_state=%s, ccm_state=%s,"
				 " crm_state=%s, join_state=%s, expected=%s",
				 crm_str(ha_state), crm_str(ccm_state),
				 crm_str(crm_state), crm_str(join_state),
				 crm_str(exp_state));
		}
	}
	
	if(online) {
		crm_debug_2("Node %s is online", uname);
		this_node->details->online = TRUE;

	} else {
		/* remove node from contention */
		crm_debug_2("Node %s is down", uname);
		this_node->weight = -INFINITY;
		this_node->fixed = TRUE;
	}

	if(this_node->details->unclean) {
		pe_proc_warn("Node %s is unclean", uname);
	}

	if(this_node->details->shutdown) {
		/* dont run resources here */
		this_node->weight = -INFINITY;
		this_node->fixed = TRUE;
		crm_debug_2("Node %s is due for shutdown", uname);
	}
	
	return online;
}

#define set_char(x) last_rsc_id[len] = x; complete = TRUE;

static void
increment_clone(char *last_rsc_id)
{
	gboolean complete = FALSE;
	int len = 0;

	CRM_DEV_ASSERT(last_rsc_id != NULL);
	if(last_rsc_id != NULL) {
		len = strlen(last_rsc_id);
	}
	len--;
	while(complete == FALSE && len > 0) {
		switch (last_rsc_id[len]) {
			case 0:
				len--;
				break;
			case '0':
				set_char('1');
				break;
			case '1':
				set_char('2');
				break;
			case '2':
				set_char('3');
				break;
			case '3':
				set_char('4');
				break;
			case '4':
				set_char('5');
				break;
			case '5':
				set_char('6');
				break;
			case '6':
				set_char('7');
				break;
			case '7':
				set_char('8');
				break;
			case '8':
				set_char('9');
				break;
			case '9':
				last_rsc_id[len] = '0';
				len--;
				break;
			default:
				crm_err("Unexpected char: %c (%d)",
					last_rsc_id[len], len);
				break;
		}
	}
}

extern gboolean DeleteRsc(resource_t *rsc, node_t *node, pe_working_set_t *data_set);

gboolean
unpack_lrm_rsc_state(node_t *node, crm_data_t * lrm_rsc_list,
		     pe_working_set_t *data_set)
{
	gboolean delete_resource = FALSE;
	enum rsc_role_e saved_role = RSC_ROLE_UNKNOWN;
	const char *rsc_id    = NULL;
	const char *node_id   = node->details->uname;
	const char *rsc_state = NULL;

	int max_call_id = -1;

	gboolean is_duped_clone = FALSE;
	resource_t *rsc   = NULL;
	GListPtr op_list = NULL;
	GListPtr sorted_op_list = NULL;
	char *alt_rsc_id = NULL;

	const char *value = NULL;
	const char *old_value = NULL;
	const char *attr_list[] = {
		XML_ATTR_TYPE, 
		XML_AGENT_ATTR_CLASS,
 		XML_AGENT_ATTR_PROVIDER
	};
	
	CRM_DEV_ASSERT(node != NULL);
	if(crm_assert_failed) {
		return FALSE;
	}
	
	xml_child_iter(
		lrm_rsc_list, rsc_entry, XML_LRM_TAG_RESOURCE,
		
		delete_resource = FALSE;
		rsc_id    = crm_element_value(rsc_entry, XML_ATTR_ID);
		rsc_state = crm_element_value(rsc_entry, XML_LRM_ATTR_RSCSTATE);
		
		rsc = NULL;
		is_duped_clone = FALSE;
		alt_rsc_id = crm_strdup(rsc_id);

		while(rsc == NULL) {
			crm_debug_3("looking for: %s", alt_rsc_id);
			rsc = pe_find_resource(data_set->resources, alt_rsc_id);
			/* no match */
			if(rsc == NULL) {
				crm_debug_3("not found");
				break;
				
				/* not running anywhere else */
			} else if(rsc->running_on == NULL) {
				crm_debug_3("not active yet");
				break;

				/* always unique */
			} else if(rsc->globally_unique) {
				crm_debug_3("unique");
				break;

				/* running somewhere already but we dont care
				 *   find another clone instead
				 */
			} else {
				crm_debug_2("find another one");
				rsc = NULL;
				is_duped_clone = TRUE;
				increment_clone(alt_rsc_id);
			}
		}
		crm_free(alt_rsc_id);
		if(is_duped_clone && rsc != NULL) {
			crm_info("Internally renamed %s on %s to %s",
				 rsc_id, node->details->uname, rsc->id);
			rsc->name = rsc_id;
		}
		
		crm_debug_3("[%s] Processing %s on %s (%s)",
			    crm_element_name(rsc_entry),
			    rsc_id, node_id, rsc_state);

		if(rsc == NULL) {
			crm_data_t *xml_rsc = create_xml_node(
				NULL, XML_CIB_TAG_RESOURCE);

			crm_log_xml_info(rsc_entry, "Orphan resource");

			pe_config_warn("Nothing known about resource"
				       " %s running on %s", rsc_id, node_id);
			
			copy_in_properties(xml_rsc, rsc_entry);
			
			common_unpack(xml_rsc, &rsc, NULL, data_set);
			rsc->orphan = TRUE;
			
			data_set->resources = g_list_append(
				data_set->resources, rsc);

			if(data_set->stop_rsc_orphans == FALSE
			   && is_duped_clone == FALSE) {
				rsc->is_managed = FALSE;

			} else {
				crm_info("Making sure orphan %s is stopped",
					 rsc_id);
			
				print_resource(LOG_DEBUG_3, "Added orphan",
					       rsc, FALSE);
				
				CRM_DEV_ASSERT(rsc != NULL);
				slist_iter(
					any_node, node_t, data_set->nodes, lpc,
					rsc2node_new(
						"__orphan_dont_run__", rsc,
						-INFINITY, any_node, data_set);
					);
			}
			
		} else {
			int attr_lpc = 0;
			gboolean force_restart = FALSE;
			for(; attr_lpc < DIMOF(attr_list); attr_lpc++) {
				value = crm_element_value(
					rsc->xml, attr_list[attr_lpc]);
				old_value = crm_element_value(
					rsc_entry, attr_list[attr_lpc]);
				if(safe_str_eq(value, old_value)) {
					continue;
				}
				
				force_restart = TRUE;
				crm_notice("Forcing restart of %s on %s,"
					   " %s changed: %s -> %s",
					   rsc->id, node->details->uname,
					   attr_list[attr_lpc],
					   crm_str(old_value), crm_str(value));
			}
			if(force_restart) {
				/* make sure the restart happens */
				stop_action(rsc, node, FALSE);
				rsc->start_pending = TRUE;
				delete_resource = TRUE;
			}
		}		
		
		max_call_id = -1;

		op_list = NULL;
		sorted_op_list = NULL;
		
		xml_child_iter(
			rsc_entry, rsc_op, XML_LRM_TAG_RSC_OP,
			op_list = g_list_append(op_list, rsc_op);
			);

		if(op_list == NULL) {
			continue;
		}
		
		sorted_op_list = g_list_sort(op_list, sort_op_by_callid);
		saved_role = rsc->role;
		rsc->role = RSC_ROLE_STOPPED;
		slist_iter(
			rsc_op, crm_data_t, sorted_op_list, lpc,

			unpack_rsc_op(rsc, node, rsc_op,
				      &max_call_id, data_set);
			);

		/* no need to free the contents */
		g_list_free(sorted_op_list);

		crm_info("Resource %s is %s on %s",
			 rsc->id, role2text(rsc->role), node->details->uname);

		rsc->known_on = g_list_append(rsc->known_on, node);
 		if(rsc->role != RSC_ROLE_STOPPED) { 
			crm_debug_2("Adding %s to %s", rsc->id, node->details->uname);
			native_add_running(rsc, node, data_set);

		} else {
			char *key = stop_key(rsc);
			GListPtr possible_matches = find_actions(rsc->actions, key, node);
			slist_iter(stop, action_t, possible_matches, lpc,
				   stop->optional = TRUE;
				);
			crm_free(key);
			
/* 			if(rsc->failed == FALSE && node->details->online) { */
/* 				delete_resource = TRUE; */
/* 			}			 */
		}

		if(delete_resource) {
			DeleteRsc(rsc, node, data_set);
		}
		
		if(saved_role > rsc->role) {
			rsc->role = saved_role;
		}
		);
	
	return TRUE;
}

#define sort_return(an_int) crm_free(a_uuid); crm_free(b_uuid); return an_int

gint
sort_op_by_callid(gconstpointer a, gconstpointer b)
{
	char *a_uuid = NULL;
	char *b_uuid = NULL;
 	const char *a_task_id = cl_get_string(a, XML_LRM_ATTR_CALLID);
 	const char *b_task_id = cl_get_string(b, XML_LRM_ATTR_CALLID);

	const char *a_key = cl_get_string(a, XML_ATTR_TRANSITION_MAGIC);
 	const char *b_key = cl_get_string(b, XML_ATTR_TRANSITION_MAGIC);

	const char *a_xml_id = ID(a);
	const char *b_xml_id = ID(b);
	
	int a_id = -1;
	int b_id = -1;

	int a_rc = -1;
	int b_rc = -1;

	int a_status = -1;
	int b_status = -1;
	
	int a_call_id = -1;
	int b_call_id = -1;

	if(safe_str_eq(a_xml_id, b_xml_id)) {
		/* We have duplicate lrm_rsc_op entries in the status
		 *    section which is unliklely to be a good thing
		 *    - we can handle it easily enough, but we need to get
		 *    to the bottom of why its happening.
		 */
		pe_err("Duplicate lrm_rsc_op entries named %s", a_xml_id);
		sort_return(0);
	}
	
	CRM_DEV_ASSERT(a_task_id != NULL && b_task_id != NULL);	
	a_call_id = crm_parse_int(a_task_id, NULL);
	b_call_id = crm_parse_int(b_task_id, NULL);
	
	if(a_call_id == -1 && b_call_id == -1) {
		/* both are pending ops so it doesnt matter since
		 *   stops are never pending
		 */
		sort_return(0);

	} else if(a_call_id >= 0 && a_call_id < b_call_id) {
		crm_debug_2("%s (%d) < %s (%d) : call id",
			    ID(a), a_call_id, ID(b), b_call_id);
		sort_return(-1);

	} else if(b_call_id >= 0 && a_call_id > b_call_id) {
		crm_debug_2("%s (%d) > %s (%d) : call id",
			    ID(a), a_call_id, ID(b), b_call_id);
		sort_return(1);
	}

	crm_debug_3("%s (%d) == %s (%d) : continuing",
		    ID(a), a_call_id, ID(b), b_call_id);
	
	/* now process pending ops */
	CRM_DEV_ASSERT(a_key != NULL && b_key != NULL);
	CRM_DEV_ASSERT(decode_transition_magic(
			       a_key,&a_uuid,&a_id,&a_status, &a_rc));
	CRM_DEV_ASSERT(decode_transition_magic(
			       b_key,&b_uuid,&b_id,&b_status, &b_rc));

	/* try and determin the relative age of the operation...
	 * some pending operations (ie. a start) may have been supuerceeded
	 *   by a subsequent stop
	 *
	 * [a|b]_id == -1 means its a shutdown operation and _always_ comes last
	 */
	if(safe_str_neq(a_uuid, b_uuid) || a_id == b_id) {
		/*
		 * some of the logic in here may be redundant...
		 *
		 * if the UUID from the TE doesnt match then one better
		 *   be a pending operation.
		 * pending operations dont survive between elections and joins
		 *   because we query the LRM directly
		 */
		
		CRM_DEV_ASSERT(a_call_id == -1 || b_call_id == -1);
		CRM_DEV_ASSERT(a_call_id >= 0  || b_call_id >= 0);

		if(b_call_id == -1) {
			crm_debug_2("%s (%d) < %s (%d) : transition + call id",
				    ID(a), a_call_id, ID(b), b_call_id);
			sort_return(-1);
		}

		if(a_call_id == -1) {
			crm_debug_2("%s (%d) > %s (%d) : transition + call id",
				    ID(a), a_call_id, ID(b), b_call_id);
			sort_return(1);
		}
		
	} else if((a_id >= 0 && a_id < b_id) || b_id == -1) {
		crm_debug_2("%s (%d) < %s (%d) : transition",
			    ID(a), a_id, ID(b), b_id);
		sort_return(-1);

	} else if((b_id >= 0 && a_id > b_id) || a_id == -1) {
		crm_debug_2("%s (%d) > %s (%d) : transition",
			    ID(a), a_id, ID(b), b_id);
		sort_return(1);
	}

	/* we should never end up here */
	crm_err("%s (%d:%d:%s) ?? %s (%d:%d:%s) : default",
		ID(a), a_call_id, a_id, a_uuid, ID(b), b_call_id, b_id, b_uuid);
	CRM_DEV_ASSERT(FALSE); 
	sort_return(0);
}

gboolean
unpack_rsc_op(resource_t *rsc, node_t *node, crm_data_t *xml_op,
	      int *max_call_id, pe_working_set_t *data_set) 
{
	const char *id          = NULL;
	const char *task        = NULL;
 	const char *task_id     = NULL;
	const char *actual_rc   = NULL;	
	const char *target_rc   = NULL;	
	const char *task_status = NULL;

	int interval = 0;
	int task_id_i = -1;
	int task_status_i = -2;
	int actual_rc_i = 0;
	
	action_t *action = NULL;
	gboolean is_stop_action = FALSE;

	crm_data_t *params = find_xml_node(xml_op, XML_TAG_PARAMS, FALSE);
	
	CRM_DEV_ASSERT(rsc    != NULL); if(crm_assert_failed) { return FALSE; }
	CRM_DEV_ASSERT(node   != NULL); if(crm_assert_failed) { return FALSE; }
	CRM_DEV_ASSERT(xml_op != NULL); if(crm_assert_failed) { return FALSE; }
	
	id = ID(xml_op);
	task        = crm_element_value(xml_op, XML_LRM_ATTR_TASK);
 	task_id     = crm_element_value(xml_op, XML_LRM_ATTR_CALLID);
	task_status = crm_element_value(xml_op, XML_LRM_ATTR_OPSTATUS);

	CRM_DEV_ASSERT(id != NULL);
        if(crm_assert_failed) { return FALSE; }	

	CRM_DEV_ASSERT(task != NULL);
        if(crm_assert_failed) { return FALSE; }

	CRM_DEV_ASSERT(task_status != NULL);
	if(crm_assert_failed) { return FALSE; }

	task_status_i = crm_parse_int(task_status, NULL);

	CRM_DEV_ASSERT(task_status_i <= LRM_OP_ERROR);
	if(crm_assert_failed) {return FALSE;}

	CRM_DEV_ASSERT(task_status_i >= LRM_OP_PENDING);
	if(crm_assert_failed) { return FALSE; }

	if(safe_str_eq(task, CRMD_ACTION_NOTIFY)) {
		/* safe to ignore these */
		return TRUE;
	}
	
	crm_debug_2("Unpacking task %s/%s (call_id=%s, status=%s) on %s (role=%s)",
		    rsc->id, task, task_id, task_status, node->details->uname,
		    role2text(rsc->role));

	if(params != NULL) {
		const char *interval_s = crm_element_value(params, "interval");
		if(interval_s != NULL) {
			interval = crm_parse_int(interval_s, NULL);
		}
	}
	
	if(params == NULL) {

	} else if(safe_str_eq(task, CRMD_ACTION_STATUS) && interval == 0) {
		/* ignore probes */
		
	} else if(rsc->orphan == FALSE || data_set->stop_rsc_orphans == FALSE){
		crm_data_t *op_match = NULL;
		crm_data_t *pdiff = NULL;
		GHashTable *local_rsc_params = NULL;
		crm_data_t *pnow = NULL;
		
		crm_debug_2("Checking parameters to %s action", task);
		
		xml_child_iter(
			rsc->ops_xml, operation, "op",
			
			const char *name = crm_element_value(operation, "name");
			int value = crm_get_msec(
				crm_element_value(operation, "interval"));
			if(interval <= 0) {
				break;
				
			} else if(safe_str_neq(name, task)) {
				continue;

			} else if(value != interval) {
				continue;
			}
			op_match = operation;
			);
		
		if(interval > 0 && op_match == NULL) {
			if(data_set->stop_action_orphans == FALSE) {
				/* create a cancel action */
				pe_config_warn("Ignoring orphan action: %s", id);
				return TRUE;
			}

			pe_config_warn("Orphan action will be stopped: %s", id);

			action = custom_action(
				rsc, crm_strdup(id), CRMD_ACTION_CANCEL, node,
				FALSE, TRUE, data_set);
			
			custom_action_order(
				rsc, NULL, action,
				rsc, stop_key(rsc), NULL,
				pe_ordering_optional, data_set);

			return TRUE;
		}

		action = custom_action(rsc, crm_strdup(id), task, node,
				       TRUE, FALSE, data_set);

		local_rsc_params = g_hash_table_new_full(
			g_str_hash, g_str_equal,
			g_hash_destroy_str, g_hash_destroy_str);

		unpack_instance_attributes(
			rsc->xml, XML_TAG_ATTR_SETS, node, local_rsc_params,
			NULL, 0, data_set);
		
		pnow = create_xml_node(NULL, XML_TAG_PARAMS);
		g_hash_table_foreach(action->extra, hash2field, pnow);
		g_hash_table_foreach(rsc->parameters, hash2field, pnow);
		g_hash_table_foreach(local_rsc_params, hash2field, pnow);
 		xml_remove_prop(pnow, XML_ATTR_CRM_VERSION); 
 		xml_remove_prop(params, XML_ATTR_CRM_VERSION); 
 		xml_remove_prop(pnow, "is_managed"); 
 		xml_remove_prop(params, "is_managed"); 
		
 		pdiff = diff_xml_object(params, pnow, TRUE);
		if(pdiff != NULL) {
			crm_info("Parameters to %s action changed", id);
			log_xml_diff(LOG_INFO, pdiff, __FUNCTION__);

			custom_action(rsc, crm_strdup(id), task, NULL,
				      FALSE, TRUE, data_set);
		}
		
		g_hash_table_destroy(action->extra);
		crm_free(action->uuid);
		crm_free(action);
		free_xml(pnow);
		free_xml(pdiff);
		g_hash_table_destroy(local_rsc_params);
		action = NULL;
	}
	
	if(safe_str_eq(task, CRMD_ACTION_STOP)) {
		is_stop_action = TRUE;
	}
	
	if(task_status_i != LRM_OP_PENDING) {

		task_id_i = crm_parse_int(task_id, "-1");

		CRM_DEV_ASSERT(task_id != NULL);
		if(crm_assert_failed) { return FALSE; }

		CRM_DEV_ASSERT(task_id_i >= 0);
		if(crm_assert_failed) { return FALSE; }

		if(task_id_i == *max_call_id) {
			crm_debug_2("Already processed this call");
			return TRUE;
		}

		CRM_DEV_ASSERT(task_id_i > *max_call_id);
		if(crm_assert_failed) { return FALSE; }
	}

	if(*max_call_id < task_id_i) {
		*max_call_id = task_id_i;
	}
	
	if(node->details->unclean) {
		crm_debug_2("Node %s (where %s is running) is unclean."
			  " Further action depends on the value of %s",
			  node->details->uname, rsc->id, XML_RSC_ATTR_STOPFAIL);
	}

	if(params != NULL) {
		target_rc = crm_element_value(params, XML_ATTR_TE_TARGET_RC);
	}

	actual_rc = crm_element_value(xml_op, XML_LRM_ATTR_RC);
	CRM_DEV_ASSERT(actual_rc != NULL);
	if(actual_rc != NULL) {
		actual_rc_i = crm_parse_int(actual_rc, NULL);
	}
	
	if(target_rc != NULL && task_status_i != LRM_OP_PENDING) {
		crm_debug_2("Exit code from %s: %s vs. %s",
			    task, target_rc, actual_rc);
		if(safe_str_eq(target_rc, actual_rc)) {
			task_status_i = LRM_OP_DONE;
		} else {
			task_status_i = LRM_OP_ERROR;
		}
	}
	
	if(EXECRA_NOT_RUNNING == actual_rc_i) {
		rsc->role = RSC_ROLE_STOPPED;
		if(safe_str_eq(task, CRMD_ACTION_STATUS)) {
			/* probe or stop action*/
			crm_info("%s: resource %s is stopped", id, rsc->id);
			return TRUE;
		}

	} else if(EXECRA_RUNNING_MASTER == actual_rc_i) {
		rsc->role = RSC_ROLE_MASTER;
		if(safe_str_eq(task, CRMD_ACTION_STATUS)) {
			crm_info("%s: resource %s is a master", id, rsc->id);
			return TRUE;
		}

	} else if(EXECRA_FAILED_MASTER == actual_rc_i) {
		rsc->role = RSC_ROLE_MASTER;
		task_status_i = LRM_OP_ERROR;
	}

	if(task_status_i == LRM_OP_ERROR
	   || task_status_i == LRM_OP_TIMEOUT
	   || task_status_i == LRM_OP_NOTSUPPORTED) {
		action = custom_action(rsc, crm_strdup(id), task, NULL,
				       TRUE, TRUE, data_set);
		if(action->on_fail == action_fail_ignore) {
			task_status_i = LRM_OP_DONE;
		}
	}
	
	switch(task_status_i) {
		case LRM_OP_PENDING:
			/*
			 * TODO: this may need some more thought
			 * Some cases:
			 * - PE reinvoked with pending action that will succeed
			 * - PE reinvoked with pending action that will fail
			 * - After DC election
			 * - After startup
			 *
			 * pending start - required start
			 * pending stop  - required stop
			 * pending <any> on unavailable node - stonith
			 *
			 * For now this should do
			 */

			if(safe_str_eq(task, CRMD_ACTION_START)) {
				rsc->start_pending = TRUE;
				rsc->role = RSC_ROLE_STARTED;
		
				/* make sure it is re-issued but,
				 * only if we have quorum
				 */
				if(data_set->have_quorum == TRUE
				   || data_set->no_quorum_policy == no_quorum_ignore){
					/* do not specify the node, we may want
					 * to start it elsewhere
					 */
					start_action(rsc, NULL, FALSE);
				}
				
			} else if(safe_str_eq(task, CRMD_ACTION_PROMOTE)) {
				rsc->role = RSC_ROLE_MASTER;

			} else if(rsc->role > RSC_ROLE_STOPPED) {
				crm_debug_2("Re-issuing pending recurring task:"
					    " %s for %s on %s",
					    task, rsc->id, node->details->id);
				/* do not specify the node, we may want
				 * to start it elsewhere
				 */
				custom_action(rsc, crm_strdup(id), task,
					      NULL, FALSE, TRUE, data_set);
			}
			break;
		
		case LRM_OP_DONE:
			crm_debug_3("%s/%s completed on %s",
				    rsc->id, task, node->details->uname);

			if(is_stop_action) {
				rsc->role = RSC_ROLE_STOPPED;

			} else if(safe_str_eq(task, CRMD_ACTION_PROMOTE)) {
				rsc->role = RSC_ROLE_MASTER;

			} else if(safe_str_eq(task, CRMD_ACTION_DEMOTE)) {
				rsc->role = RSC_ROLE_SLAVE;
				
			} else {
				/* make sure its already created and is optional
				 *
				 * creating it now tells create_recurring_actions() 
				 *  that it can safely leave it optional
				 */
				if(rsc->role < RSC_ROLE_STARTED) {
					crm_info("%s active on %s",
						 rsc->id, node->details->uname);
					rsc->role = RSC_ROLE_STARTED;
				}

				/* the != start check is so i dont have to
				 * update all the old testcases
				 */
				if(interval > 0
				   || safe_str_neq(task, CRMD_ACTION_START)) {
					crm_debug_2("%s: %s active on %s",
						    rsc->id, id, node->details->uname);
					custom_action(rsc, crm_strdup(id), task,
						      node, TRUE, TRUE, data_set);
				}
			}
			
			break;

		case LRM_OP_ERROR:
		case LRM_OP_TIMEOUT:
		case LRM_OP_NOTSUPPORTED:
			crm_warn("Processing failed op (%s) for %s on %s",
				 id, rsc->id, node->details->uname);

			action = custom_action(
				rsc, crm_strdup(id), task, NULL,
				TRUE, TRUE, data_set);

			if(task_status_i == LRM_OP_NOTSUPPORTED
			   || is_stop_action
			   || safe_str_eq(task, CRMD_ACTION_START) ) {
				crm_warn("Handling failed %s for %s on %s",
					 task, rsc->id, node->details->uname);
				rsc2node_new("dont_run__failed_stopstart",
					     rsc, -INFINITY, node, data_set);
			}
			
			rsc->failed = TRUE;

			if(safe_str_eq(task, CRMD_ACTION_PROMOTE)) {
				rsc->role = RSC_ROLE_MASTER;

			} else if(safe_str_eq(task, CRMD_ACTION_DEMOTE)) {
				rsc->role = RSC_ROLE_MASTER;
				
			} else if(rsc->role < RSC_ROLE_STARTED) {
				rsc->role = RSC_ROLE_STARTED;
			}

			crm_debug_2("Resource %s: role=%s, unclean=%s, on_fail=%s, fail_role=%s",
				    rsc->id, role2text(rsc->role),
				    node->details->unclean?"true":"false",
				    fail2text(action->on_fail),
				    role2text(action->fail_role));

			crm_debug_2("Force stop");
			if(node->details->unclean) {
				stop_action(rsc, node, FALSE);

			} else if(action->on_fail == action_fail_fence) {
				/* treat it as if it is still running
				 * but also mark the node as unclean
				 */
				node->details->unclean = TRUE;
				stop_action(rsc, node, FALSE);
				
			} else if(action->on_fail == action_fail_block) {
				/* is_managed == FALSE will prevent any
				 * actions being sent for the resource
				 */
				rsc->is_managed = FALSE;
				
			} else if(action->on_fail == action_fail_migrate) {
 				stop_action(rsc, node, FALSE);

				/* make sure it comes up somewhere else
				 * or not at all
				 */
				rsc2node_new("__action_migration_auto__",
					     rsc, -INFINITY, node, data_set);
				
			} else if(action->fail_role == RSC_ROLE_STOPPED) {
				/* make sure it doesnt come up again */
				native_assign_color(rsc, data_set->no_color);

			} else if(action->fail_role != RSC_ROLE_STARTED) {
				rsc->next_role = action->fail_role;

			} else {
 				stop_action(rsc, node, FALSE);
			}
			
			break;
		case LRM_OP_CANCELLED:
			/* do nothing?? */
			pe_err("Dont know what to do for cancelled ops yet");
			break;
	}

	crm_debug_2("Resource %s after %s: role=%s",
		    rsc->id, task, role2text(rsc->role));
	
	return TRUE;
}

gboolean
rsc_colocation_new(const char *id, enum con_strength strength,
		   resource_t *rsc_lh, resource_t *rsc_rh,
		   const char *state_lh, const char *state_rh)
{
	rsc_colocation_t *new_con      = NULL;
 	rsc_colocation_t *inverted_con = NULL; 

	if(rsc_lh == NULL){
		pe_config_err("No resource found for LHS %s", id);
		return FALSE;

	} else if(rsc_rh == NULL){
		pe_config_err("No resource found for RHS of %s", id);
		return FALSE;
	}

	crm_malloc0(new_con, sizeof(rsc_colocation_t));
	if(new_con == NULL) {
		return FALSE;
	}
	if(safe_str_eq(state_lh, CRMD_ACTION_STARTED)) {
		state_lh = NULL;
	}
	if(safe_str_eq(state_rh, CRMD_ACTION_STARTED)) {
		state_rh = NULL;
	}

	new_con->id       = id;
	new_con->rsc_lh   = rsc_lh;
	new_con->rsc_rh   = rsc_rh;
	new_con->strength = strength;
	new_con->state_lh = state_lh;
	new_con->state_rh = state_rh;

	inverted_con = invert_constraint(new_con);
	
	crm_debug_4("Adding constraint %s (%p) to %s",
		  new_con->id, new_con, rsc_lh->id);
	
	rsc_lh->rsc_cons = g_list_insert_sorted(
		rsc_lh->rsc_cons, new_con, sort_cons_strength);
	
	crm_debug_4("Adding constraint %s (%p) to %s",
		  inverted_con->id, inverted_con, rsc_rh->id);
	
	rsc_rh->rsc_cons = g_list_insert_sorted(
		rsc_rh->rsc_cons, inverted_con, sort_cons_strength);
	
	return TRUE;
}

/* LHS before RHS */
gboolean
custom_action_order(
	resource_t *lh_rsc, char *lh_action_task, action_t *lh_action,
	resource_t *rh_rsc, char *rh_action_task, action_t *rh_action,
	enum pe_ordering type, pe_working_set_t *data_set)
{
	order_constraint_t *order = NULL;

	if((lh_action == NULL && lh_rsc == NULL)
	   || (rh_action == NULL && rh_rsc == NULL)){
		pe_config_err("Invalid inputs lh_rsc=%p, lh_a=%p,"
			      " rh_rsc=%p, rh_a=%p",
			      lh_rsc, lh_action, rh_rsc, rh_action);
		crm_free(lh_action_task);
		crm_free(rh_action_task);
		return FALSE;
	}

	crm_malloc0(order, sizeof(order_constraint_t));
	if(order == NULL) { return FALSE; }
	
	order->id             = data_set->order_id++;
	order->type           = type;
	order->lh_rsc         = lh_rsc;
	order->rh_rsc         = rh_rsc;
	order->lh_action      = lh_action;
	order->rh_action      = rh_action;
	order->lh_action_task = lh_action_task;
	order->rh_action_task = rh_action_task;
	
	data_set->ordering_constraints = g_list_append(
		data_set->ordering_constraints, order);
	
	if(lh_rsc != NULL && rh_rsc != NULL) {
		crm_debug_4("Created ordering constraint %d (%s):"
			 " %s/%s before %s/%s",
			 order->id, ordering_type2text(order->type),
			 lh_rsc->id, lh_action_task,
			 rh_rsc->id, rh_action_task);
		
	} else if(lh_rsc != NULL) {
		crm_debug_4("Created ordering constraint %d (%s):"
			 " %s/%s before action %d (%s)",
			 order->id, ordering_type2text(order->type),
			 lh_rsc->id, lh_action_task,
			 rh_action->id, rh_action_task);
		
	} else if(rh_rsc != NULL) {
		crm_debug_4("Created ordering constraint %d (%s):"
			 " action %d (%s) before %s/%s",
			 order->id, ordering_type2text(order->type),
			 lh_action->id, lh_action_task,
			 rh_rsc->id, rh_action_task);
		
	} else {
		crm_debug_4("Created ordering constraint %d (%s):"
			 " action %d (%s) before action %d (%s)",
			 order->id, ordering_type2text(order->type),
			 lh_action->id, lh_action_task,
			 rh_action->id, rh_action_task);
	}
	
	return TRUE;
}

gboolean
unpack_rsc_colocation(crm_data_t * xml_obj, pe_working_set_t *data_set)
{
	enum con_strength strength_e = pecs_ignore;

	const char *id    = crm_element_value(xml_obj, XML_ATTR_ID);
	const char *id_rh = crm_element_value(xml_obj, XML_CONS_ATTR_TO);
	const char *id_lh = crm_element_value(xml_obj, XML_CONS_ATTR_FROM);
	const char *score = crm_element_value(xml_obj, XML_RULE_ATTR_SCORE);
	const char *state_lh = crm_element_value(xml_obj, XML_RULE_ATTR_FROMSTATE);
	const char *state_rh = crm_element_value(xml_obj, XML_RULE_ATTR_TOSTATE);

	resource_t *rsc_lh = pe_find_resource(data_set->resources, id_lh);
	resource_t *rsc_rh = pe_find_resource(data_set->resources, id_rh);
 
	if(rsc_lh == NULL) {
		pe_config_err("No resource (con=%s, rsc=%s)", id, id_lh);
		return FALSE;
		
	} else if(rsc_rh == NULL) {
		pe_config_err("No resource (con=%s, rsc=%s)", id, id_rh);
		return FALSE;
	}

	/* the docs indicate that only +/- INFINITY are allowed,
	 *   but no-one ever reads the docs so all positive values will
	 *   count as "must" and negative values as "must not"
	 */
	if(score == NULL || score[0] != '-') {
		strength_e = pecs_must;
	} else {
		strength_e = pecs_must_not;
	}
	return rsc_colocation_new(id, strength_e, rsc_lh, rsc_rh,
				  state_lh, state_rh);
}

static const char *
invert_action(const char *action) 
{
	if(safe_str_eq(action, CRMD_ACTION_START)) {
		return CRMD_ACTION_STOP;

	} else if(safe_str_eq(action, CRMD_ACTION_STOP)) {
		return CRMD_ACTION_START;
		
	} else if(safe_str_eq(action, CRMD_ACTION_PROMOTE)) {
		return CRMD_ACTION_DEMOTE;
		
	} else if(safe_str_eq(action, CRMD_ACTION_DEMOTE)) {
		return CRMD_ACTION_PROMOTE;

	} else if(safe_str_eq(action, CRMD_ACTION_STARTED)) {
		return CRMD_ACTION_STOPPED;
		
	} else if(safe_str_eq(action, CRMD_ACTION_STOPPED)) {
		return CRMD_ACTION_STARTED;
		
	}
	pe_err("Unknown action: %s", action);
	return NULL;
}


gboolean
unpack_rsc_order(crm_data_t * xml_obj, pe_working_set_t *data_set)
{
	gboolean symmetrical_bool = TRUE;
	
	const char *id     = crm_element_value(xml_obj, XML_ATTR_ID);
	const char *type   = crm_element_value(xml_obj, XML_ATTR_TYPE);
	const char *id_rh  = crm_element_value(xml_obj, XML_CONS_ATTR_TO);
	const char *id_lh  = crm_element_value(xml_obj, XML_CONS_ATTR_FROM);
	const char *action = crm_element_value(xml_obj, XML_CONS_ATTR_ACTION);
	const char *action_rh = crm_element_value(xml_obj, XML_CONS_ATTR_TOACTION);

	const char *symmetrical = crm_element_value(
		xml_obj, XML_CONS_ATTR_SYMMETRICAL);

	resource_t *rsc_lh   = NULL;
	resource_t *rsc_rh   = NULL;

	if(xml_obj == NULL) {
		pe_config_err("No constraint object to process.");
		return FALSE;

	} else if(id == NULL) {
		pe_config_err("%s constraint must have an id",
			crm_element_name(xml_obj));
		return FALSE;
		
	} else if(id_lh == NULL || id_rh == NULL) {
		pe_config_err("Constraint %s needs two sides lh: %s rh: %s",
			      id, crm_str(id_lh), crm_str(id_rh));
		return FALSE;
	}

	if(action == NULL) {
		action = CRMD_ACTION_START;
	}
	if(action_rh == NULL) {
		action_rh = action;
	}
	CRM_DEV_ASSERT(action != NULL);
	CRM_DEV_ASSERT(action_rh != NULL);
	
	if(safe_str_eq(type, "before")) {
		id_lh  = crm_element_value(xml_obj, XML_CONS_ATTR_TO);
		id_rh  = crm_element_value(xml_obj, XML_CONS_ATTR_FROM);
		action = crm_element_value(xml_obj, XML_CONS_ATTR_TOACTION);
		action_rh = crm_element_value(xml_obj, XML_CONS_ATTR_ACTION);
		if(action_rh == NULL) {
			action_rh = CRMD_ACTION_START;
		}
		if(action == NULL) {
			action = action_rh;
		}
	}

	CRM_DEV_ASSERT(action != NULL);
	CRM_DEV_ASSERT(action_rh != NULL);
	
	rsc_lh   = pe_find_resource(data_set->resources, id_rh);
	rsc_rh   = pe_find_resource(data_set->resources, id_lh);

	if(rsc_lh == NULL) {
		pe_config_err("Constraint %s: no resource found for LHS of %s", id, id_lh);
		return FALSE;
	
	} else if(rsc_rh == NULL) {
		pe_config_err("Constraint %s: no resource found for RHS of %s", id, id_rh);
		return FALSE;
	}

	custom_action_order(
		rsc_lh, generate_op_key(rsc_lh->id, action, 0), NULL,
		rsc_rh, generate_op_key(rsc_rh->id, action_rh, 0), NULL,
		pe_ordering_optional, data_set);

	if(rsc_rh->restart_type == pe_restart_restart
	   && safe_str_eq(action, action_rh)) {
		if(safe_str_eq(action, CRMD_ACTION_START)) {
			crm_debug_2("Recover start-start: %s-%s",
				rsc_lh->id, rsc_rh->id);
  			order_start_start(rsc_lh, rsc_rh, pe_ordering_recover);
 		} else if(safe_str_eq(action, CRMD_ACTION_STOP)) {
			crm_debug_2("Recover stop-stop: %s-%s",
				rsc_rh->id, rsc_lh->id);
  			order_stop_stop(rsc_rh, rsc_lh, pe_ordering_recover); 
		}
	}

	cl_str_to_boolean(symmetrical, &symmetrical_bool);
	if(symmetrical_bool == FALSE) {
		return TRUE;
	}
	
	action = invert_action(action);
	action_rh = invert_action(action_rh);
	
	custom_action_order(
		rsc_rh, generate_op_key(rsc_rh->id, action_rh, 0), NULL,
		rsc_lh, generate_op_key(rsc_lh->id, action, 0), NULL,
		pe_ordering_optional, data_set);

	if(rsc_lh->restart_type == pe_restart_restart
	   && safe_str_eq(action, action_rh)) {
		if(safe_str_eq(action, CRMD_ACTION_START)) {
			crm_debug_2("Recover start-start (2): %s-%s",
				rsc_lh->id, rsc_rh->id);
  			order_start_start(rsc_lh, rsc_rh, pe_ordering_recover);
		} else if(safe_str_eq(action, CRMD_ACTION_STOP)) { 
			crm_debug_2("Recover stop-stop (2): %s-%s",
				rsc_rh->id, rsc_lh->id);
  			order_stop_stop(rsc_rh, rsc_lh, pe_ordering_recover); 
		}
	}
	
	return TRUE;
}

gboolean
add_node_attrs(crm_data_t *xml_obj, node_t *node, pe_working_set_t *data_set)
{
 	g_hash_table_insert(node->details->attrs,
			    crm_strdup("#"XML_ATTR_UNAME),
			    crm_strdup(node->details->uname));
 	g_hash_table_insert(node->details->attrs,
			    crm_strdup("#"XML_ATTR_ID),
			    crm_strdup(node->details->id));
	if(safe_str_eq(node->details->id, data_set->dc_uuid)) {
		data_set->dc_node = node;
		node->details->is_dc = TRUE;
		g_hash_table_insert(node->details->attrs,
				    crm_strdup("#"XML_ATTR_DC),
				    crm_strdup(XML_BOOLEAN_TRUE));
	} else {
		g_hash_table_insert(node->details->attrs,
				    crm_strdup("#"XML_ATTR_DC),
				    crm_strdup(XML_BOOLEAN_FALSE));
	}
	
	unpack_instance_attributes(
		xml_obj, XML_TAG_ATTR_SETS, node, node->details->attrs,
		NULL, 0, data_set);

	return TRUE;
}

gboolean
unpack_rsc_location(crm_data_t * xml_obj, pe_working_set_t *data_set)
{
	const char *id_lh   = crm_element_value(xml_obj, "rsc");
	const char *id      = crm_element_value(xml_obj, XML_ATTR_ID);
	resource_t *rsc_lh  = pe_find_resource(data_set->resources, id_lh);
	
	if(rsc_lh == NULL) {
		/* only a warn as BSC adds the constraint then the resource */
		pe_config_warn("No resource (con=%s, rsc=%s)", id, id_lh);
		return FALSE;

	} else if(rsc_lh->is_managed == FALSE) {
		crm_debug_2("Ignoring constraint %s: resource %s not managed",
			    id, id_lh);
		return FALSE;
	}

	xml_child_iter(
		xml_obj, rule_xml, XML_TAG_RULE,
		crm_debug_2("Unpacking %s/%s", id, ID(rule_xml));
		generate_location_rule(rsc_lh, rule_xml, data_set);
		);
	return TRUE;
}

rsc_to_node_t *
generate_location_rule(
	resource_t *rsc, crm_data_t *rule_xml, pe_working_set_t *data_set)
{	
	const char *rule_id = NULL;
	const char *score   = NULL;
	const char *boolean = NULL;
	const char *role    = NULL;
	const char *attr_score = NULL;

	GListPtr match_L  = NULL;
	
	int score_f   = 0;
	gboolean do_and = TRUE;
	gboolean accept = TRUE;
	gboolean raw_score = TRUE;
	
	rsc_to_node_t *location_rule = NULL;
	
	rule_id = crm_element_value(rule_xml, XML_ATTR_ID);
	boolean = crm_element_value(rule_xml, XML_RULE_ATTR_BOOLEAN_OP);
	role = crm_element_value(rule_xml, XML_RULE_ATTR_ROLE);

	crm_debug_2("processing rule: %s", rule_id);

	if(role != NULL && text2role(role) == RSC_ROLE_UNKNOWN) {
		pe_err("Bad role specified for %s: %s", rule_id, role);
		return NULL;
	}
	
	score = crm_element_value(rule_xml, XML_RULE_ATTR_SCORE);
	if(score != NULL) {
		score_f = char2score(score);

	} else {
		score = crm_element_value(
			rule_xml, XML_RULE_ATTR_SCORE_ATTRIBUTE);
		if(score == NULL) {
			score = crm_element_value(
				rule_xml, XML_RULE_ATTR_SCORE_MANGLED);
		}
		if(score != NULL) {
			raw_score = FALSE;
		}
	}
	
	if(safe_str_eq(boolean, "or")) {
		do_and = FALSE;
	}
	
	location_rule = rsc2node_new(rule_id, rsc, 0, NULL, data_set);
	
	if(location_rule == NULL) {
		return NULL;
	}
	if(role != NULL) {
		crm_debug_2("Setting role filter: %s", role);
		location_rule->role_filter = text2role(role);
	}
	if(do_and) {
		match_L = node_list_dup(data_set->nodes, FALSE);
		slist_iter(
			node, node_t, match_L, lpc,
			node->weight = score_f;
			);
	}

	xml_child_iter(
		rule_xml, expr, NULL,		

		enum expression_type type = find_expression_type(expr);
		if(type == not_expr) {
			pe_err("Expression <%s id=%s...> is not valid",
			       crm_element_name(expr), crm_str(ID(expr)));
			continue;
			
		}	
		
		slist_iter(
			node, node_t, data_set->nodes, lpc,

			if(type == nested_rule) {
				accept = test_rule(expr, node, rsc, data_set);
			} else {
				accept = test_expression(
					expr, node, rsc, data_set);
			}
			
			if(raw_score == FALSE) {
				attr_score = g_hash_table_lookup(
					node->details->attrs, score);
				if(attr_score == NULL) {
					accept = FALSE;
					pe_warn("node %s did not have a value"
						" for %s",
						node->details->uname, score);
				} else {
					score_f = char2score(score);
				}
			}
			
			if(!do_and && accept) {
				node_t *local = pe_find_node_id(
					match_L, node->details->id);
				if(local == NULL) {
					local = node_copy(node);
					match_L = g_list_append(match_L, local);
				}
				local->weight = merge_weights(
					local->weight, score_f);
				crm_debug_5("node %s already matched",
					    node->details->uname);
				
			} else if(do_and && !accept) {
				/* remove it */
				node_t *delete = pe_find_node_id(
					match_L, node->details->id);
				if(delete != NULL) {
					match_L = g_list_remove(match_L,delete);
					crm_debug_5("node %s did not match",
						    node->details->uname);
				}
				crm_free(delete);
			}
			);
		);
	
	location_rule->node_list_rh = match_L;
	if(location_rule->node_list_rh == NULL) {
		crm_debug_2("No matching nodes for rule %s", rule_id);
		return NULL;
	} 

	crm_debug_2("%s: %d nodes matched",
		    rule_id, g_list_length(location_rule->node_list_rh));
	crm_action_debug_3(print_rsc_to_node("Added", location_rule, FALSE));
	return location_rule;
}
