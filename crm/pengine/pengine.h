/* $Id: pengine.h,v 1.67 2005/06/02 16:20:22 gshi Exp $ */
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
#ifndef PENGINE__H
#define PENGINE__H

#include <clplumbing/ipc.h>

typedef struct node_s node_t;
typedef struct color_s color_t;
typedef struct rsc_to_node_s rsc_to_node_t;
typedef struct rsc_colocation_s rsc_colocation_t;
typedef struct resource_s resource_t;
typedef struct lrm_agent_s lrm_agent_t;
typedef struct order_constraint_s order_constraint_t;
typedef struct action_s action_t;
typedef struct action_wrapper_s action_wrapper_t;

#include <glib.h>
#include <crm/crm.h>
#include <crm/common/msg.h>

typedef enum no_quorum_policy_e {
	no_quorum_freeze,
	no_quorum_stop,
	no_quorum_ignore
} no_quorum_policy_t;

typedef struct pe_working_set_s 
{
		crm_data_t *input;

		/* options extracted from the input */
		char *dc_uuid;
		gboolean have_quorum;
		gboolean stonith_enabled;
		gboolean symmetric_cluster;
		no_quorum_policy_t no_quorum_policy;

		/* intermediate steps */
		color_t *no_color;
		
		GListPtr nodes;
		GListPtr resources;
		GListPtr placement_constraints;
		GListPtr ordering_constraints;
		
		GListPtr colors;
		GListPtr actions;

		/* stats */
		int num_synapse;
		int max_valid_nodes;
		int order_id;
		int action_id;
		int color_id;

		/* final output */
		crm_data_t *graph;

} pe_working_set_t;

#include <complex.h>

enum con_type {
	type_none,
	rsc_colocation,
	rsc_to_node,
	rsc_to_attr,
	base_weight
};

enum node_type {
	node_ping,
	node_member
};

enum con_strength {
	pecs_ignore,
	pecs_must,
	pecs_must_not,
	pecs_startstop
};

enum action_tasks {
	no_action,
	monitor_rsc,
	stop_rsc,
	stopped_rsc,
	start_rsc,
	started_rsc,
	shutdown_crm,
	stonith_node
};

enum rsc_recovery_type {
	recovery_stop_start,
	recovery_stop_only,
	recovery_block
};


enum pe_stop_fail {
	pesf_block,
	pesf_stonith,
	pesf_ignore
};

enum pe_restart {
	pe_restart_restart,
	pe_restart_ignore
};

enum pe_ordering {
	pe_ordering_manditory,
	pe_ordering_optional
};

struct node_shared_s { 
		const char *id; 
		const char *uname; 
		gboolean online;
		gboolean unclean;
		gboolean shutdown;
		gboolean expected_up;
		gboolean is_dc;
		int	 num_resources;
		GListPtr running_rsc;	/* resource_t* */
		
		GHashTable *attrs;	/* char* => char* */
		enum node_type type;
}; 

struct node_s { 
		float	weight; 
		gboolean fixed;
		struct node_shared_s *details;
};

struct color_shared_s {
		int      id;
		float    highest_priority;
		GListPtr candidate_nodes; /* node_t* */
		GListPtr allocated_resources; /* resources_t* */
		node_t  *chosen_node;
		gboolean pending;
		int	 num_resources;
};

struct color_s { 
		int id; 
		struct color_shared_s *details;
		float local_weight;
};

struct rsc_colocation_s { 
		const char	*id;
		resource_t	*rsc_lh; 

		resource_t	*rsc_rh; 
		enum con_strength strength;
};

struct rsc_to_node_s { 
		const char *id;
		resource_t *rsc_lh; 

		float	    weight;
		GListPtr    node_list_rh; /* node_t* */
};

struct lrm_agent_s { 
		const char *class;
		const char *type;
		const char *version;
};

struct resource_s { 
		const char *id; 
		crm_data_t *xml; 
		crm_data_t *ops_xml; 

		void *variant_opaque;
		enum pe_obj_types variant;
		resource_object_functions_t *fns;

		enum rsc_recovery_type recovery_type;
		enum pe_stop_fail      stopfail_type;
		enum pe_restart        restart_type;

		float	 priority; 
		float	 effective_priority; 

		gboolean start_pending;
		gboolean recover;
		gboolean starting;
		gboolean stopping;
		gboolean is_stonith;
		gboolean runnable;
		gboolean provisional;
		gboolean unclean;

		GListPtr candidate_colors; /* color_t*          */
		GListPtr rsc_cons;         /* rsc_colocation_t* */
		GListPtr actions;	   /* action_t*         */

		GHashTable * parameters;
};

struct action_wrapper_s 
{
		enum pe_ordering type;
		action_t *action;
};


struct action_s 
{
		int         id;
		resource_t *rsc;
		void       *rsc_opaque;
		node_t     *node;
		const char *task;

		char *uuid;
		crm_data_t *op_entry;
		
		gboolean pseudo;
		gboolean runnable;
		gboolean optional;
		gboolean failure_is_fatal;

		gboolean dumped;
		gboolean processed;
		
		int seen_count;

/* 		crm_data_t *args; */
		GHashTable *extra;
		
		GListPtr actions_before; /* action_warpper_t* */
		GListPtr actions_after;  /* action_warpper_t* */
};

struct order_constraint_s 
{
		int id;
		enum pe_ordering type;

		void *lh_opaque;
		resource_t *lh_rsc;
		action_t   *lh_action;
		char *lh_action_task;
		
		void *rh_opaque;
		resource_t *rh_rsc;
		action_t   *rh_action;
		char *rh_action_task;

		/* (soon to be) variant specific */
/* 		int   lh_rsc_incarnation; */
/* 		int   rh_rsc_incarnation; */
};


extern gboolean stage0(pe_working_set_t *data_set);
extern gboolean stage1(pe_working_set_t *data_set);
extern gboolean stage2(pe_working_set_t *data_set);
extern gboolean stage3(pe_working_set_t *data_set);
extern gboolean stage4(pe_working_set_t *data_set);
extern gboolean stage5(pe_working_set_t *data_set);
extern gboolean stage6(pe_working_set_t *data_set);
extern gboolean stage7(pe_working_set_t *data_set);
extern gboolean stage8(pe_working_set_t *data_set);

extern gboolean summary(GListPtr resources);

extern gboolean pe_msg_dispatch(IPC_Channel *sender, void *user_data);

extern gboolean process_pe_message(
	HA_Message *msg, crm_data_t *xml_data, IPC_Channel *sender);

extern gboolean unpack_constraints(
	crm_data_t *xml_constraints, pe_working_set_t *data_set);

extern gboolean unpack_resources(
	crm_data_t *xml_resources, pe_working_set_t *data_set);

extern gboolean unpack_config(crm_data_t *config, pe_working_set_t *data_set);

extern gboolean unpack_nodes(crm_data_t *xml_nodes, pe_working_set_t *data_set);

extern gboolean unpack_status(crm_data_t *status, pe_working_set_t *data_set);

extern gboolean apply_placement_constraints(pe_working_set_t *data_set);

extern void color_resource(resource_t *lh_resource, pe_working_set_t *data_set);

extern gboolean choose_node_from_list(color_t *color);

extern gboolean update_action_states(GListPtr actions);

extern gboolean shutdown_constraints(
	node_t *node, action_t *shutdown_op, pe_working_set_t *data_set);

extern gboolean stonith_constraints(
	node_t *node, action_t *stonith_op, action_t *shutdown_op,
	pe_working_set_t *data_set);

extern gboolean custom_action_order(
	resource_t *lh_rsc, char *lh_task, action_t *lh_action,
	resource_t *rh_rsc, char *rh_task, action_t *rh_action,
	enum pe_ordering type, pe_working_set_t *data_set);

#define order_start_start(rsc1,rsc2)					\
	custom_action_order(rsc1, start_key(rsc1), NULL,		\
			    rsc2, start_key(rsc2) ,NULL,		\
			    pe_ordering_manditory, data_set)
#define order_stop_stop(rsc1, rsc2)					\
	custom_action_order(rsc1, stop_key(rsc1), NULL,		\
			    rsc2, stop_key(rsc2) ,NULL,		\
			    pe_ordering_manditory, data_set)
#define order_stop_start(rsc1, rsc2)					\
	custom_action_order(rsc1, stop_key(rsc1), NULL,		\
			    rsc2, start_key(rsc2) ,NULL,		\
			    pe_ordering_optional, data_set)
#define order_start_stop(rsc1, rsc2)					\
	custom_action_order(rsc1, start_key(rsc1), NULL,		\
			    rsc2, stop_key(rsc2) ,NULL,		\
			    pe_ordering_manditory, data_set)

#define pe_err(fmt...) { was_processing_error = TRUE; crm_err(fmt); }
#define pe_warn(fmt...) { was_processing_warning = TRUE; crm_warn(fmt); }

extern gboolean process_colored_constraints(resource_t *rsc);
extern void graph_element_from_action(
	action_t *action, pe_working_set_t *data_set);
extern void set_working_set_defaults(pe_working_set_t *data_set);
extern void cleanup_calculations(pe_working_set_t *data_set);

extern const char* transition_timeout;
extern gboolean was_processing_error;
extern gboolean was_processing_warning;

#endif

