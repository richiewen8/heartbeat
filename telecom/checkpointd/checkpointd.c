/* 
 * checkpointd.c: data checkpoint service
 *
 * Copyright (C) 2003 Deng Pan <deng.pan@intel.com>
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


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <glib.h>

#include <clplumbing/cl_log.h>
#include <clplumbing/cl_signal.h>
#include <clplumbing/ipc.h>
#include <clplumbing/Gmain_timeout.h>
#include <hb_api_core.h>
#include <hb_api.h>
#include <ha_msg.h>
#include <heartbeat.h>

#include <saf/ais.h>
#include <checkpointd/clientrequest.h>
#include "checkpointd.h"
#include "client.h"
#include "replica.h"
#include "message.h"
#include "request.h"
#include "response.h"
#include "operation.h"
#include "utils.h"

#ifdef USE_DMALLOC
#include <dmalloc.h>
#endif


SaCkptServiceT	*saCkptService = NULL;
GMainLoop	*mainloop = NULL;

static void usage(void);

static void SaCkptCheckpointdInit(void);

static gboolean SaCkptHbInputDispatch(int, gpointer);
static void SaCkptHbInputDestroy(gpointer);

static gboolean SaCkptClientChannelDispatch(IPC_Channel*, gpointer);
static void SaCkptClientChannelDestroy(gpointer);

static gboolean SaCkptWaitChannelDispatch(IPC_Channel *, gpointer);
static void SaCkptWaitChannelDestroy(gpointer);
static IPC_WaitConnection* SaCkptWaitChannelInit(char*);


static void
SaCkptDmalloc(int signum) {
	cl_log(LOG_INFO, "Receive signal SIGINT");
	
	g_main_quit(mainloop);
}

void
SaCkptCheckpointdInit(void)
{
	ll_cluster_t*	hb = NULL;
	const char* strNode = NULL;

	// set log options
	cl_log_set_entity("CKPT");
	if (saCkptService->flagDaemon) {
		cl_log_enable_stderr(FALSE);
	} else {
		cl_log_enable_stderr(TRUE);
	}
//	cl_log_set_facility(LOG_LOCAL1);

	cl_log(LOG_INFO, "=== Start checkpointd ===");
	
	hb = ll_cluster_new("heartbeat");

	// sign on with heartbeat
	if (hb->llc_ops->signon(hb, "checkpointd") != HA_OK) {
		cl_log(LOG_ERR, "Cannot sign on with heartbeat\n");
		cl_log(LOG_ERR, "REASON: %s\n", 
			hb->llc_ops->errmsg(hb));
		exit(1);
	}
	cl_log(LOG_INFO, "Sign on with heartbeat");

	strNode = hb->llc_ops->get_mynodeid(hb);
	cl_log(LOG_INFO, "Node id: %s", strNode);
	
	CL_SIGNAL(SIGINT, SaCkptDmalloc);

	saCkptService->heartbeat = hb;
	strcpy(saCkptService->nodeName, strNode);
	saCkptService->clientHash = 
		g_hash_table_new(g_int_hash, g_int_equal);
	saCkptService->replicaHash = 
		g_hash_table_new(g_str_hash, g_str_equal);
	saCkptService->openCheckpointHash = 
		g_hash_table_new(g_int_hash, g_int_equal);
	
	saCkptService->nextClientHandle = 0;
	saCkptService->nextCheckpointHandle = 0;
	
	saCkptService->version.major = '0';
	saCkptService->version.minor = '1';
	saCkptService->version.releaseCode = 'A';

	return;
}

void
usage()
{
	printf("checkpointd - data checkpoint service daemon\n");
	printf("Usage: checkpointd [options...]\n");
	printf("Options:\n");
	printf("\t--help, -h, -?\t\tshow this help\n");
	printf("\t--daemon, -d\trun in daemon mode\n");
	printf("\t--verbose, -v\trun in verbose mode\n");
}


static gboolean
SaCkptHbInputDispatch(int fd, gpointer user_data)
{
	SaCkptClusterMsgProcess();

	return TRUE;
}

static void
SaCkptHbInputDestroy(gpointer user_data)
{
	return;
}

static gboolean
SaCkptClientChannelDispatch(IPC_Channel *clientChannel, gpointer user_data)
{
	 return SaCkptRequestProcess(clientChannel);
}

static void
SaCkptClientChannelDestroy(gpointer user_data)
{
//	cl_log(LOG_INFO, "SaCkptClientChannelDestroy:received HUP");
	return;
}

static gboolean
SaCkptWaitChannelDispatch(IPC_Channel *newclient, gpointer user_data)
{
	G_main_add_IPC_Channel(G_PRIORITY_LOW, newclient, FALSE, 
				SaCkptClientChannelDispatch, newclient, 
				SaCkptClientChannelDestroy);
	return TRUE;
}

static void
SaCkptWaitChannelDestroy(gpointer user_data)
{
	IPC_WaitConnection *waitChannel = 
			(IPC_WaitConnection *)user_data;

	waitChannel->ops->destroy(waitChannel);
	return;
}

static IPC_WaitConnection *
SaCkptWaitChannelInit(char* pathname)
{
	IPC_WaitConnection *waitConnection = NULL;
	mode_t mask;
	char path[] = IPC_PATH_ATTR;
	char domainsocket[] = IPC_DOMAIN_SOCKET;

	GHashTable *attrs = g_hash_table_new(g_str_hash,g_str_equal);

	g_hash_table_insert(attrs, path, pathname);

	mask = umask(0);
	waitConnection = ipc_wait_conn_constructor(domainsocket, attrs);
	if (waitConnection == NULL){
		cl_perror("Can't create wait connection");
		exit(1);
	}
	mask = umask(mask);

	g_hash_table_destroy(attrs);

	return waitConnection;
}

#if 0
/* begin :added by steve*/
static gboolean SaCkptClientDebugDispatch(IPC_Channel*, gpointer);
static void SaCkptClientDebugDestroy(gpointer);

static gboolean
SaCkptClientDebugDispatch(IPC_Channel *clientChannel, gpointer user_data)
{
	return SaCkptDebugProcess(clientChannel);
}

static void
SaCkptClientDebugDestroy(gpointer user_data)
{
	cl_log(LOG_INFO, "clntDebug_input_destroy:received HUP");
	return;
}

static gboolean SaCkptWaitDebugdispatch(IPC_Channel *, gpointer);
static void SaCkptWaitDebugDestroy(gpointer);

static gboolean
SaCkptWaitDebugDispatch(IPC_Channel *newclient, gpointer user_data)
{
//	client_add(newclient);

	G_main_add_IPC_Channel(G_PRIORITY_LOW, newclient, FALSE, 
				SaCkptClientDebugDispatch, newclient, 
				SaCkptClientDebuugDestroy);
	return TRUE;
}

static void
SaWaitDebugDestroy(gpointer user_data)
{
	IPC_WaitConnection *wait_debug = 
			(IPC_WaitConnection *)user_data;

	wait_debug->ops->destroy(wait_debug);
	return;
}

/* end: added by steve */

#endif

#define OPTARGS "?dvh:"
int
main(int argc, char ** argv)
{
	IPC_WaitConnection 	*waitConnection = NULL;
	ll_cluster_t		*hb = NULL;
	char pathname[64] = {0};
	int	fd;

	int c;
#if HAVE_GETOPT_H
	static struct option long_options[] = {
		{"daemon", 0, 0, 'd'},
		{"verbose", 0, 0, 'v'},
		{"help", 0, 0, 'h'},
		{0, 0, 0, 0}
	};
#endif

	(void)_ha_msg_h_Id;
	(void)_heartbeat_h_Id;

	saCkptService = (SaCkptServiceT*)SaCkptMalloc(sizeof(SaCkptServiceT));
	if (saCkptService == NULL) {
		printf("Cannot allocat memory\n");
	}

	// get options
	while (1) {
#if HAVE_GETOPT_H
		c = getopt_long(argc, argv, OPTARGS, long_options, NULL);
#else
		c = getopt(argc, argv, OPTARGS);
#endif

		if (c == -1) break;
		switch (c) {
		case 'd': 
			saCkptService->flagDaemon= 1;
			break;
		case 'v': 
			saCkptService->flagVerbose = 1;
			break;
		case 'h': 
		case '?': 
		default :
			usage();
			exit(0);
		}
	}
	
	if (saCkptService->flagDaemon) {
		daemon(0, 0);
	} 

	SaCkptCheckpointdInit();

	mainloop = g_main_new(TRUE);

	/* heartbeat message process */
	hb = saCkptService->heartbeat;
	fd = hb->llc_ops->inputfd(hb);
	G_main_add_fd(G_PRIORITY_HIGH, 
		fd, 
		FALSE, 
		SaCkptHbInputDispatch, 
		NULL, 
		SaCkptHbInputDestroy);

	/* 
	 * the clients wait channel is the other source of events.
	 * This source delivers the clients connection events.
	 * listen to this source at a relatively lower priority.
	 */
	memset (pathname, 0, sizeof(pathname));
	strcpy (pathname, CKPTIPC) ;
	waitConnection = SaCkptWaitChannelInit(pathname);
	G_main_add_IPC_WaitConnection(G_PRIORITY_LOW, 
		waitConnection, 
		NULL,
		FALSE, 
		SaCkptWaitChannelDispatch, 
		waitConnection,
		SaCkptWaitChannelDestroy);

	g_main_run(mainloop);
	g_main_destroy(mainloop);

	g_hash_table_destroy(saCkptService->replicaHash);
	g_hash_table_destroy(saCkptService->clientHash);
	g_hash_table_destroy(saCkptService->openCheckpointHash);
	SaCkptFree((void*)&saCkptService);

#ifdef USE_DMALLOC
	dmalloc_shutdown();
#endif

	cl_log(LOG_INFO, "=== Checkpointd exited ===");

	return 0;
}

