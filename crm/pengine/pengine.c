/* $Id: pengine.c,v 1.62 2005/04/16 16:57:57 andrew Exp $ */
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
#include <sys/param.h>

#include <crm/crm.h>
#include <crm/cib.h>
#include <crm/msg_xml.h>
#include <crm/common/xml.h>
#include <crm/common/msg.h>

#include <glib.h>

#include <pengine.h>
#include <pe_utils.h>

crm_data_t * do_calculations(crm_data_t *cib_object);
int num_synapse = 0;

gboolean
process_pe_message(HA_Message *msg, crm_data_t * xml_data, IPC_Channel *sender)
{
	const char *sys_to = cl_get_string(msg, F_CRM_SYS_TO);
	const char *op = cl_get_string(msg, F_CRM_TASK);
	const char *ref = cl_get_string(msg, XML_ATTR_REFERENCE);

	crm_verbose("Processing %s op (ref=%s)...", op, ref);
	
	if(op == NULL){
		/* error */

	} else if(strcmp(op, CRM_OP_HELLO) == 0) {
		/* ignore */
		
	} else if(safe_str_eq(cl_get_string(msg, F_CRM_MSG_TYPE),
			      XML_ATTR_RESPONSE)) {
		/* ignore */
		
	} else if(sys_to == NULL || strcmp(sys_to, CRM_SYSTEM_PENGINE) != 0) {
		crm_verbose("Bad sys-to %s", crm_str(sys_to));
		return FALSE;
		
	} else if(strcmp(op, CRM_OP_PECALC) == 0) {
		crm_data_t * output = NULL;
		crm_data_t * status = get_object_root(XML_CIB_TAG_STATUS, xml_data);
		
		crm_xml_info(status, "[in ]");
		crm_xml_devel(xml_data, "[all]");
		output = do_calculations(xml_data);
		crm_xml_devel(output, "[out]");

		if (send_ipc_reply(sender, msg, output) ==FALSE) {

			crm_warn("Answer could not be sent");
		}
		free_xml(output);

	} else if(strcmp(op, CRM_OP_QUIT) == 0) {
		crm_warn("Received quit message, terminating");
		exit(0);
	}
	
	return TRUE;
}

crm_data_t *
do_calculations(crm_data_t * cib_object)
{
	GListPtr resources = NULL;
	GListPtr nodes = NULL;
	GListPtr placement_constraints = NULL;
	GListPtr actions = NULL;
	GListPtr ordering_constraints = NULL;
	GListPtr stonith_list = NULL;
	GListPtr shutdown_list = NULL;

	GListPtr colors = NULL;
	GListPtr action_sets = NULL;

	crm_data_t * graph = NULL;

/*	pe_debug_on(); */
	
	crm_trace("unpack");		  
	stage0(cib_object,
	       &resources,
	       &nodes,  &placement_constraints,
	       &actions,  &ordering_constraints,
	       &stonith_list, &shutdown_list);

	crm_trace("apply placement constraints");
	stage1(placement_constraints, nodes, resources);
	
	crm_trace("color resources");
	stage2(resources, nodes, &colors);

	/* unused */
	stage3(colors);
	
	crm_trace("assign nodes to colors");
	stage4(colors);
	
	crm_trace("creating actions and internal ording constraints");
	stage5(resources, &ordering_constraints);
		
	crm_trace("processing fencing and shutdown cases");
	stage6(&actions, &ordering_constraints, nodes, resources);
	
	crm_trace("applying ordering constraints");
	stage7(resources, actions, ordering_constraints);
	
	crm_verbose("\t========= Set %d (Un-runnable) =========", -1);
	crm_verbose_action(
		slist_iter(action, action_t, actions, lpc,
			   if(action->optional == FALSE
			      && action->runnable == FALSE) {
				   log_action(LOG_VERBOSE, "\t", action, TRUE);
			   }
			)
		);
	
	crm_verbose("========= Stonith List =========");
	crm_devel_action(
		slist_iter(node, node_t, stonith_list, lpc,
			   print_node(NULL, node, FALSE);
			)
		);
	
	crm_verbose("========= Shutdown List =========");
	crm_devel_action(
		slist_iter(node, node_t, shutdown_list, lpc,
			   print_node(NULL, node, FALSE);
			)
		);
	
	crm_trace("=#=#=#=#= Stage 8 =#=#=#=#=");
	stage8(resources, actions, &graph);
	
	crm_verbose("Cleaning up");

	while(placement_constraints) {
		pe_free_rsc_to_node((rsc_to_node_t*)placement_constraints->data);
		placement_constraints = placement_constraints->next;
	}
	if(placement_constraints != NULL) {
		g_list_free(placement_constraints);
	}

	pe_free_shallow(ordering_constraints);

	slist_iter(action_set, GList, action_sets, lpc,
		   pe_free_shallow_adv(action_set, FALSE);
		);
	pe_free_shallow_adv(action_sets, FALSE);

/* 	pe_free_actions(actions); */
	pe_free_resources(resources); 
	pe_free_colors(colors);
	pe_free_nodes(nodes);

	if(shutdown_list != NULL) {
		g_list_free(shutdown_list);
	}
	if(stonith_list != NULL) {
		g_list_free(stonith_list);
	}
	
	return graph;
}
