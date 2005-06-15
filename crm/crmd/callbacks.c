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

GHashTable *crmd_peer_state = NULL;

crm_data_t *find_xml_in_hamessage(const HA_Message * msg);
void crmd_ha_connection_destroy(gpointer user_data);

/* From join_dc... */
extern gboolean check_join_state(
	enum crmd_fsa_state cur_state, const char *source);


/* #define MAX_EMPTY_CALLBACKS 20 */
/* int empty_callbacks = 0; */

gboolean
crmd_ha_msg_dispatch(IPC_Channel *channel, gpointer user_data)
{
	int lpc = 0;
	ll_cluster_t *hb_cluster = (ll_cluster_t*)user_data;

	while(lpc < 2 && hb_cluster->llc_ops->msgready(hb_cluster)) {
		if(channel->ch_status != IPC_CONNECT) {
			/* there really is no point continuing */
			break;
		}
 		lpc++; 
		/* invoke the callbacks but dont block */
		hb_cluster->llc_ops->rcvmsg(hb_cluster, 0);
	}

	crm_debug_3("%d HA messages dispatched", lpc);
	G_main_set_trigger(fsa_source);
	
	if (channel && (channel->ch_status != IPC_CONNECT)) {
		crm_crit("Lost connection to heartbeat service.");
		return FALSE;
	}
    
	return TRUE;
}


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
			  " connected to the CCM (%s op from %s)", op, from);
		crm_log_message_adv(
			LOG_MSG, "HA[inbound]: Ignore (No CCM)", msg);
		return;
	}
	
	from_node = g_hash_table_lookup(fsa_membership_copy->members, from);

	if(from_node == NULL) {
		int level = LOG_DEBUG;
		if(safe_str_eq(op, CRM_OP_VOTE)) {
			level = LOG_WARNING;

		} else if(AM_I_DC && safe_str_eq(op, CRM_OP_JOIN_ANNOUNCE)) {
			level = LOG_WARNING;

		} else if(safe_str_eq(sys_from, CRM_SYSTEM_DC)) {
			level = LOG_WARNING;
		}
		do_crm_log(level, __FILE__, __FUNCTION__, 
			   "Ignoring HA message (op=%s) from %s: not in our"
			   " membership list (size=%d)", op, from,
			   g_hash_table_size(fsa_membership_copy->members));
		
		crm_log_message_adv(LOG_MSG, "HA[inbound]: CCM Discard", msg);

	} else if(AM_I_DC
	   && safe_str_eq(sys_from, CRM_SYSTEM_DC)
	   && safe_str_neq(from, fsa_our_uname)) {
		crm_err("Another DC detected: %s (op=%s)", from, op);
		crm_log_message_adv(
			LOG_WARNING, "HA[inbound]: Duplicate DC", msg);
		new_input = new_ha_msg_input(msg);

		/* make sure the election happens NOW */
		register_fsa_error_adv(C_FSA_INTERNAL, I_ELECTION, NULL,
				       new_input, __FUNCTION__);
		
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
		crm_debug_2("Ignoring message for the DC [F_SEQ=%s]", seq);
		crm_log_message_adv(LOG_DEBUG_4, "HA[inbound]: ignore", msg);
		return;

	} else if(safe_str_eq(from, fsa_our_uname)
		  && safe_str_eq(op, CRM_OP_VOTE)) {
		crm_log_message_adv(LOG_DEBUG_4, "HA[inbound]", msg);
		crm_debug_2("Ignoring our own vote [F_SEQ=%s]: own vote", seq);
		return;
		
	} else if(AM_I_DC && safe_str_eq(op, CRM_OP_HBEAT)) {
		crm_debug_2("Ignoring our own heartbeat [F_SEQ=%s]", seq);
		crm_log_message_adv(LOG_DEBUG_4, "HA[inbound]: own heartbeat", msg);
		return;

	} else {
		crm_debug_3("Processing message");
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
	crmd_client_t *curr_client = (crmd_client_t*)user_data;
	gboolean stay_connected = TRUE;
	
	crm_debug_2("Processing IPC message from %s",
		   curr_client->table_key);

	while(lpc == 0 && client->ops->is_message_pending(client)) {
		if (client->ch_status == IPC_DISCONNECT) {
			/* The message which was pending for us is that
			 * the IPC status is now IPC_DISCONNECT */
			break;
		}
		if (client->ops->recv(client, &msg) != IPC_OK) {
			perror("Receive failure:");
			crm_err("[%s] [receive failure]",
				curr_client->table_key);
			stay_connected = FALSE;
			break;
			
		} else if (msg == NULL) {
			crm_err("[%s] [no message this time]",
				curr_client->table_key);
			continue;
		}

		lpc++;
		new_input = new_ipc_msg_input(msg);
		msg->msg_done(msg);
		
		crm_debug_2("Processing msg from %s", curr_client->table_key);
		crm_log_message_adv(LOG_MSG, "CRMd[inbound]", new_input->msg);
		if(crmd_authorize_message(new_input, curr_client)) {
			register_fsa_input(C_IPC_MESSAGE, I_ROUTER, new_input);
		}
		delete_ha_msg_input(new_input);
		
		msg = NULL;
		new_input = NULL;
	}
	
	crm_debug_2("Processed %d messages", lpc);
    
	if (client->ch_status == IPC_DISCONNECT) {
		stay_connected = FALSE;
		process_client_disconnect(curr_client);
	}

	G_main_set_trigger(fsa_source);
	return stay_connected;
}



gboolean
lrm_dispatch(IPC_Channel*src_not_used, gpointer user_data)
{
	int num_msgs = 0;
	ll_lrm_t *lrm = (ll_lrm_t*)user_data;
	crm_debug_3("received callback");
	num_msgs = lrm->lrm_ops->rcvmsg(lrm, FALSE);
	if(num_msgs < 1) {
		crm_err("lrm->lrm_ops->rcvmsg() failed, connection lost?");
		clear_bit_inplace(fsa_input_register, R_LRM_CONNECTED);
		register_fsa_input(C_FSA_INTERNAL, I_ERROR, NULL);
		return FALSE;
	}
	return TRUE;
}

void
lrm_op_callback(lrm_op_t* op)
{
	CRM_DEV_ASSERT(op != NULL);
	if(crm_assert_failed) {
		return;
	}
	
	crm_debug("received callback: %s/%s (%s)",
		  op->op_type, op->rsc_id, op_status2text(op->op_status));

	/* Make sure the LRM events are received in order */
	register_fsa_input_later(C_LRM_OP_CALLBACK, I_LRM_EVENT, op);
}

void
crmd_ha_status_callback(
	const char *node, const char * status,	void* private_data)
{
	crm_data_t *update = NULL;

	crm_debug_3("received callback");
	crm_notice("Status update: Node %s now has status [%s]",node,status);

	if(safe_str_neq(status, DEADSTATUS)) {
		crm_debug_3("nstatus callback was not for a dead node");
		return;
	}

	/* this node is taost */
	update = create_node_state(
		node, node, status, NULL, NULL, NULL, NULL, __FUNCTION__);
	
	crm_xml_add(update, XML_CIB_ATTR_CLEAR_SHUTDOWN, XML_BOOLEAN_TRUE);

	/* this change should not be broadcast */
	update_local_cib(create_cib_fragment(update, NULL));
	G_main_set_trigger(fsa_source);
	free_xml(update);
}


void
crmd_client_status_callback(const char * node, const char * client,
		 const char * status, void * private)
{
	const char    *join = NULL;
	const char   *extra = NULL;
	crm_data_t *  update = NULL;

	crm_debug_3("received callback");
	if(safe_str_neq(client, CRM_SYSTEM_CRMD)) {
		return;
	}

	if(safe_str_eq(status, JOINSTATUS)){
		status = ONLINESTATUS;
		extra  = XML_CIB_ATTR_CLEAR_SHUTDOWN;

	} else if(safe_str_eq(status, LEAVESTATUS)){
		status = OFFLINESTATUS;
		join   = CRMD_JOINSTATE_DOWN;
		extra  = XML_CIB_ATTR_CLEAR_SHUTDOWN;
	}
	
	set_bit_inplace(fsa_input_register, R_PEER_DATA);
	g_hash_table_replace(
		crmd_peer_state, crm_strdup(node), crm_strdup(status));

	if(fsa_state == S_STARTING || fsa_state == S_STOPPING) {
		return;
	}

	crm_notice("Status update: Client %s/%s now has status [%s]",
		   node, client, status);

	if(safe_str_eq(node, fsa_our_dc)
	   && safe_str_eq(status, OFFLINESTATUS)) {
		/* did our DC leave us */
		crm_info("Got client status callback - our DC is dead");
		register_fsa_input(C_CRMD_STATUS_CALLBACK, I_ELECTION, NULL);
		
	} else {
		crm_data_t *fragment = NULL;
		crm_debug_3("Got client status callback");
		update = create_node_state(
			node, node, NULL, NULL, status, join, NULL,
			__FUNCTION__);
		
		crm_xml_add(update, extra, XML_BOOLEAN_TRUE);
		fragment = create_cib_fragment(update, NULL);

		/* it is safe to keep these updates on the local node
		 * each node updates their own CIB
		 */
		fsa_cib_conn->cmds->modify(
			fsa_cib_conn, XML_CIB_TAG_STATUS, fragment, NULL,
			cib_inhibit_bcast|cib_scope_local|cib_quorum_override);

		free_xml(fragment);
		free_xml(update);

		if(AM_I_DC && safe_str_eq(status, OFFLINESTATUS)) {

			g_hash_table_remove(confirmed_nodes,  node);
			g_hash_table_remove(finalized_nodes,  node);
			g_hash_table_remove(integrated_nodes, node);
			g_hash_table_remove(welcomed_nodes,   node);
			
			check_join_state(fsa_state, __FUNCTION__);
		}
	}
	
	G_main_set_trigger(fsa_source);
}

void
crmd_ha_connection_destroy(gpointer user_data)
{
	crm_crit("Heartbeat has left us");
	/* this is always an error */
	/* feed this back into the FSA */
	register_fsa_input(C_HA_DISCONNECT, I_ERROR, NULL);
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
		crm_debug_3("Channel connected");
		crm_malloc0(blank_client, sizeof(crmd_client_t));
	
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
	gboolean was_error = FALSE;
	
	crm_debug_3("received callback");	
	rc = oc_ev_handle_event(ccm_token);
	if(rc != 0) {
		crm_err("CCM connection appears to have failed: rc=%d.", rc);
		register_fsa_input(C_CCM_CALLBACK, I_ERROR, NULL);
		was_error = TRUE;
	}
	G_main_set_trigger(fsa_source);
	return !was_error;
}

static gboolean fsa_have_quorum = FALSE;

void 
crmd_ccm_msg_callback(
	oc_ed_t event, void *cookie, size_t size, const void *data)
{
	int instance = -1;
	gboolean update_cache = FALSE;
	struct crmd_ccm_data_s *event_data = NULL;
	const oc_ev_membership_t *membership = data;

	gboolean update_quorum = FALSE;
	gboolean trigger_transition = FALSE;

	crm_debug_3("received callback");

	if(data != NULL) {
		instance = membership->m_instance;
	}
	
	crm_info("Quorum %s after event=%s (id=%d)", 
		 ccm_have_quorum(event)?"(re)attained":"lost",
		 ccm_event_name(event), instance);
	
	switch(event) {
		case OC_EV_MS_NEW_MEMBERSHIP:
		case OC_EV_MS_INVALID:/* fall through */
			update_cache = TRUE;
			update_quorum = TRUE;
			break;
		case OC_EV_MS_NOT_PRIMARY:
#if UNTESTED
			if(AM_I_DC == FALSE) {
				break;
			}
			/* tell the TE to pretend it had completed and stop */
			/* side effect: we'll end up in S_IDLE */
			register_fsa_action(A_TE_HALT, TRUE);
#endif
			break;
		case OC_EV_MS_PRIMARY_RESTORED:
			fsa_membership_copy->id = instance;
			if(AM_I_DC && need_transition(fsa_state)) {
				trigger_transition = TRUE;
			}
			break;
		case OC_EV_MS_EVICTED:
			update_quorum = TRUE;
			register_fsa_input(C_FSA_INTERNAL, I_STOP, NULL);
			break;
		default:
			crm_err("Unknown CCM event: %d", event);
	}

	if(update_quorum && ccm_have_quorum(event) == FALSE) {
		/* did we just loose quorum? */
		if(fsa_have_quorum && need_transition(fsa_state)) {
			crm_info("Quorum lost: triggering transition (%s)",
				 ccm_event_name(event));
			trigger_transition = TRUE;
		}
		fsa_have_quorum = FALSE;
			
	} else if(update_quorum)  {
		crm_debug("Updating quorum after event %s",
			  ccm_event_name(event));
		fsa_have_quorum = TRUE;
	}

	if(trigger_transition) {
		crm_debug("Scheduling transition after event %s",
			  ccm_event_name(event));
		/* make sure that when we query the CIB that it has
		 * the changes that triggered the transition
		 */
		switch(event) {
			case OC_EV_MS_NEW_MEMBERSHIP:
			case OC_EV_MS_INVALID:
			case OC_EV_MS_PRIMARY_RESTORED:
				fsa_membership_copy->id = instance;
				break;
			default:
				break;
		}
		if(update_cache == FALSE) {
			/* a stand-alone transition */
			register_fsa_action(A_TE_CANCEL);
		}
	}
	if(update_cache) {
		crm_debug("Updating cache after event %s",
			  ccm_event_name(event));

		crm_malloc0(event_data, sizeof(struct crmd_ccm_data_s));
		if(event_data == NULL) { return; }
		
		event_data->event = event;
		if(data != NULL) {
			event_data->oc = copy_ccm_oc_data(data);
		}
		register_fsa_input_adv(
			C_CCM_CALLBACK, I_CCM_EVENT, event_data,
			trigger_transition?A_TE_CANCEL:A_NOTHING,
			FALSE, __FUNCTION__);
		
		if (event_data->oc) {
			crm_free(event_data->oc);
			event_data->oc = NULL;
		}
		crm_free(event_data);
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
	
	return;
}

longclock_t fsa_start = 0;
longclock_t fsa_stop = 0;
longclock_t fsa_diff = 0;

gboolean
crm_fsa_trigger(gpointer user_data) 
{
	unsigned int fsa_diff_ms = 0;
	if(fsa_diff_max_ms > 0) {
		fsa_start = time_longclock();
	}
	s_crmd_fsa(C_FSA_INTERNAL);
	if(fsa_diff_max_ms > 0) {
		fsa_stop = time_longclock();
		fsa_diff = sub_longclock(fsa_stop, fsa_start);
		fsa_diff_ms = longclockto_ms(fsa_diff);
		if(fsa_diff_ms > fsa_diff_max_ms) {
			crm_err("FSA took %dms to complete", fsa_diff_ms);

		} else if(fsa_diff_ms > fsa_diff_warn_ms) {
			crm_warn("FSA took %dms to complete", fsa_diff_ms);
		}
		
	}
	return TRUE;	
}
