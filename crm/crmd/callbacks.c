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
#include <string.h>
#include <crmd_fsa.h>

#include <heartbeat.h>

#include <hb_api.h>
#include <crm/msg_xml.h>
#include <crm/common/xml.h>
#include <crm/common/msg.h>
#include <crm/cib.h>

#include <crmd.h>
#include <crmd_messages.h>
#include <crmd_callbacks.h>

#include <crm/dmalloc_wrapper.h>

crm_data_t *find_xml_in_hamessage(const HA_Message * msg);
void crmd_ha_connection_destroy(gpointer user_data);

void
crmd_ha_msg_callback(const HA_Message * msg, void* private_data)
{
	ha_msg_input_t *new_input = NULL;
	oc_node_t *from_node = NULL;
	
	const char *from = ha_msg_value(msg, F_ORIG);
	const char *seq  = ha_msg_value(msg, F_SEQ);
	const char *op   = ha_msg_value(msg, F_CRM_TASK);

	const char *sys_to   = ha_msg_value(msg, F_CRM_SYS_TO);
	const char *sys_from = ha_msg_value(msg, F_CRM_SYS_FROM);

	CRM_DEV_ASSERT(from != NULL);

	if(fsa_membership_copy == NULL) {
		crm_debug("Ignoring HA messages until we are"
			  " connected to the CCM");
		crm_log_message_adv(
			LOG_DEBUG, "HA[inbound]: Ignore (No CCM)", msg);
		return;
	}
	
	from_node = g_hash_table_lookup(fsa_membership_copy->members, from);

	if(from_node == NULL) {
		crm_debug("Ignoring HA messages from %s: not in our"
			  " membership list", from);
		crm_log_message_adv(LOG_DEBUG, "HA[inbound]: CCM Discard", msg);
		
	} else if(AM_I_DC
	   && safe_str_eq(sys_from, CRM_SYSTEM_DC)
	   && safe_str_neq(from, fsa_our_uname)) {
		crm_err("Another DC detected: %s (op=%s)", from, op);
		crm_log_message_adv(LOG_WARNING, "HA[inbound]: Duplicate DC", msg);
		new_input = new_ha_msg_input(msg);
		register_fsa_input(C_HA_MESSAGE, I_ELECTION, new_input);

#if 0
		/* still thinking about this one...
		 * could create a timing issue if we dont notice the
		 * election before a new DC is elected.
		 */
	} else if(fsa_our_dc != NULL
		  && safe_str_eq(sys_from, CRM_SYSTEM_DC)
		  && safe_str_neq(from, fsa_our_dc)) {
		crm_warn("Ignoring message from wrong DC: %s vs. %s ",
			 from, fsa_our_dc);
		crm_log_message_adv(LOG_WARNING, "HA[inbound]: wrong DC", msg);
#endif
	} else if(safe_str_eq(sys_to, CRM_SYSTEM_DC) && AM_I_DC == FALSE) {
		crm_verbose("Ignoring message for the DC [F_SEQ=%s]", seq);
		crm_log_message_adv(LOG_TRACE, "HA[inbound]: ignore", msg);
		return;

	} else if(safe_str_eq(from, fsa_our_uname)
		  && safe_str_eq(op, CRM_OP_VOTE)) {
		crm_log_message_adv(LOG_TRACE, "HA[inbound]", msg);
		crm_verbose("Ignoring our own vote [F_SEQ=%s]: own vote", seq);
		return;
		
	} else if(AM_I_DC && safe_str_eq(op, CRM_OP_HBEAT)) {
		crm_verbose("Ignoring our own heartbeat [F_SEQ=%s]", seq);
		crm_log_message_adv(LOG_TRACE, "HA[inbound]: own heartbeat", msg);
		return;

	} else {
		crm_devel("Processing message");
		crm_log_message_adv(LOG_MSG, "HA[inbound]", msg);
		new_input = new_ha_msg_input(msg);
		register_fsa_input(C_HA_MESSAGE, I_ROUTER, new_input);
	}

	
#if 0
	if(ha_msg_value(msg, XML_ATTR_REFERENCE) == NULL) {
		ha_msg_add(new_input->msg, XML_ATTR_REFERENCE, seq);
	}
#endif

	delete_ha_msg_input(new_input);

	return;
}

/*
 * Apparently returning TRUE means "stay connected, keep doing stuff".
 * Returning FALSE means "we're all done, close the connection"
 */
gboolean
crmd_ipc_msg_callback(IPC_Channel *client, gpointer user_data)
{
	int lpc = 0;
	IPC_Message *msg = NULL;
	ha_msg_input_t *new_input = NULL;
	gboolean hack_return_good = TRUE;
	crmd_client_t *curr_client = (crmd_client_t*)user_data;

	crm_verbose("Processing IPC message from %s",
		   curr_client->table_key);

	while(client->ops->is_message_pending(client)) {
		if (client->ch_status == IPC_DISCONNECT) {
			/* The message which was pending for us is that
			 * the IPC status is now IPC_DISCONNECT */
			break;
		}
		if (client->ops->recv(client, &msg) != IPC_OK) {
			perror("Receive failure:");
			crm_err("[%s] [receive failure]", curr_client->table_key);
			return !hack_return_good;
		} else if (msg == NULL) {
			crm_err("No message from %s this time", curr_client->table_key);
			continue;
		}

		lpc++;
		new_input = new_ipc_msg_input(msg);
		msg->msg_done(msg);
		
		crm_verbose("Processing msg from %s", curr_client->table_key);
		crm_log_message_adv(LOG_MSG, "CRMd[inbound]", new_input->msg);
		crmd_authorize_message(new_input, curr_client);
		delete_ha_msg_input(new_input);
		
		msg = NULL;
		new_input = NULL;
	}

	crm_verbose("Processed %d messages", lpc);
    
	if (client->ch_status == IPC_DISCONNECT) {
		crm_debug("received HUP from %s", curr_client->table_key);
		if (curr_client != NULL) {
			struct crm_subsystem_s *the_subsystem = NULL;
			
			if (curr_client->sub_sys == NULL) {
				crm_debug("Client hadn't registered with us yet");

			} else if (strcmp(CRM_SYSTEM_PENGINE,
					  curr_client->sub_sys) == 0) {
				the_subsystem = pe_subsystem;

			} else if (strcmp(CRM_SYSTEM_TENGINE,
					  curr_client->sub_sys) == 0) {
				the_subsystem = te_subsystem;

			} else if (strcmp(CRM_SYSTEM_CIB,
					  curr_client->sub_sys) == 0){
				the_subsystem = cib_subsystem;
			}
			
			if(the_subsystem != NULL) {
				cleanup_subsystem(the_subsystem);
				s_crmd_fsa(C_FSA_INTERNAL);
				
			} /* else that was a transient client */
			
			if (curr_client->table_key != NULL) {
				/*
				 * Key is destroyed below:
				 *	curr_client->table_key
				 * Value is cleaned up by:
				 *	G_main_del_IPC_Channel
				 */
				g_hash_table_remove(
					ipc_clients, curr_client->table_key);
			}


			if(curr_client->client_source != NULL) {
				gboolean det = G_main_del_IPC_Channel(
					curr_client->client_source);
			
				crm_verbose("crm_client was %s detached",
					   det?"successfully":"not");
			}
			
			crm_free(curr_client->table_key);
			crm_free(curr_client->sub_sys);
			crm_free(curr_client->uuid);
			crm_free(curr_client);
		}
		return !hack_return_good;
	}
    
	return hack_return_good;
}


void
lrm_op_callback(lrm_op_t* op)
{
	/* todo: free op->rsc */
	crm_devel("received callback");
	register_fsa_input(C_LRM_OP_CALLBACK, I_LRM_EVENT, op);
	s_crmd_fsa(C_LRM_OP_CALLBACK);
}

void
crmd_ha_status_callback(
	const char *node, const char * status,	void* private_data)
{
	crm_data_t *update      = NULL;

	crm_devel("received callback");
	crm_notice("Status update: Node %s now has status [%s]",node,status);

	if(AM_I_DC == FALSE) {
		crm_devel("Got nstatus callback in non-DC mode");
		return;
		
	} else if(safe_str_neq(status, DEADSTATUS)) {
		crm_devel("nstatus callback was not for a dead node");
		return;
	}

	/* this node is taost */
	update = create_node_state(
		node, node, status, NULL, NULL, NULL, NULL);
	
	set_xml_property_copy(
		update, XML_CIB_ATTR_CLEAR_SHUTDOWN, XML_BOOLEAN_TRUE);
	
	update_local_cib(create_cib_fragment(update, NULL));
	s_crmd_fsa(C_FSA_INTERNAL);
	free_xml(update);
}


void
crmd_client_status_callback(const char * node, const char * client,
		 const char * status, void * private)
{
	const char    *join = NULL;
	const char   *extra = NULL;
	crm_data_t *  update = NULL;

	crm_devel("received callback");
	if(safe_str_neq(client, CRM_SYSTEM_CRMD)) {
		return;
	}

	set_bit_inplace(fsa_input_register, R_PEER_DATA);
	
	if(safe_str_eq(status, JOINSTATUS)){
		status = ONLINESTATUS;
		extra  = XML_CIB_ATTR_CLEAR_SHUTDOWN;

	} else if(safe_str_eq(status, LEAVESTATUS)){
		status = OFFLINESTATUS;
		join   = CRMD_JOINSTATE_DOWN;
		extra  = XML_CIB_ATTR_CLEAR_SHUTDOWN;
	}
	
	crm_notice("Status update: Client %s/%s now has status [%s]",
		   node, client, status);

	if(safe_str_eq(node, fsa_our_dc) && status == OFFLINESTATUS) {
		/* did our DC leave us */
		crm_info("Got client status callback - our DC is dead");
		register_fsa_input(C_CRMD_STATUS_CALLBACK, I_ELECTION, NULL);
		
	} else if(AM_I_DC == FALSE) {
		crm_devel("Got client status callback in non-DC mode");
		return;
		
	} else {
		crm_devel("Got client status callback in DC mode");
		update = create_node_state(
			node, node, NULL, NULL, status, join, NULL);
		
		set_xml_property_copy(update, extra, XML_BOOLEAN_TRUE);
		update_local_cib(create_cib_fragment(update, NULL));
		free_xml(update);
	}
	
	s_crmd_fsa(C_CRMD_STATUS_CALLBACK);
}


gboolean lrm_dispatch(int fd, gpointer user_data)
{
	int num_msgs = 0;
	ll_lrm_t *lrm = (ll_lrm_t*)user_data;
	crm_devel("received callback");
	num_msgs = lrm->lrm_ops->rcvmsg(lrm, FALSE);
	if(num_msgs < 1) {
		crm_err("lrm->lrm_ops->rcvmsg() failed, connection lost?");
		clear_bit_inplace(fsa_input_register, R_LRM_CONNECTED);
		register_fsa_input(C_FSA_INTERNAL, I_ERROR, NULL);
		s_crmd_fsa(C_FSA_INTERNAL);
		return FALSE;
	}
	return TRUE;
}

/* #define MAX_EMPTY_CALLBACKS 20 */
/* int empty_callbacks = 0; */

gboolean
crmd_ha_msg_dispatch(IPC_Channel *channel, gpointer user_data)
{
	int lpc = 0;
	ll_cluster_t *hb_cluster = (ll_cluster_t*)user_data;

	while(hb_cluster->llc_ops->msgready(hb_cluster)) {
		if(channel->ch_status != IPC_CONNECT) {
			/* there really is no point continuing */
			break;
		}
 		lpc++; 
		/* invoke the callbacks but dont block */
		hb_cluster->llc_ops->rcvmsg(hb_cluster, 0);
	}

	crm_devel("%d HA messages dispatched", lpc);
	s_crmd_fsa(C_HA_MESSAGE);

	if (channel && (channel->ch_status != IPC_CONNECT)) {
		crm_crit("Lost connection to heartbeat service.");
		return FALSE;
	}
    
	return TRUE;
}

void
crmd_ha_connection_destroy(gpointer user_data)
{
	crm_crit("Heartbeat has left us");
	/* this is always an error */
	/* feed this back into the FSA */
	register_fsa_input(C_HA_DISCONNECT, I_ERROR, NULL);
	s_crmd_fsa(C_HA_DISCONNECT);
}


gboolean
crmd_client_connect(IPC_Channel *client_channel, gpointer user_data)
{
	if (client_channel == NULL) {
		crm_err("Channel was NULL");

	} else if (client_channel->ch_status == IPC_DISCONNECT) {
		crm_err("Channel was disconnected");

	} else {
		crmd_client_t *blank_client = NULL;
		crm_devel("Channel connected");
		crm_malloc(blank_client, sizeof(crmd_client_t));
	
		if (blank_client == NULL) {
			return FALSE;
		}
		
		client_channel->ops->set_recv_qlen(client_channel, 100);
		client_channel->ops->set_send_qlen(client_channel, 100);
	
		blank_client->client_channel = client_channel;
		blank_client->sub_sys   = NULL;
		blank_client->uuid      = NULL;
		blank_client->table_key = NULL;
	
		blank_client->client_source =
			G_main_add_IPC_Channel(
				G_PRIORITY_LOW, client_channel,
				FALSE,  crmd_ipc_msg_callback,
				blank_client, default_ipc_connection_destroy);
	}
    
	return TRUE;
}


gboolean ccm_dispatch(int fd, gpointer user_data)
{
	int rc = 0;
	oc_ev_t *ccm_token = (oc_ev_t*)user_data;
	crm_devel("received callback");	
	rc = oc_ev_handle_event(ccm_token);
	if(0 == rc) {
		return TRUE;

	} else {
		crm_err("CCM connection appears to have failed: rc=%d.", rc);
		register_fsa_input(C_CCM_CALLBACK, I_ERROR, NULL);
		s_crmd_fsa(C_CCM_CALLBACK);
		return FALSE;
	}
}


void 
crmd_ccm_msg_callback(
	oc_ed_t event, void *cookie, size_t size, const void *data)
{
	struct crmd_ccm_data_s *event_data = NULL;
	crm_devel("received callback");
	
	if(data != NULL) {
		crm_malloc(event_data, sizeof(struct crmd_ccm_data_s));

		if(event_data != NULL) {
			event_data->event = &event;
			event_data->oc = copy_ccm_oc_data(
				(const oc_ev_membership_t *)data);

			crm_devel("Sending callback to the FSA");
			register_fsa_input(
				C_CCM_CALLBACK, I_CCM_EVENT,
				(void*)event_data);

			s_crmd_fsa(C_CCM_CALLBACK);
			
			event_data->event = NULL;
			event_data->oc = NULL;

			crm_free(event_data);
		}

	} else {
		crm_info("CCM Callback with NULL data... "
		       "I dont /think/ this is bad");
	}
	
	oc_ev_callback_done(cookie);
	
	return;
}

void
crmd_cib_connection_destroy(gpointer user_data)
{
	if(is_set(fsa_input_register, R_SHUTDOWN)) {
		crm_info("Connection to the CIB terminated...");
		return;
	}

	/* eventually this will trigger a reconnect, not a shutdown */ 
	crm_err("Connection to the CIB terminated...");
	register_fsa_input(C_FSA_INTERNAL, I_ERROR, NULL);
	clear_bit_inplace(fsa_input_register, R_CIB_CONNECTED);
	
	s_crmd_fsa(C_FSA_INTERNAL);
	return;
}
