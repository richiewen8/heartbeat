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
#include <crmd_fsa.h>

#include <sys/types.h>
#include <sys/wait.h>

#include <unistd.h>			/* for access */
#include <clplumbing/cl_signal.h>
#include <clplumbing/realtime.h>
#include <sys/types.h>	/* for calls to open */
#include <sys/stat.h>	/* for calls to open */
#include <fcntl.h>	/* for calls to open */
#include <pwd.h>	/* for getpwuid */
#include <grp.h>	/* for initgroups */

#include <sys/time.h>	/* for getrlimit */
#include <sys/resource.h>/* for getrlimit */

#include <errno.h>

#include <crm/msg_xml.h>
#include <crm/common/xml.h>
#include <crmd_messages.h>
#include <crmd_callbacks.h>

#include <crm/cib.h>
#include <crmd.h>

#include <crm/dmalloc_wrapper.h>

struct crm_subsystem_s *te_subsystem  = NULL;


/*	 A_TE_START, A_TE_STOP, A_TE_RESTART	*/
enum crmd_fsa_input
do_te_control(long long action,
	      enum crmd_fsa_cause cause,
	      enum crmd_fsa_state cur_state,
	      enum crmd_fsa_input current_input,
	      fsa_data_t *msg_data)
{
	enum crmd_fsa_input result = I_NULL;
	struct crm_subsystem_s *this_subsys = te_subsystem;
	
	long long stop_actions = A_TE_STOP;
	long long start_actions = A_TE_START;
	int lpc, pid_status;
	
/* 		if(action & stop_actions && cur_state != S_STOPPING */
/* 		   && is_set(fsa_input_register, R_TE_PEND)) { */
/* 			result = I_WAIT_FOR_EVENT; */
/* 			return result; */
/* 		} */
	
	if(action & stop_actions) {
		if(stop_subsystem(this_subsys) == FALSE)
			result = I_FAIL;
		else if(this_subsys->pid > 0){
			lpc = CLIENT_EXIT_WAIT;
			pid_status = -1;
			while(lpc-- > 0 && this_subsys->pid > 0) {

				sleep(1);

				if(! CL_PID_EXISTS(this_subsys->pid)
				   || waitpid(this_subsys->pid,
					      &pid_status, WNOHANG) > 0) {
					this_subsys->pid = -1;
					break;
				}
			}
			
			if(this_subsys->pid != -1) {
				crm_err("Proc %s is still active with pid=%d",
				       this_subsys->name, this_subsys->pid);
				result = I_FAIL;
			} 
		}

		cleanup_subsystem(this_subsys);
	}

	if(action & start_actions) {

		if(cur_state != S_STOPPING) {
			if(start_subsystem(this_subsys) == FALSE) {
				result = I_FAIL;
				cleanup_subsystem(this_subsys);
			}
		} else {
			crm_info("Ignoring request to start %s while shutting down",
				 this_subsys->name);
		}
	}

	return result;
}

static xmlNodePtr te_last_input = NULL;

/*	 A_TE_INVOKE, A_TE_CANCEL	*/
enum crmd_fsa_input
do_te_invoke(long long action,
	     enum crmd_fsa_cause cause,
	     enum crmd_fsa_state cur_state,
	     enum crmd_fsa_input current_input,
	     fsa_data_t *msg_data)
{
	xmlNodePtr graph = NULL;
	xmlNodePtr msg = (xmlNodePtr)msg_data->data;
	enum crmd_fsa_input ret = I_NULL;

	if(is_set(fsa_input_register, R_TE_CONNECTED) == FALSE){
		crm_info("Waiting for the TE to connect");
		if(msg != NULL) {
			free_xml(te_last_input);
			te_last_input = copy_xml_node_recursive(msg);
		}

		crmd_fsa_stall();
		return I_NULL;		
	}

	if(msg == NULL) {
		msg = te_last_input;
	}
	
	if(action & A_TE_INVOKE) {
		graph = find_xml_node(msg, XML_TAG_GRAPH);
		if(graph != NULL) {
			send_request(NULL, graph, CRM_OP_TRANSITION,
				     NULL, CRM_SYSTEM_TENGINE, NULL);
		} else {
			ret = I_FAIL;
		}
	
	} else if(action & A_TE_CANCEL) {
		send_request(NULL, graph, CRM_OP_TEABORT,
			     NULL, CRM_SYSTEM_TENGINE, NULL);
	}

	/* only free it if it was a local copy */
	free_xml(te_last_input);
	te_last_input = NULL;
	
	return ret;
}



