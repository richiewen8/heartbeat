
/*
 * hb_resource: Linux-HA heartbeat resource management code
 *
 * Copyright (C) 2001-2002 Luis Claudio R. Goncalves
 *				<lclaudio@conectiva.com.br>
 * Copyright (C) 1999-2002 Alan Robertson <alanr@unix.sh>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */
#include <portability.h>
#include <ctype.h>
#include <string.h>
#include <sys/types.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <hb_proc.h>
#include <hb_resource.h>
#include <heartbeat_private.h>
#include <hb_api_core.h>
#include <ha_config.h>
#include <setproctitle.h>
#include <clplumbing/cl_signal.h>
#include <clplumbing/realtime.h>

/**************************************************************************
 *
 * This file contains almost all the resource management code for
 * heartbeat.
 *
 * It contains code to handle:
 *	resource takeover
 *	standby processing
 *	STONITH operations.
 *	performing notify_world() type notifications of status changes.
 *
 * We're planning on replacing it with an external process
 * to perform resource management functions as a heartbeat client.
 *
 * In the mean time, we're planning on disentangling it from the main
 * heartbeat code and cleaning it up some.
 *
 * Here are my favorite cleanup tasks:
 *
 * Enahance the standby code to work correctly for the "normal" failback case.
 *
 * Get rid of the "standby_running" timer, and replace it with a gmainloop
 *	timer.
 *
 * Make hooks for processing incoming messages (in heartbeat.c) cleaner
 *	and probably hook them in through a hash table callback hook
 *	or something.
 *
 * Make registration hooks to allow notify_world to be called by pointer.
 *
 * Reduce the dependency on global variables shared between heartbeat.c
 *	and here.
 *
 * Generally Reduce the number of interactions between this code and
 *	heartbeat.c as evidenced by heartbeat_private.h and hb_resource.h
 *
 **************************************************************************/

int		DoManageResources = 1;
int 		nice_failback = 0;
int		other_holds_resources = HB_NO_RSC;
int		other_is_stable = 0; /* F_ISSTABLE */
int		takeover_in_progress = 0;
enum hb_rsc_state resourcestate = HB_R_INIT;
enum standby	going_standby;

enum standby	going_standby = NOT;
longclock_t	standby_running = 0L;

/*
 * A helper to allow us to pass things into the anonproc
 * environment without any warnings about passing const strings
 * being passed into a plain old (non-const) gpointer.
 */
struct hb_const_string {
	const char * str;
};

#define	HB_RSCMGMTPROC(p, s)					\
	{							\
	 	static struct hb_const_string cstr = {(s)};	\
		NewTrackedProc((p), 1, PT_LOGNORMAL		\
		,	&cstr, &hb_rsc_RscMgmtProcessTrackOps);	\
	}


/*
 * A helper function which points at a malloced string.
 */
struct StonithProcHelper {
	char *		nodename;
};
extern ProcTrack_ops ManagedChildTrackOps;

static int	ResourceMgmt_child_count = 0;

static void	StartNextRemoteRscReq(void);
static void	InitRemoteRscReqQueue(void);
static int	send_standby_msg(enum standby state);
static void	go_standby(enum standby who);

static	void	RscMgmtProcessRegistered(ProcTrack* p);
static	void	RscMgmtProcessDied(ProcTrack* p, int status, int signo
,				int exitcode, int waslogged);
static	const char * RscMgmtProcessName(ProcTrack* p);

static	void StonithProcessDied(ProcTrack* p, int status, int signo
,		int exitcode, int waslogged);
static	const char * StonithProcessName(ProcTrack* p);
void	Initiate_Reset(Stonith* s, const char * nodename);
static int FilterNotifications(const char * msgtype);
static int countbystatus(const char * status, int matchornot);

ProcTrack_ops hb_rsc_RscMgmtProcessTrackOps = {
	RscMgmtProcessDied,
	RscMgmtProcessRegistered,
	RscMgmtProcessName
};

static ProcTrack_ops StonithProcessTrackOps = {
	StonithProcessDied,
	NULL,
	StonithProcessName
};

static const char *	rsc_msg[] =	{HB_NO_RESOURCES, HB_LOCAL_RESOURCES
,	HB_FOREIGN_RESOURCES, HB_ALL_RESOURCES};

/*
 * We look at the directory /etc/ha.d/rc.d to see what
 * scripts are there to avoid trying to run anything
 * which isn't there.
 */
static GHashTable* RCScriptNames = NULL;

static void
CreateInitialFilter(void)
{
	DIR*	dp;
	struct dirent*	dep;
	static char foo[] = "bar";
	RCScriptNames = g_hash_table_new(g_str_hash, g_str_equal);

	(void)_heartbeat_h_Id;
	(void)_heartbeat_private_h_Id;
	(void)_ha_msg_h_Id;
	(void)_setproctitle_h_Id;

	if ((dp = opendir(HB_RC_DIR)) == NULL) {
		ha_perror("Cannot open directory " HB_RC_DIR);
		return;
	}
	while((dep = readdir(dp)) != NULL) {
		if (dep->d_name[0] == '.') {
			continue;
		}
		if (ANYDEBUG) {
			ha_log(LOG_DEBUG
			,	"CreateInitialFilter: %s", dep->d_name);
		}
		g_hash_table_insert(RCScriptNames, g_strdup(dep->d_name),foo);
	}
	closedir(dp);
}
static int
FilterNotifications(const char * msgtype)
{
	int		rc;
	if (RCScriptNames == NULL) {
		CreateInitialFilter();
	}
	rc = g_hash_table_lookup(RCScriptNames, msgtype) != NULL;

	if (DEBUGDETAILS) {
		ha_log(LOG_DEBUG
		,	"FilterNotifications(%s) => %d"
		,	msgtype, rc);
	}

	return rc;
}

/* Notify the (external) world of an HA event */
void
notify_world(struct ha_msg * msg, const char * ostatus)
{
/*
 *	We invoke our "rc" script with the following arguments:
 *
 *	0:	RC_ARG0	(always the same)
 *	1:	lowercase version of command ("type" field)
 *
 *	All message fields get put into environment variables
 *
 *	The rc script, in turn, runs the scripts it finds in the rc.d
 *	directory (or whatever we call it... ) with the same arguments.
 *
 *	We set the following environment variables for the RC script:
 *	HA_CURHOST:	the node name we're running on
 *	HA_OSTATUS:	Status of node (before this change)
 *
 */
	struct sigaction sa;
	char		command[STATUSLENG];
	char 		rc_arg0 [] = RC_ARG0;
	char *	const argv[MAXFIELDS+3] = {rc_arg0, command, NULL};
	const char *	fp;
	char *		tp;
	int		pid, status;

	if (!DoManageResources) {
		return;
	}

	tp = command;

	fp  = ha_msg_value(msg, F_TYPE);
	ASSERT(fp != NULL && strlen(fp) < STATUSLENG);

	if (fp == NULL || strlen(fp) >= STATUSLENG
	||	 !FilterNotifications(fp)) {
		return;
	}

	if (ANYDEBUG) {
		ha_log(LOG_DEBUG
		,	"notify_world: invoking %s: OLD status: %s"
		,	RC_ARG0,	(ostatus ? ostatus : "(none)"));
	}


	while (*fp) {
		if (isupper((unsigned int)*fp)) {
			*tp = tolower((unsigned int)*fp);
		}else{
			*tp = *fp;
		}
		++fp; ++tp;
	}
	*tp = EOS;

	switch ((pid=fork())) {

		case -1:	ha_perror("Can't fork to notify world!");
				break;


		case 0:	{	/* Child */
				int	j;
				cl_make_normaltime();
				set_proc_title("%s: notify_world()", cmdname);
				setpgid(0,0);
				CL_SIGACTION(SIGCHLD, NULL, &sa);
				if (sa.sa_handler != SIG_DFL) {
					ha_log(LOG_DEBUG
					,	"notify_world: setting SIGCHLD"
					" Handler to SIG_DFL");
					CL_SIGNAL(SIGCHLD,SIG_DFL);
				}
				for (j=0; j < msg->nfields; ++j) {
					char ename[64];
					snprintf(ename, sizeof(ename), "HA_%s"
					,	msg->names[j]);
					setenv(ename, msg->values[j], 1);
				}
				if (ostatus) {
					setenv(OLDSTATUS, ostatus, 1);
				}
				if (nice_failback) {
					setenv(HANICEFAILBACK, "yes", 1);
				}
				if (ANYDEBUG) {
					ha_log(LOG_DEBUG
					,	"notify_world: Running %s %s"
					,	argv[0], argv[1]);
				}
				execv(RCSCRIPT, argv);

				ha_log(LOG_ERR, "cannot exec %s", RCSCRIPT);
				cleanexit(1);
				/*NOTREACHED*/
				break;
			}


		default:	/* Parent */
				/*
				 * If "hook" is non-NULL, we want to queue
				 * it to run later (possibly now)
				 * So, we need a different discipline
				 * for managing such a process...
				 */
				/* We no longer need the "hook" parameter */
				HB_RSCMGMTPROC(pid, "notify world");

#if WAITFORCOMMANDS
				waitpid(pid, &status, 0);
#else
				(void)status;
#endif
	}
}


/*
 * Node 'hip' has died.  Take over its resources (if any)
 * This may mean we have to STONITH them.
 */

void
hb_rsc_recover_dead_resources(struct node_info* hip)
{
	gboolean	need_stonith = TRUE;
	standby_running = zero_longclock;

	if (hip->nodetype == PINGNODE) {
		takeover_from_node(hip->nodename);
		return;
	}

	/*
	 * If we haven't heard anything from them - they might be holding
	 * resources - we have no way of knowing.
	 */
	if (hip->anypacketsyet) {
		if (!hip->has_resources
		||	(nice_failback && other_holds_resources == HB_NO_RSC)){
			need_stonith = FALSE;
		}
	}

	if (need_stonith) {
		/* We have to Zap them before we take the resources */
		/* This often takes a few seconds. */
		if (config->stonith) {
			Initiate_Reset(config->stonith, hip->nodename);
			/* It will call takeover_from_node() later */
			return;
		}else{
			ha_log(LOG_WARNING, "No STONITH device configured.");
			ha_log(LOG_WARNING, "Shared disks are not protected.");
		}
	}else{
		ha_log(LOG_INFO, "Dead node %s held no resources."
		,	hip->nodename);
	}
	/* nice_failback needs us to do this anyway... */
	takeover_from_node(hip->nodename);
}

/*
 * Here starts the nice_failback thing. The main purpouse of
 * nice_failback is to create a controlled failback. This
 * means that when the primary comes back from an outage it
 * stays quiet and acts as a secondary/backup server.
 * There are some more comments about it in nice_failback.txt
 */

/*
 * At this point nice failback deals with two nodes and is
 * an interim measure. The new version using the API is coming soon!
 *
 * This piece of code treats five different situations:
 *
 * 1. Node1 is starting and Node2 is down (or vice-versa)
 *    Take the resources. req_our_resources(), mark_node_dead()
 *
 * 2. Node1 and Node2 are starting at the same time
 *    Let both machines req_our_resources().
 *
 * 3. Node1 is starting and Node2 holds no resources
 *    Just like #2
 *
 * 4. Node1 is starting and Node2 has (his) local resources
 *    Let's ask for our local resources. req_our_resources()
 *
 * 5. Node1 is starting and Node2 has both local and foreign
 *	resources (all resources)
 *    Do nothing :)
 *
 */
/*
 * About the nice_failback resource takeover model:
 *
 * There are two principles that seem to guarantee safety:
 *
 *      1) Take all unclaimed resources if the other side is stable.
 *	      [Once you do this, you are also stable].
 *
 *      2) Take only unclaimed local resources when a timer elapses
 *		without things becoming stable by (1) above.
 *	      [Once this occurs, you're stable].
 *
 * Stable means that we have taken the resources we think we ought to, and
 * won't take any more without another transition ocurring.
 *
 * The other side is stable whenever it says it is (in its RESOURCE
 * message), or if it is dead.
 *
 * The nice thing about the stable bit in the resources message is that it
 * enables you to tell if the other side is still messing around, or if
 * they think they're done messing around.  If they're done, then it's safe
 * to proceed.  If they're not, then you need to wait until they say
 * they're done, or until a timeout occurs (because no one has become stable).
 *
 * When the timeout occurs, you're both deadlocked each waiting for the
 * other to become stable.  Then it's safe to take your local resources
 * (unless, of course, for some unknown reason, the other side has taken
 * them already).
 *
 * If a node dies die, then they'll be marked dead, and its resources will
 * be marked unclaimed.  In this case, you'll take over everything - whether
 * local resources through mark_node_dead() or remote resources through
 * mach_down.
 */

#define	HB_UPD_RSC(cur, up)	((up == HB_NO_RSC) ? HB_NO_RSC : ((up)|(cur)))

void
process_resources(const char * type, struct ha_msg* msg, struct node_info * thisnode)
{
	static int		resources_requested_yet = 0;

	enum hb_rsc_state	newrstate = resourcestate;
	static int			first_time = 1;

	if (!DoManageResources) {
		return;
	}

	if (!nice_failback) {
		/* Original ("normal") starting behavior */
		if (!WeAreRestarting && !resources_requested_yet) {
			resources_requested_yet=1;
			req_our_resources(0);
		}
		return;
	}

	/* Otherwise, we're in the nice_failback case */

	/* This first_time switch might still be buggy -- FIXME */

	if (first_time && WeAreRestarting) {
		resourcestate = newrstate = HB_R_STABLE;
	}


	/*
	 * Deal with T_STARTING messages coming from the other side.
	 *
	 * These messages are a request for resource usage information.
	 * The appropriate reply is a T_RESOURCES message.
	 */

	 if (strcasecmp(type, T_STARTING) == 0 && (thisnode != curnode)) {

		switch(resourcestate) {

		case HB_R_RSCRCVD:
		case HB_R_STABLE:
		case HB_R_SHUTDOWN:
			break;
		case HB_R_STARTING:
			newrstate = HB_R_BOTHSTARTING;
			/* ??? req_our_resources(); ??? */
			break;

		default:
			ha_log(LOG_ERR, "Received '%s' message in state %d"
			,	T_STARTING, resourcestate);
			return;

		}
		other_is_stable = 0;
		if (ANYDEBUG) {
			ha_log(LOG_DEBUG
			, "process_resources: other now unstable");
		}
		if (takeover_in_progress) {
			ha_log(LOG_WARNING
			,	"T_STARTING received during takeover.");
		}
		hb_send_resources_held(rsc_msg[procinfo->i_hold_resources]
		,	resourcestate == HB_R_STABLE, NULL);
	}

	/* Manage resource related messages... */

	if (strcasecmp(type, T_RESOURCES) == 0) {
		const char *p;
		int n;
		/*
		 * There are four possible resource answers:
		 *
		 * "I don't hold any resources"			HB_NO_RSC
		 * "I hold only LOCAL resources"		HB_LOCAL_RSC
		 * "I hold only FOREIGN resources"		HB_FOREIGN_RSC
		 * "I hold ALL resources" (local+foreign)	HB_ALL_RSC
		 */

		p=ha_msg_value(msg, F_RESOURCES);
		if (p == NULL) {
			ha_log(LOG_ERR
			,	T_RESOURCES " message without " F_RESOURCES
			" field.");
			return;
		}

		switch (resourcestate) {

		case HB_R_BOTHSTARTING:
		case HB_R_STARTING:	newrstate = HB_R_RSCRCVD;
		case HB_R_RSCRCVD:
		case HB_R_STABLE:
		case HB_R_SHUTDOWN:
					break;

		default:		ha_log(LOG_ERR,	T_RESOURCES
					" message received in state %d"
					,	resourcestate);
					return;
		}

		n = encode_resources(p);

		if (thisnode != curnode) {
			/*
			 * This T_RESOURCES message is from the other side.
			 */

			const char *	f_stable;

			/* f_stable is NULL when msg from takeover script */
			if ((f_stable = ha_msg_value(msg, F_ISSTABLE)) != NULL){
				if (strcmp(f_stable, "1") == 0) {
					if (!other_is_stable) {
						ha_log(LOG_INFO
						,	"remote resource"
						" transition completed.");
						other_is_stable = 1;
					}
				}else{
					other_is_stable = 0;
					if (ANYDEBUG) {
						ha_log(LOG_DEBUG
						, "process_resources(2): %s"
						, " other now unstable");
					}
				}
			}

			other_holds_resources
			=	HB_UPD_RSC(other_holds_resources,n);

			if ((resourcestate != HB_R_STABLE
			&&   resourcestate != HB_R_SHUTDOWN)
			&&	other_is_stable) {
				ha_log(LOG_INFO
				,	"remote resource transition completed."
				);
				req_our_resources(0);
				newrstate = HB_R_STABLE;
				hb_send_resources_held
				(	rsc_msg[procinfo->i_hold_resources]
				,	1, NULL);
			}
		}else{
			const char * comment = ha_msg_value(msg, F_COMMENT);

			/*
			 * This T_RESOURCES message is from us.  It might be
			 * from the "mach_down" script or our own response to
			 * the other side's T_STARTING message.  The mach_down
			 * script sets the info (F_COMMENT) field to "mach_down"
			 * We set it to "shutdown" in giveup_resources().
			 *
			 * We do this so the audits work cleanly AND we can
			 * avoid a potential race condition.
			 *
			 * Also, we could now time how long a takeover is
			 * taking to occur, and complain if it takes "too long"
			 * 	[ whatever *that* means ]
			 */
				/* Probably unnecessary */
			procinfo->i_hold_resources
			=	HB_UPD_RSC(procinfo->i_hold_resources, n);

			if (comment) {
				if (strcmp(comment, "mach_down") == 0) {
					ha_log(LOG_INFO
					,	"mach_down takeover complete.");
					takeover_in_progress = 0;
					/* FYI: This also got noted earlier */
					procinfo->i_hold_resources
					|=	HB_FOREIGN_RSC;
					other_is_stable = 1;
					if (ANYDEBUG) {
						ha_log(LOG_DEBUG
						, "process_resources(3): %s"
						, " other now stable");
					}
				}else if (strcmp(comment, "shutdown") == 0) {
					resourcestate = newrstate = HB_R_SHUTDOWN;
				}
			}
		}
	}
	if (strcasecmp(type, T_SHUTDONE) == 0) {
		if (thisnode != curnode) {
			other_is_stable = 0;
			other_holds_resources = HB_NO_RSC;
			if (ANYDEBUG) {
				ha_log(LOG_DEBUG
				, "process_resources(4): %s"
				, " other now stable - T_SHUTDONE");
			}
		}else{
			resourcestate = newrstate = HB_R_SHUTDOWN;
			procinfo->i_hold_resources = 0;
		}
	}

	if (resourcestate != newrstate) {
		if (ANYDEBUG) {
			ha_log(LOG_INFO
			,	"STATE %d => %d", resourcestate, newrstate);
		}
	}

	resourcestate = newrstate;

	if (resourcestate == HB_R_RSCRCVD && local_takeover_time == 0L) {
		local_takeover_time =	add_longclock(time_longclock()
		,	secsto_longclock(RQSTDELAY));
	}

	AuditResources();
}

void
AuditResources(void)
{
	if (!nice_failback) {
		return;
	}

	/*******************************************************
	 *	Look for for duplicated or orphaned resources
	 *******************************************************/

	/*
	 *	Do both nodes own our local resources?
	 */

	if ((procinfo->i_hold_resources & HB_LOCAL_RSC) != 0
	&&	(other_holds_resources & HB_FOREIGN_RSC) != 0) {
		ha_log(LOG_ERR, "Both machines own our resources!");
	}

	/*
	 *	Do both nodes own foreign resources?
	 */

	if ((other_holds_resources & HB_LOCAL_RSC) != 0
	&&	(procinfo->i_hold_resources & HB_FOREIGN_RSC) != 0) {
		ha_log(LOG_ERR, "Both machines own foreign resources!");
	}

	/*
	 *	If things are stable, look for orphaned resources...
	 */

	if (resourcestate == HB_R_STABLE && other_is_stable
	&&	!shutdown_in_progress) {
		/*
		 *	Does someone own local resources?
		 */

		if ((procinfo->i_hold_resources & HB_LOCAL_RSC) == 0
		&&	(other_holds_resources & HB_FOREIGN_RSC) == 0) {
			ha_log(LOG_ERR, "No one owns our local resources!");
		}

		/*
		 *	Does someone own foreign resources?
		 */

		if ((other_holds_resources & HB_LOCAL_RSC) == 0
		&&	(procinfo->i_hold_resources & HB_FOREIGN_RSC) == 0) {
			ha_log(LOG_ERR, "No one owns foreign resources!");
		}
	}
}

const char *
decode_resources(int i)
{

	return (i < 0 || i >= DIMOF(rsc_msg))?  "huh?" : rsc_msg[i];
}
int
encode_resources(const char *p)
{
	int i;

	for (i=0; i < DIMOF(rsc_msg); i++) {
		if (strcmp(rsc_msg[i], p) == 0) {
			return i;
			break;
		}
	}
	ha_log(LOG_ERR, "encode_resources: bad resource type [%s]", p);
	return 0;
}


/* Send the "I hold resources" or "I don't hold" resource messages */
int
hb_send_resources_held(const char *str, int stable, const char * comment)
{
	struct ha_msg * m;
	int		rc = HA_OK;
	char		timestamp[16];

	if (!nice_failback) {
		return HA_OK;
	}
	sprintf(timestamp, TIME_X, (TIME_T) time(NULL));

	if (ANYDEBUG) {
		ha_log(LOG_DEBUG
		,	"Sending hold resources msg: %s, stable=%d # %s"
		,	str, stable, (comment ? comment : "<none>"));
	}
	if ((m=ha_msg_new(0)) == NULL) {
		ha_log(LOG_ERR, "Cannot send local starting msg");
		return(HA_FAIL);
	}
	if ((ha_msg_add(m, F_TYPE, T_RESOURCES) != HA_OK)
	||  (ha_msg_add(m, F_RESOURCES, str) != HA_OK)
	||  (ha_msg_add(m, F_ISSTABLE, (stable ? "1" : "0")) != HA_OK)) {
		ha_log(LOG_ERR, "hb_send_resources_held: Cannot create local msg");
		rc = HA_FAIL;
	}else if (comment) {
		rc = ha_msg_add(m, F_COMMENT, comment);
	}
	if (rc == HA_OK) {
		rc = send_cluster_msg(m);
	}

	ha_msg_del(m);
	return(rc);
}


/* Send the starting msg out to the cluster */
int
send_local_starting(void)
{
	struct ha_msg * m;
	int		rc;

	if (ANYDEBUG) {
		ha_log(LOG_DEBUG
		,	"Sending local starting msg: resourcestate = %d"
		,	resourcestate);
	}
	if ((m=ha_msg_new(0)) == NULL) {
		ha_log(LOG_ERR, "Cannot send local starting msg");
		return(HA_FAIL);
	}
	if ((ha_msg_add(m, F_TYPE, T_STARTING) != HA_OK)) {
		ha_log(LOG_ERR, "send_local_starting: "
		"Cannot create local starting msg");
		rc = HA_FAIL;
	}else{
		rc = send_cluster_msg(m);
	}

	ha_msg_del(m);
	resourcestate = HB_R_STARTING;
	return(rc);
}
/* We take all resources over from a given node */
void
takeover_from_node(const char * nodename)
{
	struct node_info *	hip = lookup_node(nodename);
	struct ha_msg *	hmsg;
	char		timestamp[16];

	if (hip == 0) {
		return;
	}
	if (shutdown_in_progress) {
		ha_log(LOG_INFO
		,	"Resource takeover cancelled - shutdown in progress.");
		return;
	}else if (hip->nodetype != PINGNODE) {
		ha_log(LOG_INFO
		,	"Resources being acquired from %s."
		,	hip->nodename);
	}
	if ((hmsg = ha_msg_new(6)) == NULL) {
		ha_log(LOG_ERR, "no memory to takeover_from_node");
		return;
	}

	sprintf(timestamp, TIME_X, (TIME_T) time(NULL));

	if (	ha_msg_add(hmsg, F_TYPE, T_STATUS) != HA_OK
	||	ha_msg_add(hmsg, F_SEQ, "1") != HA_OK
	||	ha_msg_add(hmsg, F_TIME, timestamp) != HA_OK
	||	ha_msg_add(hmsg, F_ORIG, hip->nodename) != HA_OK
	||	ha_msg_add(hmsg, F_STATUS, DEADSTATUS) != HA_OK) {
		ha_log(LOG_ERR, "no memory to takeover_from_node");
		ha_msg_del(hmsg);
		return;
	}

	if (hip->nodetype == PINGNODE) {
		if (ha_msg_add(hmsg, F_COMMENT, "ping") != HA_OK) {
			ha_log(LOG_ERR, "no memory to mark ping node dead");
			ha_msg_del(hmsg);
			return;
		}
	}

	/* Sending this message triggers the "mach_down" script */

	heartbeat_monitor(hmsg, KEEPIT, "<internal>");
	notify_world(hmsg, hip->status);

	/*
	 * STONITH has already successfully completed, or wasn't needed...
	 */
	if (nice_failback && hip->nodetype != PINGNODE) {

		/* mach_down is out there acquiring foreign resources */
		/* So, make a note of it... */
		procinfo->i_hold_resources |= HB_FOREIGN_RSC;

		other_holds_resources = HB_NO_RSC;
		other_is_stable = 1;	/* Not going anywhere */
		takeover_in_progress = 1;
		if (ANYDEBUG) {
			ha_log(LOG_DEBUG
			,	"mark_node_dead: other now stable");
		}
		/*
		 * We MUST do this now, or the other side might come
		 * back up and think they can own their own resources
		 * when we do due to receiving an interim
		 * T_RESOURCE message from us.
		 */
		/* case 1 - part 1 */
		/* part 2 is done by the mach_down script... */
		req_our_resources(0);
		/* req_our_resources turns on the HB_LOCAL_RSC bit */

	}
	hip->anypacketsyet = 1;
	ha_msg_del(hmsg);
}

void
req_our_resources(int getthemanyway)
{
	FILE *	rkeys;
	char	cmd[MAXLINE];
	char	getcmd[MAXLINE];
	char	buf[MAXLINE];
	int	finalrc = HA_OK;
	int	rc;
	int	rsc_count = 0;
	int	pid;
	int	upcount;

	if (nice_failback) {

		if (((other_holds_resources & HB_FOREIGN_RSC) != 0
		||	(procinfo->i_hold_resources & HB_LOCAL_RSC) != 0)
		&&	!getthemanyway) {

			if (going_standby == NOT) {
				/* Someone already owns our resources */
				ha_log(LOG_INFO
				,   "Local Resource acquisition completed"
				". (none)");
				return;
			}
		}

		/*
		 * We MUST do this now, or the other side might think they
		 * can have our resources, due to an interim T_RESOURCE
		 * message
		 */
		procinfo->i_hold_resources |= HB_LOCAL_RSC;
	}

	/* We need to fork so we can make child procs not real time */
	switch(pid=fork()) {

		case -1:	ha_log(LOG_ERR, "Cannot fork.");
				return;
		default:
				HB_RSCMGMTPROC(pid, "req_our_resources");
				return;

		case 0:		/* Child */
				break;
	}

	cl_make_normaltime();
	set_proc_title("%s: req_our_resources()", cmdname);
	setpgid(0,0);
	CL_SIGNAL(SIGCHLD, SIG_DFL);
	alarm(0);
	CL_IGNORE_SIG(SIGALRM);
	CL_SIGINTERRUPT(SIGALRM, 0);
	if (nice_failback) {
		setenv(HANICEFAILBACK, "yes", 1);
	}
	upcount = countbystatus(ACTIVESTATUS, TRUE);

	/* Our status update is often not done yet */
	if (strcmp(curnode->status, ACTIVESTATUS) != 0) {
		upcount++;
	}
 
	/* Are we all alone in the world? */
	if (upcount < 2) {
		setenv(HADONTASK, "yes", 1);
	}
	sprintf(cmd, HALIB "/ResourceManager listkeys %s", curnode->nodename);

	if ((rkeys = popen(cmd, "r")) == NULL) {
		ha_log(LOG_ERR, "Cannot run command %s", cmd);
		exit(1);
	}


	for (;;) {
		errno = 0;
		if (fgets(buf, MAXLINE, rkeys) == NULL) {
			if (ferror(rkeys)) {
				ha_perror("req_our_resources: fgets failure");
			}
			break;
		}
		++rsc_count;

		if (buf[strlen(buf)-1] == '\n') {
			buf[strlen(buf)-1] = EOS;
		}
		sprintf(getcmd, HALIB "/req_resource %s", buf);
		if ((rc=system(getcmd)) != 0) {
			ha_perror("%s returned %d", getcmd, rc);
			finalrc=HA_FAIL;
		}
	}
	rc=pclose(rkeys);
	if (rc < 0 && errno != ECHILD) {
		ha_perror("pclose(%s) returned %d", cmd, rc);
	}else if (rc > 0) {
		ha_log(LOG_ERR, "[%s] exited with 0x%x", cmd, rc);
	}

	if (rsc_count == 0) {
		ha_log(LOG_INFO, "No local resources [%s]", cmd);
	}else{
		if (ANYDEBUG) {
			ha_log(LOG_INFO, "%d local resources from [%s]"
			,	rsc_count, cmd);
		}
	}
	hb_send_resources_held(rsc_msg[procinfo->i_hold_resources], 1
	,	"req_our_resources()");
	ha_log(LOG_INFO, "Resource acquisition completed.");
	exit(0);
}

/* Send "standby" related msgs out to the cluster */
static int
send_standby_msg(enum standby state)
{
	const char * standby_msg[] = { "not", "me", "other", "done"};
	struct ha_msg * m;
	int		rc;
	char		timestamp[16];

	sprintf(timestamp, TIME_X, (TIME_T) time(NULL));

	if (ANYDEBUG) {
		ha_log(LOG_DEBUG, "Sending standby [%s] msg"
		,			standby_msg[state]);
	}
	if ((m=ha_msg_new(0)) == NULL) {
		ha_log(LOG_ERR, "Cannot send standby [%s] msg"
		,			standby_msg[state]);
		return(HA_FAIL);
	}
	if ((ha_msg_add(m, F_TYPE, T_ASKRESOURCES) != HA_OK)
	||  (ha_msg_add(m, F_COMMENT, standby_msg[state]) != HA_OK)) {
		ha_log(LOG_ERR, "send_standby_msg: "
		"Cannot create standby reply msg");
		rc = HA_FAIL;
	}else{
		rc = send_cluster_msg(m);
	}

	ha_msg_del(m);
	return(rc);
}

#define	STANDBY_INIT_TO_MS	10000L		/* ms timeout for initial reply */
#define	HB_STANDBY_RSC_TO_MS	1200000L	/* resource handling timeout (ms)*/

void
ask_for_resources(struct ha_msg *msg)
{

	const char *	info;
	const char *	from;
	int 		msgfromme;
	longclock_t 	now = time_longclock();
	int		message_ignored = 0;
	const enum standby	orig_standby = going_standby;
	longclock_t	standby_rsc_to = msto_longclock(HB_STANDBY_RSC_TO_MS);

	if (!nice_failback) {
		ha_log(LOG_INFO
		,	"Standby mode only implemented when nice_failback on");
		return;
	}
	info = ha_msg_value(msg, F_COMMENT);
	from = ha_msg_value(msg, F_ORIG);

	if (info == NULL || from == NULL) {
		ha_log(LOG_ERR, "Received standby message without info/from");
		return;
	}
	msgfromme = strcmp(from, curnode->nodename) == 0;

	if (ANYDEBUG){
		ha_log(LOG_DEBUG
		,	"Received standby message %s from %s in state %d "
		,	info, from, going_standby);
	}

	if (cmp_longclock(standby_running, zero_longclock) != 0
	&&	cmp_longclock(now, standby_running) < 0
	&&	strcasecmp(info, "me") == 0) {
		unsigned long	secs_left;

		secs_left = longclockto_ms(sub_longclock(standby_running, now));

		secs_left = (secs_left+999)/1000;

		ha_log(LOG_WARNING
		,	"Standby in progress"
		"- new request from %s ignored [%ld seconds left]"
		,	from, secs_left);
		return;
	}

	/* Starting the STANDBY 3-phased protocol */

	switch(going_standby) {
	case NOT:
		if (!other_is_stable) {
			ha_log(LOG_WARNING, "standby message [%s] from %s"
			" ignored.  Other side is in flux.", info, from);
			return;
		}
		if (resourcestate != HB_R_STABLE) {
			ha_log(LOG_WARNING, "standby message [%s] from %s"
			" ignored.  local resources in flux.", info, from);
			return;
		}
		if (strcasecmp(info, "me") == 0) {
			longclock_t	init_to = msto_longclock(STANDBY_INIT_TO_MS);
			standby_running = add_longclock(now, init_to);

			if (ANYDEBUG) {
				ha_log(LOG_DEBUG
				, "ask_for_resources: other now unstable");
			}
			other_is_stable = 0;
			ha_log(LOG_INFO, "%s wants to go standby", from);
			if (msgfromme) {
				/* We want to go standby */
				if (ANYDEBUG) {
					ha_log(LOG_INFO
					,	"i_hold_resources: %d"
					,	procinfo->i_hold_resources);
				}
				going_standby = ME;
			}else{
				if (ANYDEBUG) {
					ha_log(LOG_INFO
					,	"other_holds_resources: %d"
					,	other_holds_resources);
				}
				/* Other node wants to go standby */
				going_standby = OTHER;
				send_standby_msg(going_standby);
			}
		}else{
			message_ignored = 1;
		}
		break;

	case ME:
		/* Other node is alive, so give up our resources */
		if (!msgfromme) {
			standby_running = add_longclock(now, standby_rsc_to);
			if (strcasecmp(info,"other") == 0) {
				ha_log(LOG_INFO
				,	"standby: %s can take our resources"
				,	from);
				go_standby(ME);
				/* Our child proc sends a "done" message */
				/* after all the resources are released	*/
			}else{
				message_ignored = 1;
			}
		}else if (strcasecmp(info, "done") == 0) {
			/*
			 * The "done" message came from our child process
			 * indicating resources are completely released now.
			 */
			ha_log(LOG_INFO
			,	"Standby process finished. /Me secondary");
			going_standby = DONE;
			procinfo->i_hold_resources = HB_NO_RSC;
			standby_running = add_longclock(now, standby_rsc_to);
		}else{
			message_ignored = 1;
		}
		break;
	case OTHER:
		if (strcasecmp(info, "done") == 0) {
			standby_running = add_longclock(now, standby_rsc_to);
			if (!msgfromme) {
				/* It's time to acquire resources */

				ha_log(LOG_INFO
				,	"standby: Acquire [%s] resources"
				,	from);
				/* go_standby gets *all* resources */
				/* req_our_resources(1); */
				go_standby(OTHER);
				going_standby = DONE;
			}else{
				message_ignored = 1;
			}
		}else if (!msgfromme || strcasecmp(info, "other") != 0) {
			/* We expect an "other" message from us */
			/* But, that's not what this one is ;-) */
			message_ignored = 1;
		}
		break;

	case DONE:
		if (strcmp(info, "done")== 0) {
			standby_running = zero_longclock;
			going_standby = NOT;
			if (msgfromme) {
				ha_log(LOG_INFO
				,	"Standby process done. /Me primary");
				procinfo->i_hold_resources = HB_ALL_RSC;
			}else{
				ha_log(LOG_INFO
				,	"Other node completed standby"
				" takeover.");
			}
			hb_send_resources_held(rsc_msg[procinfo->i_hold_resources], 1, NULL);
			going_standby = NOT;
		}else{
			message_ignored = 1;
		}
		break;
	}
	if (message_ignored){
		ha_log(LOG_ERR
		,	"Ignored standby message '%s' from %s in state %d"
		,	info, from, orig_standby);
	}
	if (ANYDEBUG) {
		ha_log(LOG_INFO, "New standby state: %d", going_standby);
	}
}

static int
countbystatus(const char * status, int matchornot)
{
	int	count = 0;
	int	matches;
	int	j;

	matchornot = (matchornot ? TRUE : FALSE);

	for (j=0; j < config->nodecount; ++j) {
		matches = (strcmp(config->nodes[j].status, status) == 0);
		if (matches == matchornot) {
			++count;
		}
	}
	return count;
}




static void
go_standby(enum standby who)
{
	FILE *		rkeys;
	char		cmd[MAXLINE];
	char		buf[MAXLINE];
	int		finalrc = HA_OK;
	int		rc = 0;
	pid_t		pid;

	/*
	 * We consider them unstable because they're about to pick up
	 * our resources.
	 */
	if (who == ME) {
		other_is_stable = 0;
		if (ANYDEBUG) {
			ha_log(LOG_DEBUG, "go_standby: other is unstable");
		}
	}
	/* We need to fork so we can make child procs not real time */

	switch((pid=fork())) {

		case -1:	ha_log(LOG_ERR, "Cannot fork.");
				return;

				/*
				 * We shouldn't block here, because then we
				 * aren't sending heartbeats out...
				 */
		default:	
				if (who == ME) {
					HB_RSCMGMTPROC(pid, "go_standby");
				}else{
					HB_RSCMGMTPROC(pid, "go_standby");
				}
				/* waitpid(pid, NULL, 0); */
				return;

		case 0:		/* Child */
				break;
	}

	cl_make_normaltime();
	setpgid(0,0);
	CL_SIGNAL(SIGCHLD, SIG_DFL);

	if (who == ME) {
		procinfo->i_hold_resources = HB_NO_RSC;
		/* Make sure they know what we're doing and that we're
		 * not done yet (not stable)
		 * Since heartbeat doesn't guarantee message ordering
		 * this could theoretically have problems, but all that
		 * happens if it gets out of order is that we get
		 * a funky warning message (or maybe two).
		 */
		hb_send_resources_held(rsc_msg[procinfo->i_hold_resources], 0, "standby");
	}
	/*
	 *	We could do this ourselves fairly easily...
	 */

	sprintf(cmd, HALIB "/ResourceManager listkeys '.*'");

	if ((rkeys = popen(cmd, "r")) == NULL) {
		ha_log(LOG_ERR, "Cannot run command %s", cmd);
		return;
	}
	ha_log(LOG_INFO
	,	"%s all HA resources (standby)."
	,	who == ME ? "Giving up" : "Acquiring");

	while (fgets(buf, MAXLINE, rkeys) != NULL) {
		if (buf[strlen(buf)-1] == '\n') {
			buf[strlen(buf)-1] = EOS;
		}
		if (who == ME) {
			sprintf(cmd, HALIB "/ResourceManager givegroup %s",buf);
		}else{
			if (who == OTHER) {
				sprintf(cmd, HALIB
					"/ResourceManager takegroup %s", buf);
			}
		}
		if ((rc=system(cmd)) != 0) {
			ha_log(LOG_ERR, "%s returned %d", cmd, rc);
			finalrc=HA_FAIL;
		}
	}
	pclose(rkeys);
	if (ANYDEBUG) {
		ha_log(LOG_INFO, "go_standby: who: %d", who);
	}
	if (who == ME) {
		ha_log(LOG_INFO, "All HA resources relinquished (standby).");
	}else if (who == OTHER) {
		procinfo->i_hold_resources |= HB_FOREIGN_RSC;
		ha_log(LOG_INFO, "All resources acquired (standby).");
	}
	send_standby_msg(DONE);
	exit(rc);

}

void
hb_giveup_resources(void)
{
	FILE *		rkeys;
	char		cmd[MAXLINE];
	char		buf[MAXLINE];
	int		finalrc = HA_OK;
	int		rc;
	pid_t		pid;
	struct ha_msg *	m;


	if (shutdown_in_progress) {
		ha_log(LOG_INFO, "Heartbeat shutdown already underway.");
		return;
	}
	if (ANYDEBUG) {
		ha_log(LOG_INFO, "hb_signal_giveup_resources(): "
			"current status: %s", curnode->status);
	}
	shutdown_in_progress =1;
	hb_close_watchdog();
	DisableProcLogging();	/* We're shutting down */
	/* Kill all our managed children... */
	ForEachProc(&ManagedChildTrackOps, hb_kill_tracked_process
	,	GINT_TO_POINTER(SIGTERM));
	ForEachProc(&hb_rsc_RscMgmtProcessTrackOps, hb_kill_tracked_process
	,	GINT_TO_POINTER(SIGKILL));
	procinfo->i_hold_resources = HB_NO_RSC ;
	resourcestate = HB_R_SHUTDOWN; /* or we'll get a whiny little comment
				out of the resource management code */
	if (nice_failback) {
		hb_send_resources_held(decode_resources(procinfo->i_hold_resources)
		,	0, "shutdown");
	}
	ha_log(LOG_INFO, "Heartbeat shutdown in progress. (%d)"
	,	(int) getpid());

	/* We need to fork so we can make child procs not real time */

	switch((pid=fork())) {

		case -1:	ha_log(LOG_ERR, "Cannot fork.");
				return;

		default:
				HB_RSCMGMTPROC(pid
				,	"hb_signal_giveup_resources");
				return;

		case 0:		/* Child */
				break;
	}

	cl_make_normaltime();
	setpgid(0,0);
	set_proc_title("%s: hb_signal_giveup_resources()", cmdname);

	/* We don't want to be interrupted while shutting down */

	CL_SIGNAL(SIGCHLD, SIG_DFL);
	CL_SIGINTERRUPT(SIGCHLD, 0);

	alarm(0);
	CL_IGNORE_SIG(SIGALRM);
	CL_SIGINTERRUPT(SIGALRM, 0);

	CL_IGNORE_SIG(SIGTERM);
	/* CL_SIGINTERRUPT(SIGTERM, 0); */

	ha_log(LOG_INFO, "Giving up all HA resources.");
	/*
	 *	We could do this ourselves fairly easily...
	 */

	sprintf(cmd, HALIB "/ResourceManager listkeys '.*'");

	if ((rkeys = popen(cmd, "r")) == NULL) {
		ha_log(LOG_ERR, "Cannot run command %s", cmd);
		exit(1);
	}

	while (fgets(buf, MAXLINE, rkeys) != NULL) {
		if (buf[strlen(buf)-1] == '\n') {
			buf[strlen(buf)-1] = EOS;
		}
		sprintf(cmd, HALIB "/ResourceManager givegroup %s", buf);
		if ((rc=system(cmd)) != 0) {
			ha_log(LOG_ERR, "%s returned %d", cmd, rc);
			finalrc=HA_FAIL;
		}
	}
	pclose(rkeys);
	ha_log(LOG_INFO, "All HA resources relinquished.");

	if ((m=ha_msg_new(0)) == NULL) {
		ha_log(LOG_ERR, "Cannot send final shutdown msg");
		exit(1);
	}
	if ((ha_msg_add(m, F_TYPE, T_SHUTDONE) != HA_OK
	||	ha_msg_add(m, F_STATUS, DEADSTATUS) != HA_OK)) {
		ha_log(LOG_ERR, "hb_signal_giveup_resources: "
			"Cannot create local msg");
	}else{
		if (ANYDEBUG) {
			ha_log(LOG_DEBUG, "Sending T_SHUTDONE.");
		}
		rc = send_cluster_msg(m);
	}

	ha_msg_del(m);
	exit(0);
}

void
Initiate_Reset(Stonith* s, const char * nodename)
{
	const char*	result = "bad";
	struct ha_msg*	hmsg;
	int		pid;
	int		exitcode = 0;
	struct StonithProcHelper *	h;
	/*
	 * We need to fork because the stonith operations block for a long
	 * time (10 seconds in common cases)
	 */
	switch((pid=fork())) {

		case -1:	ha_log(LOG_ERR, "Cannot fork.");
				return;
		default:
				h = g_new(struct StonithProcHelper, 1);
				h->nodename = g_strdup(nodename);
				NewTrackedProc(pid, 1, PT_LOGVERBOSE, h
				,	&StonithProcessTrackOps);
				/* StonithProcessDied is called when done */
				return;

		case 0:		/* Child */
				break;

	}
	/* Guard against possibly hanging Stonith code... */
	cl_make_normaltime();
	setpgid(0,0);
	set_proc_title("%s: Initiate_Reset()", cmdname);
	CL_SIGNAL(SIGCHLD,SIG_DFL);

	ha_log(LOG_INFO
	,	"Resetting node %s with [%s]"
	,	nodename
	,	s->s_ops->getinfo(s, ST_DEVICEID));

	switch (s->s_ops->reset_req(s, ST_GENERIC_RESET, nodename)){

	case S_OK:
		result="OK";
		ha_log(LOG_INFO
		,	"node %s now reset.", nodename);
		exitcode = 0;
		break;

	case S_BADHOST:
		ha_log(LOG_ERR
		,	"Device %s cannot reset host %s."
		,	s->s_ops->getinfo(s, ST_DEVICEID)
		,	nodename);
		exitcode = 100;
		result = "badhost";
		break;

	default:
		ha_log(LOG_ERR, "Host %s not reset!", nodename);
		exitcode = 1;
		result = "bad";
	}

	if ((hmsg = ha_msg_new(6)) == NULL) {
		ha_log(LOG_ERR, "no memory for " T_REXMIT);
	}

	if (	hmsg != NULL
	&& 	ha_msg_add(hmsg, F_TYPE, T_STONITH)    == HA_OK
	&&	ha_msg_add(hmsg, F_NODE, nodename) == HA_OK
	&&	ha_msg_add(hmsg, F_APIRESULT, result) == HA_OK) {
		/* Send a Stonith message */
		if (send_cluster_msg(hmsg) != HA_OK) {
			ha_log(LOG_ERR, "cannot send " T_STONITH
			" request for %s", nodename);
		}
	}else{
		ha_log(LOG_ERR
		,	"Cannot send reset reply message [%s] for %s", result
		,	nodename);
	}
	exit (exitcode);
}


static void
RscMgmtProcessRegistered(ProcTrack* p)
{
	ResourceMgmt_child_count ++;
}
/* Handle the death of a resource management process */
static void
RscMgmtProcessDied(ProcTrack* p, int status, int signo, int exitcode
,	int waslogged)
{
	ResourceMgmt_child_count --;
	p->privatedata = NULL;
	StartNextRemoteRscReq();
}

static const char *
RscMgmtProcessName(ProcTrack* p)
{
	struct hb_const_string * s = p->privatedata;

	return (s && s->str ? s->str : "heartbeat resource child");
}

/***********************************************************************
 *
 * RemoteRscRequests are resource management requests from other nodes
 *
 * Our "privatedata" is a GHook.  This GHook points back to the
 * queue entry for this object. Its "data" element points to the message
 * which we want to give to the function which the hook points to...
 * QueueRemoteRscReq is the function which sets up the hook, then queues
 * it for later execution.
 *
 * StartNextRemoteRscReq() is the function which runs the hook,
 * when the time is right.  Basically, we won't run the hook if any
 * other asynchronous resource management operations are going on.
 * This solves the problem of a remote request coming in and conflicting
 * with a different local resource management request.  It delays
 * it until the local startup/takeover/etc. operations are complete.
 * At this time, it has a clear picture of what's going on, and
 * can safely do its thing.
 *
 * So, we queue the job to do in a Ghook.  When the Ghook runs, it
 * will create a ProcTrack object to track the completion of the process.
 *
 * When the process completes, it will clean up the ProcTrack, which in
 * turn will remove the GHook from the queue, destroying it and the
 * associated struct ha_msg* from the original message.
 *
 ***********************************************************************/

static GHookList	RemoteRscReqQueue = {0,0,0};
static GHook*		RunningRemoteRscReq = NULL;

/* Initialized the remote resource request queue */
static void
InitRemoteRscReqQueue(void)
{
	if (RemoteRscReqQueue.is_setup) {
		return;
	}
	g_hook_list_init(&RemoteRscReqQueue, sizeof(GHook));
}

/* Queue a remote resource request */
void
QueueRemoteRscReq(RemoteRscReqFunc func, struct ha_msg* msg)
{
	GHook*	hook;

	InitRemoteRscReqQueue();
	hook = g_hook_alloc(&RemoteRscReqQueue);

	if (ANYDEBUG) {
		ha_log(LOG_DEBUG
		,	"Queueing remote resource request (hook = 0x%p)"
		,	(void *)hook);
		ha_log_message(msg);
	}
	hook->func = func;
	hook->data = msg;
	hook->destroy = (GDestroyNotify)(ha_msg_del);
	g_hook_append(&RemoteRscReqQueue, hook);
	StartNextRemoteRscReq();
}

/* If the time is right, start the next remote resource request */
static void
StartNextRemoteRscReq(void)
{
	GHook*		hook;
	RemoteRscReqFunc	func;

	/* We can only run one of these at a time... */
	if (ResourceMgmt_child_count != 0) {
		return;
	}

	RunningRemoteRscReq = NULL;

	/* Run the first hook in the list... */

	hook = g_hook_first_valid(&RemoteRscReqQueue, FALSE);
	if (hook == NULL) {
		ResourceMgmt_child_count = 0;
		return;
	}

	RunningRemoteRscReq = hook;
	func = hook->func;

	if (ANYDEBUG) {
		ha_log(LOG_DEBUG, "StartNextRemoteRscReq() - calling hook");
	}
	/* Call the hook... */
	func(hook);
	g_hook_unref(&RemoteRscReqQueue, hook);
	g_hook_destroy_link(&RemoteRscReqQueue, hook);
}


/*
 * Perform a queued notify_world() call
 *
 * The Ghook and message are automatically destroyed by our
 * caller.
 */

void
PerformQueuedNotifyWorld(GHook* hook)
{
	struct ha_msg* m = hook->data;
	/*
	 * We have been asked to run a notify_world() which
	 * we would like to have done earlier...
	 */
	if (ANYDEBUG) {
		ha_log(LOG_DEBUG, "PerformQueuedNotifyWorld() msg follows");
		ha_log_message(m);
	}
	notify_world(m, curnode->status);
	/* "m" is automatically destroyed when "hook" is */
}

	

/* Handle the death of a STONITH process */
static void
StonithProcessDied(ProcTrack* p, int status, int signo, int exitcode, int waslogged)
{
	struct StonithProcHelper*	h = p->privatedata;

	if (signo != 0 || exitcode != 0) {
		ha_log(LOG_ERR, "STONITH of %s failed.  Retrying..."
		,	(const char*) p->privatedata);
		Initiate_Reset(config->stonith, h->nodename);
	}else{
		/* We need to finish taking over the other side's resources */
		takeover_from_node(h->nodename);
	}
	g_free(h->nodename);	h->nodename=NULL;
	g_free(p->privatedata);	p->privatedata = NULL;
}

static const char *
StonithProcessName(ProcTrack* p)
{
	static char buf[100];
	struct StonithProcHelper *	h = p->privatedata;
	snprintf(buf, sizeof(buf), "STONITH %s", h->nodename);
	return buf;
}

/*
 * $Log: hb_resource.c,v $
 * Revision 1.5  2002/11/08 15:49:39  alan
 * Fixed a bug in STONITH for the true cluster partition case.
 * When we came up, and didn't see the other node, we just took over
 * resources w/o STONITH.
 * Now we STONITH the node first, then take the data over.
 *
 * Revision 1.4  2002/10/30 17:17:40  alan
 * Added some debugging, and changed one message from an ERROR to a WARNING.
 *
 * Revision 1.3  2002/10/22 17:41:58  alan
 * Added some documentation about deadtime, etc.
 * Switched one of the sets of FIFOs to IPC channels.
 * Added msg_from_IPC to ha_msg.c make that easier.
 * Fixed a few compile errors that were introduced earlier.
 * Moved hb_api_core.h out of the global include directory,
 * and back into a local directory.  I also make sure it doesn't get
 * installed.  This *shouldn't* cause problems.
 * Added a ipc_waitin() function to the IPC code to allow you to wait for
 * input synchronously if you really want to.
 * Changes the STONITH test to default to enabled.
 *
 */
