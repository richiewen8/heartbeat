/* $Id: libckpt.c,v 1.7 2004/03/12 18:01:47 msoffen Exp $ */
/* 
 * ckptlib.c: data checkpoint API library
 *
 * Copyright (C) 2003 Jerry Yu <jerry.yu@intel.com>
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
 */
 
/* 
 * This library is an implementation of the Application Interface 
 * Specification on Service Availability Forum. Refer to: www.saforum.org/
 * specification
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <errno.h>
#include <string.h>

#include <glib.h>

#include <clplumbing/cl_log.h>
#include <clplumbing/cl_signal.h>
#include <clplumbing/ipc.h>
#include <clplumbing/Gmain_timeout.h>
#include <clplumbing/realtime.h>

#include <saf/ais.h>
#include <checkpointd/clientrequest.h>

#ifndef AF_LOCAL
#	define AF_LOCAL	AF_UNIX
#endif

/* 
 * TODO list: 
 * 1. make all APIs thread safe
 */

/* 
 * the request timeout value in seconds 
 */

#define	LIB_REQUEST_TIMEOUT	10

/* 
 * the client structure  
 */
typedef struct _SaCkptLibClientT{
	char		hostName[SA_MAX_NAME_LENGTH];
	pid_t		pid;
	int		threadID;

	/* The handle returned by the checkpoint service daemon */
	SaCkptHandleT	clientHandle;

	/* 
	 * The client channel 
	 * channel[0] is for the sync calls
	 * channel[1] is for the async calls
	 */
	IPC_Channel*	channel[2];
	
	/* 
	 * the opened checkpoints
	 */
	GList*		checkpointList;

	SaCkptCallbacksT callbacks;
} SaCkptLibClientT; 

/* 
 * the request structure  
 */
typedef struct _SaCkptLibRequestT{
	SaCkptLibClientT*	client;

	SaCkptClientRequestT*	clientRequest;

	/* request timeout handler tag */
	guint			timeoutTag;

} SaCkptLibRequestT;

typedef struct _SaCkptLibCheckpointT{
	SaCkptLibClientT*	client;

	/* 
	 * opened checkpoint handle. 
	 * Returned by checkpoint service daemon
	 */
	SaCkptCheckpointHandleT checkpointHandle; 
	
	SaNameT ckptName; 

	/*
	 * checkpoint attributes
	 */
	SaCkptCheckpointOpenFlagsT		openFlag;
	SaCkptCheckpointCreationAttributesT	createAttributes;

	GList* sectionList;
} SaCkptLibCheckpointT;

GList*	libClientList = NULL;
GList*	libCheckpointList = NULL;

GList* 	libResponseList = NULL;

GList*	libAsyncRequestList = NULL;
GList*	libAsyncResponseList = NULL;

GHashTable*	libIteratorHash = NULL;

SaCkptCallbacksT *libCallback = NULL;

extern void *ha_malloc(size_t size);
extern void  ha_free(void *mem);

static IPC_Channel*
SaCkptClientChannelInit(char* pathname)
{
	IPC_Channel *clientChannel = NULL;
	mode_t mask;
	char path[] = IPC_PATH_ATTR;
	char domainsocket[] = IPC_DOMAIN_SOCKET;

	GHashTable *attrs = g_hash_table_new(g_str_hash,g_str_equal);

	g_hash_table_insert(attrs, path, pathname);

	mask = umask(0);
	clientChannel = ipc_channel_constructor(domainsocket, attrs);
	if (clientChannel == NULL){
		cl_log(LOG_ERR, 
			"Checkpoint library Can't create client channel");
		return NULL;
	}
	mask = umask(mask);

	g_hash_table_destroy(attrs);

	return clientChannel;
}

static SaUint32T 
SaCkptLibGetReqNO(void)
{
	static SaUint32T ckptLibRequestNO = 1;
	return ckptLibRequestNO++;
}

static SaCkptSectionIteratorT 
SaCkptLibGetIterator(void)
{
	static SaCkptSectionIteratorT ckptLibSecIterator = 1;
	
	SaCkptSectionIteratorT iterator;
	GList* sectionList = NULL;

	do {
		iterator = ckptLibSecIterator++;
		sectionList = g_hash_table_lookup(
			libIteratorHash, &iterator);
	} while (sectionList != NULL);

	return iterator;
}

static SaCkptLibRequestT*
SaCkptGetLibRequestByReqno(SaUint32T reqno)
{
	SaCkptLibRequestT* libRequest;
	GList* list = NULL;
	
	list = libAsyncRequestList;
	while( list != NULL ) {
		libRequest= (SaCkptLibRequestT*)(list->data);
		if(libRequest->clientRequest->requestNO == reqno) {
			return libRequest;
		}
		list = g_list_next(list);
	}
	return NULL;
}

static SaCkptClientResponseT*
SaCkptGetLibResponseByReqno(SaUint32T reqno)
{
	SaCkptClientResponseT* libResponse;
	GList* list = NULL;
	
	list = libResponseList;
	while( list != NULL ) {
		libResponse= (SaCkptClientResponseT*)(list->data);
		if(libResponse->requestNO == reqno) {
			libResponseList = g_list_remove(
				libResponseList,
				libResponse);
			return libResponse;
		}
		list = g_list_next(list);
	}
	return NULL;

}

static SaCkptLibClientT*
SaCkptGetLibClientByHandle(SaCkptHandleT clientHandle)
{
	SaCkptLibClientT* libClient;
	GList* list = NULL;
	
	list = libClientList;
	while( list != NULL ) {
		libClient = (SaCkptLibClientT*)(list->data);
		if(libClient->clientHandle == clientHandle) {
			return libClient;
		}
		list = g_list_next(list);
	}
	return NULL;
}

static SaCkptLibCheckpointT*
SaCkptGetLibCheckpointByHandle(
	SaCkptCheckpointHandleT checkpointHandle)
{
	GList* list;
	SaCkptLibCheckpointT* libCheckpoint;

	list = libCheckpointList;
	while(list != NULL ) {
		libCheckpoint = (SaCkptLibCheckpointT*)(list->data);
		if(libCheckpoint->checkpointHandle == checkpointHandle) {
			return libCheckpoint;
		}
		list = g_list_next(list);
	}
	return NULL;
}

static SaErrorT
SaCkptLibRequestSend(IPC_Channel* ch, 
	SaCkptClientRequestT* ckptReq) 
{
	IPC_Message* ipcMsg = NULL;
	
	int rc = IPC_OK;
	char* p = NULL;

	if(ch->ch_status != IPC_CONNECT) {
		cl_log(LOG_WARNING, 
			"IPC is in state %d before send message",
			ch->ch_status);
		return SA_ERR_LIBRARY;
	}
	
	/* build response message */
	ipcMsg = (IPC_Message*)ha_malloc(sizeof(IPC_Message));
	if (ipcMsg == NULL) {
		cl_log(LOG_ERR, "No memory in checkpoint library");
		return SA_ERR_NO_MEMORY;
	}
	
	ipcMsg->msg_private = NULL;
	ipcMsg->msg_done = NULL;
	ipcMsg->msg_ch = ch;
	ipcMsg->msg_len = sizeof(SaCkptClientRequestT) - 
		2* sizeof(void*) + 
		ckptReq->dataLength +
		ckptReq->reqParamLength ;
	ipcMsg->msg_body = ha_malloc(ipcMsg->msg_len);
	if (ipcMsg->msg_body == NULL) {
		cl_log(LOG_ERR, "No memory in checkpoint library");
		ha_free(ipcMsg);
		return SA_ERR_NO_MEMORY;
	}

	p = ipcMsg->msg_body;
	
	memcpy(p, ckptReq, 
		sizeof(SaCkptClientRequestT) - 2*sizeof(void*));
	p += sizeof(SaCkptClientRequestT) - 2*sizeof(void*);

	if (ckptReq->reqParamLength > 0) {
		memcpy(p, ckptReq->reqParam,
			ckptReq->reqParamLength);
		p += ckptReq->reqParamLength;
	}
	
	if (ckptReq->dataLength> 0) {
		memcpy(p, ckptReq->data, 
			ckptReq->dataLength);
		p += ckptReq->dataLength;
	}
		
	/* send request message */
	while (ch->ops->send(ch, ipcMsg) == IPC_FAIL) {
		cl_log(LOG_ERR, "Checkpoint library send request failed");
		cl_log(LOG_ERR, "Sleep for a while and try again");
		cl_shortsleep();
	}
	if(ch->ch_status != IPC_CONNECT) {
		cl_log(LOG_WARNING, 
			"IPC is in state %d after send message",
			ch->ch_status);
	}

	ch->ops->waitout(ch);
	
	/* free ipc message */
	if (ipcMsg != NULL) {
		if (ipcMsg->msg_body != NULL) {
			ha_free(ipcMsg->msg_body);
		}
		ha_free(ipcMsg);
	}

	if (rc == IPC_OK) {
		return SA_OK;
	} else {
		return SA_ERR_LIBRARY;
	}
	
}

static SaErrorT 
SaCkptLibResponseReceive(IPC_Channel* ch, 
	SaUint32T requestNO,
	SaCkptClientResponseT** pCkptResp) 
{

	SaCkptClientResponseT* ckptResp = NULL;

	IPC_Message	*ipcMsg = NULL;
	int		rc = IPC_OK;
	SaErrorT	retval = SA_OK;
	char		*p = NULL;

	ckptResp = SaCkptGetLibResponseByReqno(requestNO);
	if (ckptResp != NULL) {
		*pCkptResp = ckptResp;
		return SA_OK;
	}
	
	if(ch->ch_status != IPC_CONNECT) {
		cl_log(LOG_WARNING, 
			"IPC is in state %d before receive message",
			ch->ch_status);
		return SA_ERR_LIBRARY;
	}
	
	while (ch->ops->is_message_pending(ch) != TRUE) {
		cl_shortsleep();
	}

	while (ch->ops->is_message_pending(ch) == TRUE) {
		/* receive ipc message */
		rc = ch->ops->recv(ch, &ipcMsg);
		if (rc != IPC_OK) {
			cl_log(LOG_ERR, "Receive response failed");
			ha_free(ipcMsg);
			retval =  SA_ERR_LIBRARY;
			break;
		}

		if (ipcMsg->msg_len <
			sizeof(SaCkptClientResponseT) - sizeof(void*)) {
			cl_log(LOG_ERR, "Received error response");
			ha_free(ipcMsg);
			retval = SA_ERR_LIBRARY;
			break;
		}
		p = ipcMsg->msg_body;

		ckptResp = ha_malloc(sizeof(SaCkptClientResponseT));
		if (ckptResp == NULL) {
			cl_log(LOG_ERR, 
				"No memory in checkpoint library");
			if (ipcMsg != NULL) {
				if (ipcMsg->msg_body != NULL) {
					ha_free(ipcMsg->msg_body);
				}
				ha_free(ipcMsg);
			}
			retval = SA_ERR_NO_MEMORY;
			break;
		}

		memset(ckptResp, 0, sizeof(SaCkptClientResponseT));
		memcpy(ckptResp, p, 
			sizeof(SaCkptClientResponseT) - sizeof(void*));
		p += (sizeof(SaCkptClientResponseT) - sizeof(void*));

		if (ckptResp->dataLength > 0) {
			ckptResp->data = ha_malloc(ckptResp->dataLength);
			if (ckptResp->data == NULL) {
				cl_log(LOG_ERR, 
					"No memory in checkpoint library");
				if (ipcMsg != NULL) {
					if (ipcMsg->msg_body != NULL) {
						ha_free(ipcMsg->msg_body);
					}
					ha_free(ipcMsg);
				}
				ha_free(ckptResp);
				retval = SA_ERR_NO_MEMORY;
				break;
			} else {
				memcpy(ckptResp->data, p, 
					ckptResp->dataLength);
				p += ckptResp->dataLength;
			}
		} else {
			ckptResp->data = NULL;
		}

		/* free ipc message */
		if (ipcMsg->msg_body != NULL) {
			ha_free(ipcMsg->msg_body);
		}
		ha_free(ipcMsg);
		
		libResponseList = g_list_append(libResponseList,
			ckptResp);
	}
	
	ckptResp = SaCkptGetLibResponseByReqno(requestNO);
	if (ckptResp != NULL) {
		*pCkptResp = ckptResp;
		return SA_OK;
	}

	return retval;
}

static SaErrorT 
SaCkptLibResponseReceiveAsync(IPC_Channel* ch) 
{
	SaCkptClientResponseT* ckptResp = NULL;

	IPC_Message	*ipcMsg = NULL;
	int		rc = IPC_OK;
	SaErrorT	retval = SA_OK;
	char		*p = NULL;

	if(ch->ch_status != IPC_CONNECT) {
		cl_log(LOG_WARNING, 
			"IPC is in state %d before receive message",
			ch->ch_status);
		return SA_ERR_LIBRARY;
	}
	
	if (ch->ops->is_message_pending(ch) != TRUE) {
		return SA_OK;
	}

	while (ch->ops->is_message_pending(ch) == TRUE) {
		/* receive ipc message */
		rc = ch->ops->recv(ch, &ipcMsg);
		if (rc != IPC_OK) {
			cl_log(LOG_ERR, "Receive response failed");
			ha_free(ipcMsg);
			retval =  SA_ERR_LIBRARY;
			break;
		}

		if (ipcMsg->msg_len <
			sizeof(SaCkptClientResponseT) - sizeof(void*)) {
			cl_log(LOG_ERR, "Received error response");
			ha_free(ipcMsg);
			retval = SA_ERR_LIBRARY;
			break;
		}
		p = ipcMsg->msg_body;

		ckptResp = ha_malloc(sizeof(SaCkptClientResponseT));
		if (ckptResp == NULL) {
			cl_log(LOG_ERR, 
				"No memory in checkpoint library");
			if (ipcMsg != NULL) {
				if (ipcMsg->msg_body != NULL) {
					ha_free(ipcMsg->msg_body);
				}
				ha_free(ipcMsg);
			}
			retval = SA_ERR_NO_MEMORY;
			break;
		}

		memset(ckptResp, 0, sizeof(SaCkptClientResponseT));
		memcpy(ckptResp, p, 
			sizeof(SaCkptClientResponseT) - sizeof(void*));
		p += (sizeof(SaCkptClientResponseT) - sizeof(void*));

		if (ckptResp->dataLength > 0) {
			ckptResp->data = ha_malloc(ckptResp->dataLength);
			if (ckptResp->data == NULL) {
				cl_log(LOG_ERR, 
					"No memory in checkpoint library");
				if (ipcMsg != NULL) {
					if (ipcMsg->msg_body != NULL) {
						ha_free(ipcMsg->msg_body);
					}
					ha_free(ipcMsg);
				}
				ha_free(ckptResp);
				retval = SA_ERR_NO_MEMORY;
				break;
			} else {
				memcpy(ckptResp->data, p, 
					ckptResp->dataLength);
				p += ckptResp->dataLength;
			}
		} else {
			ckptResp->data = NULL;
		}

		/* free ipc message */
		if (ipcMsg->msg_body != NULL) {
			ha_free(ipcMsg->msg_body);
		}
		ha_free(ipcMsg);
		
		libAsyncResponseList = g_list_append(
			libAsyncResponseList,
			ckptResp);
	}
	
	return retval;
}

/* ------------------------- exported API ------------------------------- */

/* 
 * Initialize the checkpoint service for the invoking process and register
 * the related callback functions. 
 */
SaErrorT
saCkptInitialize(SaCkptHandleT *ckptHandle/*[out]*/,
	const SaCkptCallbacksT *callbacks,
	const SaVersionT *version)
{	
	SaErrorT libError = SA_OK;
	
	SaCkptLibClientT* libClient = NULL;
	SaCkptLibRequestT* libRequest = NULL;
	SaCkptClientRequestT* clientRequest = NULL;
	SaCkptReqInitParamT* initParam = NULL;
	SaCkptClientResponseT* clientResponse = NULL;

	int i = 0;

	cl_log_set_entity("AIS");
	cl_log_enable_stderr(TRUE);
	
	if (version == NULL) {
		cl_log(LOG_ERR, 
			"Null version number in checkpoint library");
		return SA_ERR_INVALID_PARAM;
	}

	libClient = (SaCkptLibClientT*)ha_malloc(
					sizeof(SaCkptLibClientT));
  	libRequest = (SaCkptLibRequestT*)ha_malloc(
					sizeof(SaCkptLibRequestT));
	clientRequest = (SaCkptClientRequestT*)ha_malloc(
					sizeof(SaCkptClientRequestT));
	initParam = (SaCkptReqInitParamT*)ha_malloc(
					sizeof(SaCkptReqInitParamT));
	if ((libClient == NULL) || 
		(libRequest == NULL) ||
		(clientRequest == NULL) ||
		(initParam == NULL)) {
		cl_log(LOG_ERR, 
			"No memory in checkpoint library");
		libError = SA_ERR_NO_MEMORY;
		goto initError;
	}
	
	memset(libClient, 0, sizeof(SaCkptLibClientT));
	memset(libRequest, 0, sizeof(SaCkptLibRequestT));
	memset(clientRequest, 0, sizeof(SaCkptClientRequestT));
	memset(initParam, 0, sizeof(SaCkptReqInitParamT));
	
	libClient->hostName[0] = 0;
	libClient->pid = getpid();
	libClient->threadID = 0;
	libClient->clientHandle = 0;
	libClient->checkpointList = NULL;
	for (i=0; i<2; i++) {
		IPC_Channel* ch = NULL;
		char pathname[128];
		
		memset (pathname, 0, sizeof(pathname));
		strcpy (pathname, CKPTIPC) ;
		ch = SaCkptClientChannelInit(pathname);
		if (ch == NULL) {
			cl_log(LOG_ERR, 
			"Checkpoint library can not initiate connection");
			libError = SA_ERR_LIBRARY;
			goto initError;
		}
		if (ch->ops->initiate_connection(ch)
			!= IPC_OK) {
			cl_log(LOG_ERR, 
			"Checkpoint library can not connect to daemon");
			libError = SA_ERR_LIBRARY;
			goto initError;
		}

		libClient->channel[i] = ch;
	}

	libRequest->client = libClient;
	libRequest->timeoutTag = 0;
	libRequest->clientRequest = clientRequest;

	clientRequest->clientHandle = 0;
	clientRequest->requestNO = SaCkptLibGetReqNO();
	clientRequest->req = REQ_SERVICE_INIT;
	clientRequest->reqParamLength = sizeof(SaCkptReqInitParamT);
	clientRequest->dataLength = 0;
	clientRequest->reqParam = initParam;
	clientRequest->data = NULL;

	initParam->pid = libClient->pid;
	initParam->tid = libClient->threadID;
	memcpy(&(initParam->ver), version, sizeof(SaVersionT));

	libError = SaCkptLibRequestSend(libClient->channel[0], 
		libRequest->clientRequest);
	if (libError != SA_OK) {
		cl_log(LOG_ERR, 
			"Send initialize request failed");
		goto initError;
	}

	libError = SaCkptLibResponseReceive(libClient->channel[0], 
		libRequest->clientRequest->requestNO,
		&clientResponse);
	if (libError != SA_OK) {
		cl_log(LOG_ERR, 
			"Receive response failed");
		goto initError;
	}
	if (clientResponse == NULL) {
		cl_log(LOG_ERR, 
			"Received null response");
		libError = SA_ERR_LIBRARY;
		goto initError;
	}
	if (clientResponse->retVal != SA_OK) {
		cl_log(LOG_ERR, 
			"Checkpoint daemon returned error");
		libError = clientResponse->retVal;
		goto initError;
	}

	*ckptHandle = clientResponse->clientHandle;
	
	libClient->clientHandle = *ckptHandle;
	if (callbacks != NULL) {
		libClient->callbacks = *callbacks;
	}
	libClientList = g_list_append(libClientList, libClient);

	if (libIteratorHash == NULL) {
		libIteratorHash = 
			g_hash_table_new(g_int_hash, g_int_equal);
	}

	/*
	 * FIXME
	 * initialize the asyn channel and register callbacks
	 */

	libError = SA_OK;

initError:
	if (libError != SA_OK) {
		if (libClient != NULL) {
			for (i=0; i<2; i++) {
				if (libClient->channel[i] != NULL) {
					IPC_Channel* ch = 
						libClient->channel[i];
					ch->ops->destroy(ch);
				}
			}
			ha_free(libClient);
		}
	}
	
	if (libRequest != NULL) {
		ha_free(libRequest);
	}
	
	if (clientRequest != NULL) {
		ha_free(clientRequest);
	}
	
	if (initParam != NULL) {
		ha_free(initParam);
	}

	if (clientResponse != NULL) {
		if (clientResponse->dataLength > 0) {
			ha_free(clientResponse->data);
		}
		ha_free(clientResponse);
	}

	return libError;
}

/* 
 * Returns the fd for asynchronously operation
 */
SaErrorT
saCkptSelectionObjectGet(
	const SaCkptHandleT *ckptHandle,
	SaSelectionObjectT *selectionObject/*[out]*/)
{
	SaCkptLibClientT* libClient = NULL;
	IPC_Channel* ch = NULL;

	if (ckptHandle == NULL) {
		cl_log(LOG_ERR, 
			"Null handle in checkpoint library");
		return SA_ERR_INVALID_PARAM;
	}

	libClient = SaCkptGetLibClientByHandle(*ckptHandle);
	if (libClient == NULL) {
		cl_log(LOG_ERR, 
			"Invalid handle in checkpoint library");
		return SA_ERR_INVALID_PARAM;
	}
	
	ch = libClient->channel[1];
	*selectionObject = ch->ops->get_recv_select_fd(ch);
	
	return SA_OK;
}

/* 
 * This function invokes, in the context of the calling thread,one or all 
 * of the pending callbacks for the handle ckptHandle.
 */
SaErrorT
saCkptDispatch(const SaCkptHandleT *ckptHandle,
	SaDispatchFlagsT dispatchFlags)
{
	SaCkptLibClientT* libClient = NULL;
	SaCkptLibRequestT* libRequest = NULL;
	
	SaCkptClientRequestT* clientRequest = NULL;
 	SaCkptClientResponseT* clientResponse = NULL;
	
	SaInvocationT invocation;
	SaCkptLibCheckpointT* libCheckpoint = NULL;
	SaCkptCheckpointHandleT* checkpointHandle = NULL;
	SaUint32T reqno;
	SaCkptReqOpenAsyncParamT* openAsyncParam = NULL;
	SaCkptReqAsyncParamT* asyncParam = NULL;
	
	SaErrorT libError = SA_OK;
	IPC_Channel* ch = NULL;
	
	if (ckptHandle == NULL) {
		cl_log(LOG_ERR, 
			"Null handle in checkpoint library");
		return SA_ERR_INVALID_PARAM;
	}

	libClient = SaCkptGetLibClientByHandle(*ckptHandle);
	if (libClient == NULL) {
		cl_log(LOG_ERR, 
			"Invalid handle in checkpoint library");
		return SA_ERR_INVALID_PARAM;
	}
	
	ch = libClient->channel[1]; /* async channel */
	libError = SaCkptLibResponseReceiveAsync(ch);
	if (libError != SA_OK) {
		cl_log(LOG_ERR, 
			"Receive response failed");
		return SA_ERR_LIBRARY;
	}
	
	if (g_list_length(libAsyncResponseList) == 0) {
		return SA_OK;
	}

	while (libAsyncResponseList != NULL) {
		clientResponse = libAsyncResponseList->data;
		
		libError = clientResponse->retVal;
		reqno = clientResponse->requestNO;
		libRequest = SaCkptGetLibRequestByReqno(reqno);
		clientRequest = libRequest->clientRequest;
		switch (clientRequest->req) {
		case REQ_CKPT_OPEN_ASYNC:
			openAsyncParam = clientRequest->reqParam;
			invocation = openAsyncParam->invocation;
			memcpy(checkpointHandle, clientResponse->data,
				sizeof(SaCkptCheckpointHandleT));

			/*
			 * create libCheckpoint and add it to the 
			 * opened checkpoint list
			 */
			libCheckpoint->client = libClient;
			libCheckpoint->checkpointHandle = *checkpointHandle;
			libCheckpoint->ckptName.length = 
				openAsyncParam->ckptName.length;
			memcpy(libCheckpoint->ckptName.value, 
				openAsyncParam->ckptName.value,
				openAsyncParam->ckptName.length);
			memcpy(&(libCheckpoint->createAttributes),
				&openAsyncParam->attr,
				sizeof(SaCkptCheckpointCreationAttributesT));
			libCheckpoint->openFlag = openAsyncParam->openFlag;
				
			libClient->checkpointList = g_list_append(
				libClient->checkpointList,
				libCheckpoint);
			libCheckpointList = g_list_append(libCheckpointList,
				libCheckpoint);
			
			
			libCallback->saCkptCheckpointOpenCallback(
				invocation, checkpointHandle, libError);
			break;
		case REQ_CKPT_SYNC_ASYNC:
			asyncParam = clientRequest->reqParam;
			invocation = asyncParam->invocation;
			libCallback->saCkptCheckpointSynchronizeCallback(
				invocation, libError);
			break;
		default:
			break;
		};

		libAsyncResponseList = g_list_remove(libAsyncResponseList,
			clientResponse);
		libAsyncRequestList = g_list_remove(libAsyncRequestList,
			libRequest);
		ha_free(clientResponse->data);
		ha_free(clientResponse);
		ha_free(libRequest->clientRequest->reqParam);
		ha_free(libRequest->clientRequest->data);
		ha_free(libRequest->clientRequest);
		ha_free(libRequest);
		
		if (dispatchFlags == SA_DISPATCH_ONE) {
			break;
		}
	}
	
	return SA_OK; 
}

/* 
 * closes the association, represented by ckptHandle, between the process and
 * the Checkpoint Service. It frees up resources and close all the opened 
 * checkpoints.
 */
SaErrorT
saCkptFinalize(const SaCkptHandleT *ckptHandle)
{
	SaCkptLibClientT* libClient = NULL;
	SaCkptLibRequestT* libRequest = NULL;
	SaCkptClientRequestT* clientRequest = NULL;
	SaCkptReqFinalParamT* finalParam = NULL;
 	SaCkptClientResponseT* clientResponse = NULL;
	
	SaErrorT libError = SA_OK;
	IPC_Channel* ch = NULL;
	int i = 0;
	
	GList* list = NULL;

	if (ckptHandle == NULL) {
		cl_log(LOG_ERR, 
			"Null handle in checkpoint library");
		return SA_ERR_INVALID_PARAM;
	}

	libClient = SaCkptGetLibClientByHandle(*ckptHandle);
	if (libClient == NULL) {
		cl_log(LOG_ERR, 
			"Invalid handle in checkpoint library");
		return SA_ERR_INVALID_PARAM;
	}
	
	/*
	 * close all opened checkpoints first
	 */
	list = libClient->checkpointList;
	while(list != NULL ) {
		SaCkptLibCheckpointT* libCheckpoint = NULL;
		SaCkptCheckpointHandleT* checkpointHandle = NULL;

		libCheckpoint = (SaCkptLibCheckpointT*)(list->data);
		checkpointHandle = &(libCheckpoint->checkpointHandle);
		saCkptCheckpointClose(checkpointHandle);
		ha_free(libCheckpoint);

		list = libClient->checkpointList;
	}
	
  	libRequest = (SaCkptLibRequestT*)ha_malloc(
					sizeof(SaCkptLibRequestT));
	clientRequest = (SaCkptClientRequestT*)ha_malloc(
					sizeof(SaCkptClientRequestT));
	finalParam = (SaCkptReqFinalParamT*)ha_malloc(
					sizeof(SaCkptReqFinalParamT));
 	if ((libRequest == NULL) ||
		(clientRequest == NULL) ||
		(finalParam == NULL)) {
		cl_log(LOG_ERR, 
			"No memory in checkpoint library");
		libError = SA_ERR_NO_MEMORY;
		goto finalError;
	}
	
	memset(libRequest, 0, sizeof(SaCkptLibRequestT));
	memset(clientRequest, 0, sizeof(SaCkptClientRequestT));
	memset(finalParam, 0, sizeof(SaCkptReqFinalParamT));
	
	libRequest->client = libClient;
	libRequest->timeoutTag = 0;
	libRequest->clientRequest = clientRequest;

	clientRequest->clientHandle = libClient->clientHandle;
	clientRequest->requestNO = SaCkptLibGetReqNO();
	clientRequest->req = REQ_SERVICE_FINL;
	clientRequest->reqParamLength = sizeof(SaCkptReqFinalParamT);
	clientRequest->dataLength = 0;
	clientRequest->reqParam = finalParam;
	clientRequest->data = NULL;

	finalParam->clientHandle = *ckptHandle;

	ch = libClient->channel[0];
	libError = SaCkptLibRequestSend(ch, libRequest->clientRequest);
	if (libError != SA_OK) {
		cl_log(LOG_ERR, 
			"Send finalize request failed");
		goto finalError;
	}

	libError = SaCkptLibResponseReceive(ch, 
		libRequest->clientRequest->requestNO,
		&clientResponse);
	if (libError != SA_OK) {
		cl_log(LOG_ERR, 
			"Receive response failed");
		goto finalError;
	}
	if (clientResponse == NULL) {
		cl_log(LOG_ERR, 
			"Received null response");
		libError = SA_ERR_LIBRARY;
		goto finalError;
	}
	if (clientResponse->retVal != SA_OK) {
		cl_log(LOG_ERR, 
			"Checkpoint daemon returned error");
		libError = clientResponse->retVal;
		goto finalError;
	}

	
	/*
	 * FIXME
	 * cancel all pending callbacks related to this client
	 */
	
	/* remove client */
	libClientList = g_list_remove(libClientList, libClient);

	/* only destroy the hash table after all the clients finalized */
	if (g_list_length(libClientList) == 0) {
		g_hash_table_destroy(libIteratorHash);
	}
	
	libError = SA_OK;

finalError:
	if (libError == SA_OK) {
		if (libClient != NULL) {
			for (i=0; i<2; i++) {
				if (libClient->channel[i] != NULL) {
					ch = libClient->channel[i];
					ch->ops->destroy(ch);
				}
			}
			ha_free(libClient);
		}
	}
	
	if (libRequest != NULL) {
		ha_free(libRequest);
	}
	
	if (clientRequest != NULL) {
		ha_free(clientRequest);
	}
	
	if (finalParam != NULL) {
		ha_free(finalParam);
	}

	if (clientResponse != NULL) {
		if (clientResponse->dataLength > 0) {
			ha_free(clientResponse->data);
		}
		ha_free(clientResponse);
	}

	return libError; 
}

/* 
 * the invocation of this function is blocking. A new checkpoint handle is
 * returned upon completion.
 */
SaErrorT
saCkptCheckpointOpen(
	const SaCkptHandleT *ckptHandle,
	const SaNameT *checkpointName,
	const SaCkptCheckpointCreationAttributesT *checkpointCreationAttributes,
	SaCkptCheckpointOpenFlagsT checkpointOpenFlags,
	SaTimeT timeout,
	SaCkptCheckpointHandleT *checkpointHandle/*[out]*/)
{
	SaCkptLibClientT* libClient = NULL;
	SaCkptLibRequestT* libRequest = NULL;
	SaCkptClientRequestT* clientRequest = NULL;
	SaCkptReqOpenParamT* openParam = NULL;
 	SaCkptClientResponseT* clientResponse = NULL;
	SaCkptLibCheckpointT* libCheckpoint = NULL;
	
	SaErrorT libError = SA_OK;
	IPC_Channel* ch = NULL;
	
	if (ckptHandle == NULL) {
		cl_log(LOG_ERR, 
			"Null handle in checkpoint library");
		return SA_ERR_INVALID_PARAM;
	}

	libClient = SaCkptGetLibClientByHandle(*ckptHandle);
	if (libClient == NULL) {
		cl_log(LOG_ERR, 
			"Invalid handle in checkpoint library");
		return SA_ERR_INVALID_PARAM;
	}
	
  	libRequest = (SaCkptLibRequestT*)ha_malloc(
					sizeof(SaCkptLibRequestT));
	clientRequest = (SaCkptClientRequestT*)ha_malloc(
					sizeof(SaCkptClientRequestT));
	openParam = (SaCkptReqOpenParamT*)ha_malloc(
					sizeof(SaCkptReqOpenParamT));
	libCheckpoint = (SaCkptLibCheckpointT*)ha_malloc(
					sizeof(SaCkptLibCheckpointT));
 	if ((libRequest == NULL) ||
		(clientRequest == NULL) ||
		(openParam == NULL) ||
		(libCheckpoint == NULL)) {
		cl_log(LOG_ERR, 
			"No memory in checkpoint library");
		libError = SA_ERR_NO_MEMORY;
		goto openError;
	}
	
	memset(libRequest, 0, sizeof(SaCkptLibRequestT));
	memset(clientRequest, 0, sizeof(SaCkptClientRequestT));
	memset(openParam, 0, sizeof(SaCkptReqOpenParamT));
	memset(libCheckpoint, 0, sizeof(SaCkptLibCheckpointT));
	
	libRequest->client = libClient;
	libRequest->timeoutTag = 0;
	libRequest->clientRequest = clientRequest;

	clientRequest->clientHandle = libClient->clientHandle;
	clientRequest->requestNO = SaCkptLibGetReqNO();
	clientRequest->req = REQ_CKPT_OPEN;
	clientRequest->reqParamLength = sizeof(SaCkptReqOpenParamT);
	clientRequest->dataLength = 0;
	clientRequest->reqParam = openParam;
	clientRequest->data = NULL;

	memcpy(&(openParam->attr), checkpointCreationAttributes,
		sizeof(SaCkptCheckpointCreationAttributesT));
	openParam->openFlag = checkpointOpenFlags;
	openParam->timetout = timeout;
	openParam->ckptName.length = checkpointName->length;
	memcpy(openParam->ckptName.value, checkpointName->value,
		checkpointName->length);

	ch = libClient->channel[0];
	libError = SaCkptLibRequestSend(ch, libRequest->clientRequest);
	if (libError != SA_OK) {
		cl_log(LOG_ERR, 
			"Send open request failed");
		goto openError;
	}

	libError = SaCkptLibResponseReceive(ch, 
		libRequest->clientRequest->requestNO,
		&clientResponse);
	if (libError != SA_OK) {
		cl_log(LOG_ERR, 
			"Receive response failed");
		goto openError;
	}
	if (clientResponse == NULL) {
		cl_log(LOG_ERR, 
			"Received null response");
		libError = SA_ERR_LIBRARY;
		goto openError;
	}
	if (clientResponse->retVal != SA_OK) {
		cl_log(LOG_ERR, 
			"Checkpoint daemon returned error");
		libError = clientResponse->retVal;
		goto openError;
	}

	memcpy(checkpointHandle, clientResponse->data,
		sizeof(SaCkptCheckpointHandleT));

	/*
	 * create libCheckpoint and add it to the opened checkpoint list
	 */
	libCheckpoint->client = libClient;
	libCheckpoint->checkpointHandle = *checkpointHandle;
	libCheckpoint->ckptName.length = checkpointName->length;
	memcpy(libCheckpoint->ckptName.value, checkpointName->value,
		checkpointName->length);
	memcpy(&(libCheckpoint->createAttributes),
		checkpointCreationAttributes,
		sizeof(SaCkptCheckpointCreationAttributesT));
	libCheckpoint->openFlag = checkpointOpenFlags;
		
	libClient->checkpointList = g_list_append(
		libClient->checkpointList,
		libCheckpoint);
	libCheckpointList = g_list_append(libCheckpointList,
		libCheckpoint);
	
	libError = SA_OK;

openError:
	if (libError != SA_OK) {
		if (libCheckpoint != NULL) {
			ha_free(libCheckpoint);
		}
	}
	
	if (libRequest != NULL) {
		ha_free(libRequest);
	}
	
	if (clientRequest != NULL) {
		ha_free(clientRequest);
	}
	
	if (openParam != NULL) {
		ha_free(openParam);
	}

	if (clientResponse != NULL) {
		if (clientResponse->dataLength > 0) {
			ha_free(clientResponse->data);
		}
		ha_free(clientResponse);
	}

	return libError; 
}

/* 
 * open a checkpoint asynchronously
 */
SaErrorT
saCkptCheckpointOpenAsync(
	const SaCkptHandleT *ckptHandle,
	SaInvocationT invocation,
	const SaNameT *checkpointName,
	const SaCkptCheckpointCreationAttributesT *checkpointCreationAttributes,
	SaCkptCheckpointOpenFlagsT checkpointOpenFlags)
{
	SaCkptLibClientT* libClient = NULL;
	SaCkptLibRequestT* libRequest = NULL;
	SaCkptClientRequestT* clientRequest = NULL;
	SaCkptReqOpenAsyncParamT* openAsyncParam = NULL;
	SaCkptLibCheckpointT* libCheckpoint = NULL;
	
	SaErrorT libError = SA_OK;
	IPC_Channel* ch = NULL;
	
	if (ckptHandle == NULL) {
		cl_log(LOG_ERR, 
			"Null handle in checkpoint library");
		return SA_ERR_INVALID_PARAM;
	}

	libClient = SaCkptGetLibClientByHandle(*ckptHandle);
	if (libClient == NULL) {
		cl_log(LOG_ERR, 
			"Invalid handle in checkpoint library");
		return SA_ERR_INVALID_PARAM;
	}
	
  	libRequest = (SaCkptLibRequestT*)ha_malloc(
					sizeof(SaCkptLibRequestT));
	clientRequest = (SaCkptClientRequestT*)ha_malloc(
					sizeof(SaCkptClientRequestT));
	openAsyncParam = (SaCkptReqOpenAsyncParamT*)ha_malloc(
					sizeof(SaCkptReqOpenAsyncParamT));
	libCheckpoint = (SaCkptLibCheckpointT*)ha_malloc(
					sizeof(SaCkptLibCheckpointT));
 	if ((libRequest == NULL) ||
		(clientRequest == NULL) ||
		(openAsyncParam == NULL) ||
		(libCheckpoint == NULL)) {
		cl_log(LOG_ERR, 
			"No memory in checkpoint library");
		libError = SA_ERR_NO_MEMORY;
		goto openError;
	}
	
	memset(libRequest, 0, sizeof(SaCkptLibRequestT));
	memset(clientRequest, 0, sizeof(SaCkptClientRequestT));
	memset(openAsyncParam, 0, sizeof(SaCkptReqOpenAsyncParamT));
	memset(libCheckpoint, 0, sizeof(SaCkptLibCheckpointT));
	
	libRequest->client = libClient;
	libRequest->timeoutTag = 0;
	libRequest->clientRequest = clientRequest;

	clientRequest->clientHandle = libClient->clientHandle;
	clientRequest->requestNO = SaCkptLibGetReqNO();
	clientRequest->req = REQ_CKPT_OPEN_ASYNC;
	clientRequest->reqParamLength = sizeof(SaCkptReqOpenAsyncParamT);
	clientRequest->dataLength = 0;
	clientRequest->reqParam = openAsyncParam;
	clientRequest->data = NULL;

	memcpy(&(openAsyncParam->attr), checkpointCreationAttributes,
		sizeof(SaCkptCheckpointCreationAttributesT));
	openAsyncParam->openFlag = checkpointOpenFlags;
	openAsyncParam->invocation= invocation;
	openAsyncParam->ckptName.length = checkpointName->length;
	memcpy(openAsyncParam->ckptName.value, checkpointName->value,
		checkpointName->length);

	ch = libClient->channel[1]; /*async channel*/
	libError = SaCkptLibRequestSend(ch, libRequest->clientRequest);
	if (libError != SA_OK) {
		cl_log(LOG_ERR, 
			"Send open request failed");
		goto openError;
	}

	libAsyncRequestList = g_list_append(libAsyncRequestList, 
		libRequest);
	
	return SA_OK;

openError:
	if (libError != SA_OK) {
		if (libCheckpoint != NULL) {
			ha_free(libCheckpoint);
		}
	}
	
	if (libRequest != NULL) {
		ha_free(libRequest);
	}
	
	if (clientRequest != NULL) {
		ha_free(clientRequest);
	}
	
	if (openAsyncParam != NULL) {
		ha_free(openAsyncParam);
	}

	return libError; 
}

/* 
 * free the resources allocated for checkpoint handle
 */
SaErrorT
saCkptCheckpointClose(
	const SaCkptCheckpointHandleT *checkpointHandle)
{
	SaCkptLibClientT* libClient = NULL;
	SaCkptLibRequestT* libRequest = NULL;
	SaCkptClientRequestT* clientRequest = NULL;
	SaCkptReqCloseParamT* closeParam = NULL;
 	SaCkptClientResponseT* clientResponse = NULL;
	SaCkptLibCheckpointT* libCheckpoint = NULL;
	
	SaErrorT libError = SA_OK;
	IPC_Channel* ch = NULL;

	if (checkpointHandle == NULL) {
		cl_log(LOG_ERR, 
			"Null handle in checkpoint library");
		return SA_ERR_INVALID_PARAM;
	}

	libCheckpoint = SaCkptGetLibCheckpointByHandle(
		*checkpointHandle);
	if (libCheckpoint == NULL) {
		cl_log(LOG_ERR, 
			"Checkpoint is not open");
		return SA_ERR_INVALID_PARAM;
	}
	libClient = libCheckpoint->client;

	
  	libRequest = (SaCkptLibRequestT*)ha_malloc(
					sizeof(SaCkptLibRequestT));
	clientRequest = (SaCkptClientRequestT*)ha_malloc(
					sizeof(SaCkptClientRequestT));
	closeParam = (SaCkptReqCloseParamT*)ha_malloc(
					sizeof(SaCkptReqCloseParamT));
 	if ((libRequest == NULL) ||
		(clientRequest == NULL) ||
		(closeParam == NULL)) {
		cl_log(LOG_ERR, 
			"No memory in checkpoint library");
		libError = SA_ERR_NO_MEMORY;
		goto closeError;
	}
	
	memset(libRequest, 0, sizeof(SaCkptLibRequestT));
	memset(clientRequest, 0, sizeof(SaCkptClientRequestT));
	memset(closeParam, 0, sizeof(SaCkptReqCloseParamT));
	
	libRequest->client = libClient;
	libRequest->timeoutTag = 0;
	libRequest->clientRequest = clientRequest;

	clientRequest->clientHandle = libClient->clientHandle;
	clientRequest->requestNO = SaCkptLibGetReqNO();
	clientRequest->req = REQ_CKPT_CLOSE;
	clientRequest->reqParamLength = sizeof(SaCkptReqCloseParamT);
	clientRequest->dataLength = 0;
	clientRequest->reqParam = closeParam;
	clientRequest->data = NULL;

	closeParam->checkpointHandle = *checkpointHandle;
	
	ch = libClient->channel[0];
	libError = SaCkptLibRequestSend(ch, libRequest->clientRequest);
	if (libError != SA_OK) {
		cl_log(LOG_ERR, 
			"Send close request failed");
		goto closeError;
	}

	libError = SaCkptLibResponseReceive(ch, 
		libRequest->clientRequest->requestNO,
		&clientResponse);
	if (libError != SA_OK) {
		cl_log(LOG_ERR, 
			"Receive response failed");
		goto closeError;
	}
	if (clientResponse == NULL) {
		cl_log(LOG_ERR, 
			"Received null response");
		libError = SA_ERR_LIBRARY;
		goto closeError;
	}
	if (clientResponse->retVal != SA_OK) {
		cl_log(LOG_ERR, 
			"Checkpoint daemon returned error");
		libError = clientResponse->retVal;
		goto closeError;
	}

	libClient->checkpointList = g_list_remove(
		libClient->checkpointList, libCheckpoint);
	libCheckpointList = g_list_remove(
		libCheckpointList, libCheckpoint);

	libError = SA_OK;

closeError:
	if (libError == SA_OK) {
		if (libCheckpoint != NULL) {
			ha_free(libCheckpoint);
		}
	}
	
	if (libRequest != NULL) {
		ha_free(libRequest);
	}
	
	if (clientRequest != NULL) {
		ha_free(clientRequest);
	}
	
	if (closeParam != NULL) {
		ha_free(closeParam);
	}

	if (clientResponse != NULL) {
		if (clientResponse->dataLength > 0) {
			ha_free(clientResponse->data);
		}
		ha_free(clientResponse);
	}

	return libError; 

}

/* 
 * remove this checkpoint.
 */
SaErrorT
saCkptCheckpointUnlink(
	const SaCkptHandleT *ckptHandle,
	const SaNameT *checkpointName)
{
	SaCkptLibClientT* libClient = NULL;
	SaCkptLibRequestT* libRequest = NULL;
	SaCkptClientRequestT* clientRequest = NULL;
	SaCkptReqUlnkParamT* unlinkParam = NULL;
 	SaCkptClientResponseT* clientResponse = NULL;
	
	SaErrorT libError = SA_OK;
	IPC_Channel* ch = NULL;

	if (ckptHandle == NULL) {
		cl_log(LOG_ERR, 
			"Null handle in checkpoint library");
		return SA_ERR_INVALID_PARAM;
	}

	libClient = SaCkptGetLibClientByHandle(*ckptHandle);
	if (libClient == NULL) {
		cl_log(LOG_ERR, 
			"Invalid handle in checkpoint library");
		return SA_ERR_INVALID_PARAM;
	}

  	libRequest = (SaCkptLibRequestT*)ha_malloc(
					sizeof(SaCkptLibRequestT));
	clientRequest = (SaCkptClientRequestT*)ha_malloc(
					sizeof(SaCkptClientRequestT));
	unlinkParam = (SaCkptReqUlnkParamT*)ha_malloc(
					sizeof(SaCkptReqUlnkParamT));
 	if ((libRequest == NULL) ||
		(clientRequest == NULL) ||
		(unlinkParam == NULL)) {
		cl_log(LOG_ERR, 
			"No memory in checkpoint library");
		libError = SA_ERR_NO_MEMORY;
		goto unlinkError;
	}
	
	memset(libRequest, 0, sizeof(SaCkptLibRequestT));
	memset(clientRequest, 0, sizeof(SaCkptClientRequestT));
	memset(unlinkParam, 0, sizeof(SaCkptReqUlnkParamT));
	
	libRequest->client = libClient;
	libRequest->timeoutTag = 0;
	libRequest->clientRequest = clientRequest;

	clientRequest->clientHandle = libClient->clientHandle;
	clientRequest->requestNO = SaCkptLibGetReqNO();
	clientRequest->req = REQ_CKPT_ULNK;
	clientRequest->reqParamLength = sizeof(SaCkptReqUlnkParamT);
	clientRequest->dataLength = 0;
	clientRequest->reqParam = unlinkParam;
	clientRequest->data = NULL;

	unlinkParam->clientHandle = *ckptHandle;
	unlinkParam->ckptName.length = checkpointName->length;
	memcpy(unlinkParam->ckptName.value,
		checkpointName->value,
		checkpointName->length);
	
	ch = libClient->channel[0];
	libError = SaCkptLibRequestSend(ch, libRequest->clientRequest);
	if (libError != SA_OK) {
		cl_log(LOG_ERR, 
			"Send checkpoint_unlink request failed");
		goto unlinkError;
	}

	libError = SaCkptLibResponseReceive(ch, 
		libRequest->clientRequest->requestNO,
		&clientResponse);
	if (libError != SA_OK) {
		cl_log(LOG_ERR, "Receive response failed");
		goto unlinkError;
	}
	if (clientResponse == NULL) {
		cl_log(LOG_ERR, "Received null response");
		libError = SA_ERR_LIBRARY;
		goto unlinkError;
	}
	if (clientResponse->retVal != SA_OK) {
		cl_log(LOG_ERR, "Checkpoint daemon returned error");
		libError = clientResponse->retVal;
		goto unlinkError;
	}

	libError = SA_OK;

unlinkError:
	
	if (libRequest != NULL) {
		ha_free(libRequest);
	}
	
	if (clientRequest != NULL) {
		ha_free(clientRequest);
	}
	
	if (unlinkParam != NULL) {
		ha_free(unlinkParam);
	}

	if (clientResponse != NULL) {
		if (clientResponse->dataLength > 0) {
			ha_free(clientResponse->data);
		}
		ha_free(clientResponse);
	}

	return libError; }

/* 
 * set the checkpoint duration.
 */
SaErrorT
saCkptCheckpointRetentionDurationSet(
	const SaCkptCheckpointHandleT *checkpointHandle,
	SaTimeT retentionDuration)
{
	SaCkptLibClientT* libClient = NULL;
	SaCkptLibRequestT* libRequest = NULL;
	SaCkptClientRequestT* clientRequest = NULL;
	SaCkptReqRtnParamT* rtnParam = NULL;
 	SaCkptClientResponseT* clientResponse = NULL;
	SaCkptLibCheckpointT* libCheckpoint = NULL;
	
	SaErrorT libError = SA_OK;
	IPC_Channel* ch = NULL;

	if (checkpointHandle == NULL) {
		cl_log(LOG_ERR, 
			"Null handle in checkpoint library");
		return SA_ERR_INVALID_PARAM;
	}

	libCheckpoint = SaCkptGetLibCheckpointByHandle(
		*checkpointHandle);
	if (libCheckpoint == NULL) {
		cl_log(LOG_ERR, 
			"Checkpoint is not open");
		return SA_ERR_INVALID_PARAM;
	}
	libClient = libCheckpoint->client;
	
  	libRequest = (SaCkptLibRequestT*)ha_malloc(
					sizeof(SaCkptLibRequestT));
	clientRequest = (SaCkptClientRequestT*)ha_malloc(
					sizeof(SaCkptClientRequestT));
	rtnParam = (SaCkptReqRtnParamT*)ha_malloc(
					sizeof(SaCkptReqRtnParamT));
 	if ((libRequest == NULL) ||
		(clientRequest == NULL) ||
		(rtnParam == NULL)) {
		cl_log(LOG_ERR, 
			"No memory in checkpoint library");
		libError = SA_ERR_NO_MEMORY;
		goto rtnError;
	}
	
	memset(libRequest, 0, sizeof(SaCkptLibRequestT));
	memset(clientRequest, 0, sizeof(SaCkptClientRequestT));
	memset(rtnParam, 0, sizeof(SaCkptReqRtnParamT));
	
	libRequest->client = libClient;
	libRequest->timeoutTag = 0;
	libRequest->clientRequest = clientRequest;

	clientRequest->clientHandle = libClient->clientHandle;
	clientRequest->requestNO = SaCkptLibGetReqNO();
	clientRequest->req = REQ_CKPT_RTN_SET;
	clientRequest->reqParamLength = sizeof(SaCkptReqRtnParamT);
	clientRequest->dataLength = 0;
	clientRequest->reqParam = rtnParam;
	clientRequest->data = NULL;

	rtnParam->checkpointHandle = *checkpointHandle;
	rtnParam->retention = retentionDuration;
	
	ch = libClient->channel[0];
	libError = SaCkptLibRequestSend(ch, libRequest->clientRequest);
	if (libError != SA_OK) {
		cl_log(LOG_ERR, 
			"Send retention duration failed");
		goto rtnError;
	}

	libError = SaCkptLibResponseReceive(ch, 
		libRequest->clientRequest->requestNO,
		&clientResponse);
	if (libError != SA_OK) {
		cl_log(LOG_ERR, 
			"Receive response failed");
		goto rtnError;
	}
	if (clientResponse == NULL) {
		cl_log(LOG_ERR, 
			"Received null response");
		libError = SA_ERR_LIBRARY;
		goto rtnError;
	}
	if (clientResponse->retVal != SA_OK) {
		cl_log(LOG_ERR, 
			"Checkpoint daemon returned error");
		libError = clientResponse->retVal;
		goto rtnError;
	}

	libCheckpoint->createAttributes.retentionDuration = retentionDuration;

	libError = SA_OK;

rtnError:
	
	if (libRequest != NULL) {
		ha_free(libRequest);
	}
	
	if (clientRequest != NULL) {
		ha_free(clientRequest);
	}
	
	if (rtnParam != NULL) {
		ha_free(rtnParam);
	}

	if (clientResponse != NULL) {
		if (clientResponse->dataLength > 0) {
			ha_free(clientResponse->data);
		}
		ha_free(clientResponse);
	}

	return libError; 
}

/* 
 * set the local replica as the active replica.
 */
SaErrorT
saCkptActiveCheckpointSet(const SaCkptCheckpointHandleT *checkpointHandle)
{
	SaCkptLibClientT* libClient = NULL;
	SaCkptLibRequestT* libRequest = NULL;
	SaCkptClientRequestT* clientRequest = NULL;
	SaCkptReqActSetParamT* activeParam = NULL;
 	SaCkptClientResponseT* clientResponse = NULL;
	SaCkptLibCheckpointT* libCheckpoint = NULL;
	
	SaErrorT libError = SA_OK;
	IPC_Channel* ch = NULL;

	if (checkpointHandle == NULL) {
		cl_log(LOG_ERR, 
			"Null handle in checkpoint library");
		return SA_ERR_INVALID_PARAM;
	}

	libCheckpoint = SaCkptGetLibCheckpointByHandle(
		*checkpointHandle);
	if (libCheckpoint == NULL) {
		cl_log(LOG_ERR, 
			"Checkpoint is not open");
		return SA_ERR_INVALID_PARAM;
	}
	libClient = libCheckpoint->client;
	
  	libRequest = (SaCkptLibRequestT*)ha_malloc(
					sizeof(SaCkptLibRequestT));
	clientRequest = (SaCkptClientRequestT*)ha_malloc(
					sizeof(SaCkptClientRequestT));
	activeParam = (SaCkptReqActSetParamT*)ha_malloc(
					sizeof(SaCkptReqActSetParamT));
 	if ((libRequest == NULL) ||
		(clientRequest == NULL) ||
		(activeParam == NULL)) {
		cl_log(LOG_ERR, 
			"No memory in checkpoint library");
		libError = SA_ERR_NO_MEMORY;
		goto activeError;
	}
	
	memset(libRequest, 0, sizeof(SaCkptLibRequestT));
	memset(clientRequest, 0, sizeof(SaCkptClientRequestT));
	memset(activeParam, 0, sizeof(SaCkptReqActSetParamT));
	
	libRequest->client = libClient;
	libRequest->timeoutTag = 0;
	libRequest->clientRequest = clientRequest;

	clientRequest->clientHandle = libClient->clientHandle;
	clientRequest->requestNO = SaCkptLibGetReqNO();
	clientRequest->req = REQ_CKPT_ACT_SET;
	clientRequest->reqParamLength = sizeof(SaCkptReqActSetParamT);
	clientRequest->dataLength = 0;
	clientRequest->reqParam = activeParam;
	clientRequest->data = NULL;

	activeParam->checkpointHandle = *checkpointHandle;
	
	ch = libClient->channel[0];
	libError = SaCkptLibRequestSend(ch, libRequest->clientRequest);
	if (libError != SA_OK) {
		cl_log(LOG_ERR, 
			"Send activate_checkpoint_set request failed");
		goto activeError;
	}

	libError = SaCkptLibResponseReceive(ch, 
		libRequest->clientRequest->requestNO,
		&clientResponse);
	if (libError != SA_OK) {
		cl_log(LOG_ERR, "Receive response failed");
		goto activeError;
	}
	if (clientResponse == NULL) {
		cl_log(LOG_ERR, "Received null response");
		libError = SA_ERR_LIBRARY;
		goto activeError;
	}
	if (clientResponse->retVal != SA_OK) {
		cl_log(LOG_ERR, "Checkpoint daemon returned error");
		libError = clientResponse->retVal;
		goto activeError;
	}

	libError = SA_OK;

activeError:
	
	if (libRequest != NULL) {
		ha_free(libRequest);
	}
	
	if (clientRequest != NULL) {
		ha_free(clientRequest);
	}
	
	if (activeParam != NULL) {
		ha_free(activeParam);
	}

	if (clientResponse != NULL) {
		if (clientResponse->dataLength > 0) {
			ha_free(clientResponse->data);
		}
		ha_free(clientResponse);
	}

	return libError; 
}

/* get the checkpoint status
 */
SaErrorT
saCkptCheckpointStatusGet(
	const SaCkptCheckpointHandleT *checkpointHandle,
	SaCkptCheckpointStatusT *checkpointStatus/*[out]*/)
{
	SaCkptLibClientT* libClient = NULL;
	SaCkptLibRequestT* libRequest = NULL;
	SaCkptClientRequestT* clientRequest = NULL;
	SaCkptReqStatGetParamT* statParam = NULL;
 	SaCkptClientResponseT* clientResponse = NULL;
	SaCkptLibCheckpointT* libCheckpoint = NULL;
	
	SaErrorT libError = SA_OK;
	IPC_Channel* ch = NULL;

	if (checkpointHandle == NULL) {
		cl_log(LOG_ERR, 
			"Null handle in checkpoint library");
		return SA_ERR_INVALID_PARAM;
	}

	libCheckpoint = SaCkptGetLibCheckpointByHandle(
		*checkpointHandle);
	if (libCheckpoint == NULL) {
		cl_log(LOG_ERR, 
			"Checkpoint is not open");
		return SA_ERR_INVALID_PARAM;
	}
	libClient = libCheckpoint->client;
	
  	libRequest = (SaCkptLibRequestT*)ha_malloc(
					sizeof(SaCkptLibRequestT));
	clientRequest = (SaCkptClientRequestT*)ha_malloc(
					sizeof(SaCkptClientRequestT));
	statParam = (SaCkptReqStatGetParamT*)ha_malloc(
					sizeof(SaCkptReqStatGetParamT));
 	if ((libRequest == NULL) ||
		(clientRequest == NULL) ||
		(statParam == NULL)) {
		cl_log(LOG_ERR, 
			"No memory in checkpoint library");
		libError = SA_ERR_NO_MEMORY;
		goto statError;
	}
	
	memset(libRequest, 0, sizeof(SaCkptLibRequestT));
	memset(clientRequest, 0, sizeof(SaCkptClientRequestT));
	memset(statParam, 0, sizeof(SaCkptReqStatGetParamT));
	
	libRequest->client = libClient;
	libRequest->timeoutTag = 0;
	libRequest->clientRequest = clientRequest;

	clientRequest->clientHandle = libClient->clientHandle;
	clientRequest->requestNO = SaCkptLibGetReqNO();
	clientRequest->req = REQ_CKPT_STAT_GET;
	clientRequest->reqParamLength = sizeof(SaCkptReqStatGetParamT);
	clientRequest->dataLength = 0;
	clientRequest->reqParam = statParam;
	clientRequest->data = NULL;

	statParam->checkpointHandle = *checkpointHandle;
	
	ch = libClient->channel[0];
	libError = SaCkptLibRequestSend(ch, libRequest->clientRequest);
	if (libError != SA_OK) {
		cl_log(LOG_ERR, 
			"Send status_get request failed");
		goto statError;
	}

	libError = SaCkptLibResponseReceive(ch, 
		libRequest->clientRequest->requestNO,
		&clientResponse);
	if (libError != SA_OK) {
		cl_log(LOG_ERR, 
			"Receive response failed");
		goto statError;
	}
	if (clientResponse == NULL) {
		cl_log(LOG_ERR, 
			"Received null response");
		libError = SA_ERR_LIBRARY;
		goto statError;
	}
	if (clientResponse->retVal != SA_OK) {
		cl_log(LOG_ERR, 
			"Checkpoint daemon returned error");
		libError = clientResponse->retVal;
		goto statError;
	}

	if (clientResponse->dataLength < 
		sizeof(SaCkptCheckpointStatusT)) {
		cl_log(LOG_ERR, 
			"Checkpoint daemon returned error data");
		libError = clientResponse->retVal;
		goto statError;
	}

	memcpy(checkpointStatus, clientResponse->data,
		clientResponse->dataLength);

	libError = SA_OK;

statError:
	
	if (libRequest != NULL) {
		ha_free(libRequest);
	}
	
	if (clientRequest != NULL) {
		ha_free(clientRequest);
	}
	
	if (statParam != NULL) {
		ha_free(statParam);
	}

	if (clientResponse != NULL) {
		if (clientResponse->dataLength > 0) {
			ha_free(clientResponse->data);
		}
		ha_free(clientResponse);
	}

	return libError; 
}

/* create a new section within the checkpoint and fill with the initial data.
 */
SaErrorT
saCkptSectionCreate(
	const SaCkptCheckpointHandleT *checkpointHandle,
	SaCkptSectionCreationAttributesT *sectionCreationAttributes,
	const void *initialData,
	SaUint32T initialDataSize)
{
	SaCkptLibClientT* libClient = NULL;
	SaCkptLibRequestT* libRequest = NULL;
	SaCkptClientRequestT* clientRequest = NULL;
	SaCkptReqSecCrtParamT* secCrtParam = NULL;
 	SaCkptClientResponseT* clientResponse = NULL;
	SaCkptLibCheckpointT* libCheckpoint = NULL;
	
	SaErrorT libError = SA_OK;
	IPC_Channel* ch = NULL;

	SaCkptSectionIdT* sectionId = NULL;
	void* data = NULL;

	if (checkpointHandle == NULL) {
		cl_log(LOG_ERR, 
			"Null handle in checkpoint library");
		return SA_ERR_INVALID_PARAM;
	}

	libCheckpoint = SaCkptGetLibCheckpointByHandle(
		*checkpointHandle);
	if (libCheckpoint == NULL) {
		cl_log(LOG_ERR, 
			"Checkpoint is not open");
		return SA_ERR_INVALID_PARAM;
	}
	if (!(libCheckpoint->openFlag & SA_CKPT_CHECKPOINT_WRITE)) {
		cl_log(LOG_ERR, 
			"Checkpoint is not opened for write");
		return SA_ERR_LIBRARY;
	}
	libClient = libCheckpoint->client;
	
  	libRequest = (SaCkptLibRequestT*)ha_malloc(
					sizeof(SaCkptLibRequestT));
	clientRequest = (SaCkptClientRequestT*)ha_malloc(
					sizeof(SaCkptClientRequestT));
	secCrtParam = (SaCkptReqSecCrtParamT*)ha_malloc(
					sizeof(SaCkptReqSecCrtParamT));
	if (initialDataSize > 0) {
		data = (void*)ha_malloc(initialDataSize);
	}
	
 	if ((libRequest == NULL) ||
		(clientRequest == NULL) ||
		(secCrtParam == NULL) ||
		((initialDataSize > 0) && (data == NULL))) {
		cl_log(LOG_ERR, 
			"No memory in checkpoint library");
		libError = SA_ERR_NO_MEMORY;
		goto secCrtError;
	}
	
	memset(libRequest, 0, sizeof(SaCkptLibRequestT));
	memset(clientRequest, 0, sizeof(SaCkptClientRequestT));
	memset(secCrtParam, 0, sizeof(SaCkptReqSecCrtParamT));
	memcpy(data, initialData, initialDataSize);
	
	libRequest->client = libClient;
	libRequest->timeoutTag = 0;
	libRequest->clientRequest = clientRequest;

	clientRequest->clientHandle = libClient->clientHandle;
	clientRequest->requestNO = SaCkptLibGetReqNO();
	clientRequest->req = REQ_SEC_CRT;
	clientRequest->reqParamLength = sizeof(SaCkptReqSecCrtParamT);
	clientRequest->dataLength = initialDataSize;
	clientRequest->reqParam = secCrtParam;
	clientRequest->data = data;

	secCrtParam->checkpointHandle = *checkpointHandle;
	secCrtParam->expireTime = 
		sectionCreationAttributes->expirationTime;
	secCrtParam->sectionID.idLen = 
		sectionCreationAttributes->sectionId->idLen;
	memcpy(secCrtParam->sectionID.id,
		sectionCreationAttributes->sectionId->id,
		sectionCreationAttributes->sectionId->idLen);
	
	ch = libClient->channel[0];
	libError = SaCkptLibRequestSend(ch, libRequest->clientRequest);
	if (libError != SA_OK) {
		cl_log(LOG_ERR, 
			"Send section_create request failed");
		goto secCrtError;
	}

	libError = SaCkptLibResponseReceive(ch, 
		libRequest->clientRequest->requestNO,
		&clientResponse);
	if (libError != SA_OK) {
		cl_log(LOG_ERR, 
			"Receive response failed");
		goto secCrtError;
	}
	if (clientResponse == NULL) {
		cl_log(LOG_ERR, 
			"Received null response");
		libError = SA_ERR_LIBRARY;
		goto secCrtError;
	}
	if (clientResponse->retVal != SA_OK) {
		cl_log(LOG_ERR, 
			"Checkpoint daemon returned error");
		libError = clientResponse->retVal;
		goto secCrtError;
	}

	/* return the section ID */
	if (clientResponse->dataLength > 0) {
		sectionId = (SaCkptSectionIdT*)clientResponse->data;
		sectionCreationAttributes->sectionId->idLen = 
			sectionId->idLen;
		memcpy(sectionCreationAttributes->sectionId->id,
			sectionId->id, sectionId->idLen);
	}

	libError = SA_OK;

secCrtError:

	if (data != NULL) {
		ha_free(data);
	}
	
	if (libRequest != NULL) {
		ha_free(libRequest);
	}
	
	if (clientRequest != NULL) {
		ha_free(clientRequest);
	}
	
	if (secCrtParam != NULL) {
		ha_free(secCrtParam);
	}

	if (clientResponse != NULL) {
		if (clientResponse->dataLength > 0) {
			ha_free(clientResponse->data);
		}
		ha_free(clientResponse);
	}

	return libError; 
	
}

/* 
 * delete the section within the checkpoint.
 */
SaErrorT
saCkptSectionDelete(
	const SaCkptCheckpointHandleT *checkpointHandle,
	const SaCkptSectionIdT *sectionId)
{
	SaCkptLibClientT* libClient = NULL;
	SaCkptLibRequestT* libRequest = NULL;
	SaCkptClientRequestT* clientRequest = NULL;
	SaCkptReqSecDelParamT* secDelParam = NULL;
 	SaCkptClientResponseT* clientResponse = NULL;
	SaCkptLibCheckpointT* libCheckpoint = NULL;
	
	SaErrorT libError = SA_OK;
	IPC_Channel* ch = NULL;

	if (checkpointHandle == NULL) {
		cl_log(LOG_ERR, 
			"Null handle in checkpoint library");
		return SA_ERR_INVALID_PARAM;
	}

	libCheckpoint = SaCkptGetLibCheckpointByHandle(
		*checkpointHandle);
	if (libCheckpoint == NULL) {
		cl_log(LOG_ERR, 
			"Checkpoint is not open");
		return SA_ERR_INVALID_PARAM;
	}
	if (!(libCheckpoint->openFlag & SA_CKPT_CHECKPOINT_WRITE)) {
		cl_log(LOG_ERR, 
			"Checkpoint is not opened for write");
		return SA_ERR_LIBRARY;
	}
	libClient = libCheckpoint->client;
	
  	libRequest = (SaCkptLibRequestT*)ha_malloc(
					sizeof(SaCkptLibRequestT));
	clientRequest = (SaCkptClientRequestT*)ha_malloc(
					sizeof(SaCkptClientRequestT));
	secDelParam = (SaCkptReqSecDelParamT*)ha_malloc(
					sizeof(SaCkptReqSecDelParamT));
	
 	if ((libRequest == NULL) ||
		(clientRequest == NULL) ||
		(secDelParam == NULL) ) {
		cl_log(LOG_ERR, 
			"No memory in checkpoint library");
		libError = SA_ERR_NO_MEMORY;
		goto secDelError;
	}
	
	memset(libRequest, 0, sizeof(SaCkptLibRequestT));
	memset(clientRequest, 0, sizeof(SaCkptClientRequestT));
	memset(secDelParam, 0, sizeof(SaCkptReqSecDelParamT));
	
	libRequest->client = libClient;
	libRequest->timeoutTag = 0;
	libRequest->clientRequest = clientRequest;

	clientRequest->clientHandle = libClient->clientHandle;
	clientRequest->requestNO = SaCkptLibGetReqNO();
	clientRequest->req = REQ_SEC_DEL;
	clientRequest->reqParamLength = sizeof(SaCkptReqSecDelParamT);
	clientRequest->dataLength = 0;
	clientRequest->reqParam = secDelParam;
	clientRequest->data = NULL;

	secDelParam->checkpointHandle = *checkpointHandle;
	secDelParam->sectionID.idLen = sectionId->idLen;
	memcpy(secDelParam->sectionID.id, sectionId->id,
		sectionId->idLen);
	
	ch = libClient->channel[0];
	libError = SaCkptLibRequestSend(ch, libRequest->clientRequest);
	if (libError != SA_OK) {
		cl_log(LOG_ERR, 
			"Send section_delete request failed");
		goto secDelError;
	}

	libError = SaCkptLibResponseReceive(ch, 
		libRequest->clientRequest->requestNO,
		&clientResponse);
	if (libError != SA_OK) {
		cl_log(LOG_ERR, 
			"Receive response failed");
		goto secDelError;
	}
	if (clientResponse == NULL) {
		cl_log(LOG_ERR, 
			"Received null response");
		libError = SA_ERR_LIBRARY;
		goto secDelError;
	}
	if (clientResponse->retVal != SA_OK) {
		cl_log(LOG_ERR, 
			"Checkpoint daemon returned error");
		libError = clientResponse->retVal;
		goto secDelError;
	}

	libError = SA_OK;

secDelError:

	if (libRequest != NULL) {
		ha_free(libRequest);
	}
	
	if (clientRequest != NULL) {
		ha_free(clientRequest);
	}
	
	if (secDelParam != NULL) {
		ha_free(secDelParam);
	}

	if (clientResponse != NULL) {
		if (clientResponse->dataLength > 0) {
			ha_free(clientResponse->data);
		}
		ha_free(clientResponse);
	}

	return libError; 
}

/* set the section expiration time
 */
SaErrorT
saCkptSectionExpirationTimeSet(
	const SaCkptCheckpointHandleT *checkpointHandle,
	const SaCkptSectionIdT* sectionId,
	SaTimeT expirationTime)
{
	SaCkptLibClientT* libClient = NULL;
	SaCkptLibRequestT* libRequest = NULL;
	SaCkptClientRequestT* clientRequest = NULL;
	SaCkptReqSecExpSetParamT* secExpSetParam = NULL;
 	SaCkptClientResponseT* clientResponse = NULL;
	SaCkptLibCheckpointT* libCheckpoint = NULL;
	
	SaErrorT libError = SA_OK;
	IPC_Channel* ch = NULL;

	if (checkpointHandle == NULL) {
		cl_log(LOG_ERR, 
			"Null handle in checkpoint library");
		return SA_ERR_INVALID_PARAM;
	}

	libCheckpoint = SaCkptGetLibCheckpointByHandle(
		*checkpointHandle);
	if (libCheckpoint == NULL) {
		cl_log(LOG_ERR, 
			"Checkpoint is not open");
		return SA_ERR_INVALID_PARAM;
	}
	libClient = libCheckpoint->client;
	
  	libRequest = (SaCkptLibRequestT*)ha_malloc(
					sizeof(SaCkptLibRequestT));
	clientRequest = (SaCkptClientRequestT*)ha_malloc(
					sizeof(SaCkptClientRequestT));
	secExpSetParam = (SaCkptReqSecExpSetParamT*)ha_malloc(
					sizeof(SaCkptReqSecExpSetParamT));
	
 	if ((libRequest == NULL) ||
		(clientRequest == NULL) ||
		(secExpSetParam == NULL) ) {
		cl_log(LOG_ERR, 
			"No memory in checkpoint library");
		libError = SA_ERR_NO_MEMORY;
		goto secExpSetError;
	}
	
	memset(libRequest, 0, sizeof(SaCkptLibRequestT));
	memset(clientRequest, 0, sizeof(SaCkptClientRequestT));
	memset(secExpSetParam, 0, sizeof(SaCkptReqSecExpSetParamT));
	
	libRequest->client = libClient;
	libRequest->timeoutTag = 0;
	libRequest->clientRequest = clientRequest;

	clientRequest->clientHandle = libClient->clientHandle;
	clientRequest->requestNO = SaCkptLibGetReqNO();
	clientRequest->req = REQ_SEC_EXP_SET;
	clientRequest->reqParamLength = sizeof(SaCkptReqSecExpSetParamT);
	clientRequest->dataLength = 0;
	clientRequest->reqParam = secExpSetParam;
	clientRequest->data = NULL;

	secExpSetParam->checkpointHandle = *checkpointHandle;
	secExpSetParam->sectionID.idLen = sectionId->idLen;
	memcpy(secExpSetParam->sectionID.id, sectionId->id,
		sectionId->idLen);
	secExpSetParam->expireTime = expirationTime;
	
	ch = libClient->channel[0];
	libError = SaCkptLibRequestSend(ch, libRequest->clientRequest);
	if (libError != SA_OK) {
		cl_log(LOG_ERR, 
			"Send section_expiration_set request failed");
		goto secExpSetError;
	}

	libError = SaCkptLibResponseReceive(ch, 
		libRequest->clientRequest->requestNO,
		&clientResponse);
	if (libError != SA_OK) {
		cl_log(LOG_ERR, 
			"Receive response failed");
		goto secExpSetError;
	}
	if (clientResponse == NULL) {
		cl_log(LOG_ERR, 
			"Received null response");
		libError = SA_ERR_LIBRARY;
		goto secExpSetError;
	}
	if (clientResponse->retVal != SA_OK) {
		cl_log(LOG_ERR, 
			"Checkpoint daemon returned error");
		libError = clientResponse->retVal;
		goto secExpSetError;
	}

	libError = SA_OK;

secExpSetError:

	if (libRequest != NULL) {
		ha_free(libRequest);
	}
	
	if (clientRequest != NULL) {
		ha_free(clientRequest);
	}
	
	if (secExpSetParam != NULL) {
		ha_free(secExpSetParam);
	}

	if (clientResponse != NULL) {
		if (clientResponse->dataLength > 0) {
			ha_free(clientResponse->data);
		}
		ha_free(clientResponse);
	}

	return libError; 
}

/* 
 * initialize the section interator
 */
SaErrorT
saCkptSectionIteratorInitialize(
	const SaCkptCheckpointHandleT *checkpointHandle,
	SaCkptSectionsChosenT sectionsChosen,
	SaTimeT expirationTime,
	SaCkptSectionIteratorT *sectionIterator/*[out]*/)
{
	SaCkptLibClientT* libClient = NULL;
	SaCkptLibRequestT* libRequest = NULL;
	SaCkptClientRequestT* clientRequest = NULL;
	SaCkptReqSecQueryParamT* secQueryParam = NULL;
 	SaCkptClientResponseT* clientResponse = NULL;
	SaCkptLibCheckpointT* libCheckpoint = NULL;
	
	SaErrorT libError = SA_OK;
	IPC_Channel* ch = NULL;

	GList* sectionList = NULL;
	SaCkptSectionDescriptorT* sectionDescriptor = NULL;
	int sectionNumber = 0;
	int i = 0;
	char* p = NULL;

	if (checkpointHandle == NULL) {
		cl_log(LOG_ERR, 
			"Null handle in checkpoint library");
		return SA_ERR_INVALID_PARAM;
	}

	libCheckpoint = SaCkptGetLibCheckpointByHandle(
		*checkpointHandle);
	if (libCheckpoint == NULL) {
		cl_log(LOG_ERR, 
			"Checkpoint is not open");
		return SA_ERR_INVALID_PARAM;
	}
	libClient = libCheckpoint->client;
	
  	libRequest = (SaCkptLibRequestT*)ha_malloc(
					sizeof(SaCkptLibRequestT));
	clientRequest = (SaCkptClientRequestT*)ha_malloc(
					sizeof(SaCkptClientRequestT));
	secQueryParam = (SaCkptReqSecQueryParamT*)ha_malloc(
					sizeof(SaCkptReqSecQueryParamT));
	
 	if ((libRequest == NULL) ||
		(clientRequest == NULL) ||
		(secQueryParam == NULL) ) {
		cl_log(LOG_ERR, 
			"No memory in checkpoint library");
		libError = SA_ERR_NO_MEMORY;
		goto secQueryError;
	}
	
	memset(libRequest, 0, sizeof(SaCkptLibRequestT));
	memset(clientRequest, 0, sizeof(SaCkptClientRequestT));
	memset(secQueryParam, 0, sizeof(SaCkptReqSecQueryParamT));
	
	libRequest->client = libClient;
	libRequest->timeoutTag = 0;
	libRequest->clientRequest = clientRequest;

	clientRequest->clientHandle = libClient->clientHandle;
	clientRequest->requestNO = SaCkptLibGetReqNO();
	clientRequest->req = REQ_SEC_QUERY;
	clientRequest->reqParamLength = sizeof(SaCkptReqSecQueryParamT);
	clientRequest->dataLength = 0;
	clientRequest->reqParam = secQueryParam;
	clientRequest->data = NULL;

	secQueryParam->checkpointHandle = *checkpointHandle;
	secQueryParam->chosenFlag = sectionsChosen;
	secQueryParam->expireTime = expirationTime;
	
	ch = libClient->channel[0];
	libError = SaCkptLibRequestSend(ch, libRequest->clientRequest);
	if (libError != SA_OK) {
		cl_log(LOG_ERR, 
			"Send iterator_init request failed");
		goto secQueryError;
	}

	libError = SaCkptLibResponseReceive(ch, 
		libRequest->clientRequest->requestNO,
		&clientResponse);
	if (libError != SA_OK) {
		cl_log(LOG_ERR, 
			"Receive response failed");
		goto secQueryError;
	}
	if (clientResponse == NULL) {
		cl_log(LOG_ERR, 
			"Received null response");
		libError = SA_ERR_LIBRARY;
		goto secQueryError;
	}
	if (clientResponse->retVal != SA_OK) {
		cl_log(LOG_ERR, 
			"Checkpoint daemon returned error");
		libError = clientResponse->retVal;
		goto secQueryError;
	}

	*sectionIterator = SaCkptLibGetIterator();
	
	sectionNumber = clientResponse->dataLength / 
		sizeof(SaCkptSectionDescriptorT);
	p = clientResponse->data;
	
	for(i=0; i<sectionNumber; i++) {
		sectionDescriptor = (SaCkptSectionDescriptorT*)p;
		sectionList = g_list_append(sectionList, sectionDescriptor);
		p += sizeof(SaCkptSectionDescriptorT);
	}
	g_hash_table_insert(libIteratorHash, sectionIterator, sectionList);

	libError = SA_OK;

secQueryError:

	if (libRequest != NULL) {
		ha_free(libRequest);
	}
	
	if (clientRequest != NULL) {
		ha_free(clientRequest);
	}
	
	if (secQueryParam != NULL) {
		ha_free(secQueryParam);
	}

	if (clientResponse != NULL) {
		ha_free(clientResponse);
	}

	return libError; 
}

/* get the next section in the iterator
 */
SaErrorT
saCkptSectionIteratorNext(
	SaCkptSectionIteratorT *sectionIterator/*[in/out]*/,
	SaCkptSectionDescriptorT *sectionDescriptor/*[out]*/)
{
	SaCkptSectionDescriptorT* secDescriptor = NULL;
	GList* sectionList = NULL;

	sectionList = g_hash_table_lookup(libIteratorHash, 
		sectionIterator);
	if (sectionList == NULL) {
		/* FIXME: should be SA_ERR_NO_SECTIONS */
		return SA_OK;
	}

	secDescriptor = (SaCkptSectionDescriptorT*)sectionList->data;
	memcpy(sectionDescriptor, secDescriptor, 
		sizeof(SaCkptSectionDescriptorT));
	ha_free(secDescriptor);
	sectionList = g_list_remove(sectionList, secDescriptor);
	
	g_hash_table_insert(libIteratorHash, sectionIterator,
		sectionList);

	return SA_OK;
}

/* release the resource allocated to this iterator, sectionIterator should be
 * that returned by saCkptSectionIteratorInitialize
 */
SaErrorT
saCkptSectionIteratorFinalize(
	SaCkptSectionIteratorT *sectionIterator)
{
	SaCkptSectionDescriptorT* secDescriptor = NULL;
	GList* sectionList = NULL;

	sectionList = g_hash_table_lookup(libIteratorHash, 
		sectionIterator);

	while (sectionList != NULL) {
		secDescriptor = 
			(SaCkptSectionDescriptorT*)sectionList->data;
		ha_free(secDescriptor);
		sectionList = g_list_remove(sectionList, secDescriptor);
	}
	
	g_hash_table_remove(libIteratorHash, sectionIterator);

	return SA_OK;
}

static SaErrorT
saCkptCheckpointSectionWrite(
	SaCkptReqSecWrtParamT *wrtParam,
	SaUint32T dataLength, void* data)
{
	SaCkptCheckpointHandleT* checkpointHandle = NULL;
	SaCkptLibClientT* libClient = NULL;
	SaCkptLibRequestT* libRequest = NULL;
	SaCkptClientRequestT* clientRequest = NULL;
 	SaCkptClientResponseT* clientResponse = NULL;
	SaCkptLibCheckpointT* libCheckpoint = NULL;
	
	SaErrorT libError = SA_OK;
	IPC_Channel* ch = NULL;

	checkpointHandle = &wrtParam->checkpointHandle;
	libCheckpoint = SaCkptGetLibCheckpointByHandle(
		*checkpointHandle);
	libClient = libCheckpoint->client;
	
  	libRequest = (SaCkptLibRequestT*)ha_malloc(
					sizeof(SaCkptLibRequestT));
	clientRequest = (SaCkptClientRequestT*)ha_malloc(
					sizeof(SaCkptClientRequestT));
 	if ((libRequest == NULL) ||
		(clientRequest == NULL) ) {
		cl_log(LOG_ERR, 
			"No memory in checkpoint library");
		libError = SA_ERR_NO_MEMORY;
		goto secWrtError;
	}
	
	memset(libRequest, 0, sizeof(SaCkptLibRequestT));
	memset(clientRequest, 0, sizeof(SaCkptClientRequestT));
	
	libRequest->client = libClient;
	libRequest->timeoutTag = 0;
	libRequest->clientRequest = clientRequest;

	clientRequest->clientHandle = libClient->clientHandle;
	clientRequest->requestNO = SaCkptLibGetReqNO();
	clientRequest->req = REQ_SEC_WRT;
	clientRequest->reqParamLength = sizeof(SaCkptReqSecWrtParamT);
	clientRequest->dataLength = dataLength;
	clientRequest->reqParam = wrtParam;
	clientRequest->data = data;

	ch = libClient->channel[0];
	libError = SaCkptLibRequestSend(ch, libRequest->clientRequest);
	if (libError != SA_OK) {
		cl_log(LOG_ERR, 
			"Send section_write request failed");
		goto secWrtError;
	}

	libError = SaCkptLibResponseReceive(ch, 
		libRequest->clientRequest->requestNO,
		&clientResponse);
	if (libError != SA_OK) {
		cl_log(LOG_ERR, 
			"Receive response failed");
		goto secWrtError;
	}
	if (clientResponse == NULL) {
		cl_log(LOG_ERR, 
			"Received null response");
		libError = SA_ERR_LIBRARY;
		goto secWrtError;
	}
	if (clientResponse->retVal != SA_OK) {
		cl_log(LOG_ERR, 
			"Checkpoint daemon returned error");
		libError = clientResponse->retVal;
		goto secWrtError;
	}

	libError = SA_OK;

secWrtError:
	
	if (libRequest != NULL) {
		ha_free(libRequest);
	}
	
	if (clientRequest != NULL) {
		ha_free(clientRequest);
	}
	
	if (clientResponse != NULL) {
		if (clientResponse->dataLength > 0) {
			ha_free(clientResponse->data);
		}
		ha_free(clientResponse);
	}

	return libError;
}

static SaErrorT
saCkptCheckpointSectionRead(
	SaCkptReqSecReadParamT *readParam,
	SaSizeT* dataLength, void* data)
{
	SaCkptCheckpointHandleT* checkpointHandle = NULL;
	SaCkptLibClientT* libClient = NULL;
	SaCkptLibRequestT* libRequest = NULL;
	SaCkptClientRequestT* clientRequest = NULL;
 	SaCkptClientResponseT* clientResponse = NULL;
	SaCkptLibCheckpointT* libCheckpoint = NULL;
	
	SaErrorT libError = SA_OK;
	IPC_Channel* ch = NULL;

	checkpointHandle = &readParam->checkpointHandle;
	libCheckpoint = SaCkptGetLibCheckpointByHandle(
		*checkpointHandle);
	libClient = libCheckpoint->client;
	
  	libRequest = (SaCkptLibRequestT*)ha_malloc(
					sizeof(SaCkptLibRequestT));
	clientRequest = (SaCkptClientRequestT*)ha_malloc(
					sizeof(SaCkptClientRequestT));
 	if ((libRequest == NULL) ||
		(clientRequest == NULL) ) {
		cl_log(LOG_ERR, 
			"No memory in checkpoint library");
		libError = SA_ERR_NO_MEMORY;
		goto secReadError;
	}
	
	memset(libRequest, 0, sizeof(SaCkptLibRequestT));
	memset(clientRequest, 0, sizeof(SaCkptClientRequestT));
	
	libRequest->client = libClient;
	libRequest->timeoutTag = 0;
	libRequest->clientRequest = clientRequest;

	clientRequest->clientHandle = libClient->clientHandle;
	clientRequest->requestNO = SaCkptLibGetReqNO();
	clientRequest->req = REQ_SEC_RD;
	clientRequest->reqParamLength = sizeof(SaCkptReqSecReadParamT);
	clientRequest->dataLength = 0;
	clientRequest->reqParam = readParam;
	clientRequest->data = NULL;

	ch = libClient->channel[0];
	libError = SaCkptLibRequestSend(ch, libRequest->clientRequest);
	if (libError != SA_OK) {
		cl_log(LOG_ERR, 
			"Send section read request failed");
		goto secReadError;
	}

	libError = SaCkptLibResponseReceive(ch, 
		libRequest->clientRequest->requestNO,
		&clientResponse);
	if (libError != SA_OK) {
		cl_log(LOG_ERR, 
			"Receive response failed");
		goto secReadError;
	}
	if (clientResponse == NULL) {
		cl_log(LOG_ERR, 
			"Received null response");
		libError = SA_ERR_LIBRARY;
		goto secReadError;
	}
	if (clientResponse->retVal != SA_OK) {
		cl_log(LOG_ERR, 
			"Checkpoint daemon returned error");
		libError = clientResponse->retVal;
		goto secReadError;
	}

	*dataLength = clientResponse->dataLength;
	memcpy(data, clientResponse->data,
		clientResponse->dataLength);

	libError = SA_OK;

secReadError:
	
	if (libRequest != NULL) {
		ha_free(libRequest);
	}
	
	if (clientRequest != NULL) {
		ha_free(clientRequest);
	}
	
	if (clientResponse != NULL) {
		if (clientResponse->dataLength > 0) {
			ha_free(clientResponse->data);
		}
		ha_free(clientResponse);
	}

	return libError;
}

SaErrorT
saCkptCheckpointWrite(
	const SaCkptCheckpointHandleT *checkpointHandle,
	const SaCkptIOVectorElementT *ioVector,
	SaUint32T numberOfElements,
	SaUint32T *erroneousVectorIndex)
{
	SaCkptLibCheckpointT* libCheckpoint = NULL;
	SaCkptReqSecWrtParamT* wrtParam = NULL;
	
	SaErrorT libError = SA_OK;
	int i = 0;

	if (checkpointHandle == NULL) {
		cl_log(LOG_ERR, 
			"Null handle in checkpoint library");
		return SA_ERR_INVALID_PARAM;
	}

	libCheckpoint = SaCkptGetLibCheckpointByHandle(
		*checkpointHandle);
	if (libCheckpoint == NULL) {
		cl_log(LOG_ERR, 
			"Checkpoint is not open");
		return SA_ERR_INVALID_PARAM;
	}
	if (!(libCheckpoint->openFlag & SA_CKPT_CHECKPOINT_WRITE)) {
		cl_log(LOG_ERR, 
			"Checkpoint is not opened for write");
		return SA_ERR_LIBRARY;
	}
	
	wrtParam = (SaCkptReqSecWrtParamT*)ha_malloc(
		sizeof(SaCkptReqSecWrtParamT));
	if (wrtParam == NULL) {
		cl_log(LOG_ERR, 
			"No memory in checkpoint library");
		return SA_ERR_NO_MEMORY;
	}

	for(i=0; i<numberOfElements; i++) {
		memset(wrtParam, 0, sizeof(SaCkptReqSecWrtParamT));
		wrtParam->checkpointHandle = *checkpointHandle;
		wrtParam->sectionID.idLen= ioVector[i].sectionId.idLen;
		memcpy(wrtParam->sectionID.id,
			ioVector[i].sectionId.id,
			ioVector[i].sectionId.idLen);
		wrtParam->offset = ioVector[i].dataOffset;

		libError = saCkptCheckpointSectionWrite(wrtParam,
			ioVector[i].dataSize, 
			ioVector[i].dataBuffer);
		if (libError != SA_OK) {
			if (erroneousVectorIndex != NULL) {
				*erroneousVectorIndex = i;
			}
			break;
		}
	}

	ha_free(wrtParam);

	return libError;
}


/* overwrite the section
 */
SaErrorT
saCkptSectionOverwrite(
	const SaCkptCheckpointHandleT *checkpointHandle,
	const SaCkptSectionIdT *sectionId,
	SaUint8T *dataBuffer,
	SaSizeT dataSize)
{
	SaCkptLibClientT* libClient = NULL;
	SaCkptLibRequestT* libRequest = NULL;
	SaCkptClientRequestT* clientRequest = NULL;
	SaCkptReqSecOwrtParamT* secOwrtParam = NULL;
 	SaCkptClientResponseT* clientResponse = NULL;
	SaCkptLibCheckpointT* libCheckpoint = NULL;
	
	SaErrorT libError = SA_OK;
	IPC_Channel* ch = NULL;

	if (checkpointHandle == NULL) {
		cl_log(LOG_ERR, 
			"Null handle in checkpoint library");
		return SA_ERR_INVALID_PARAM;
	}

	libCheckpoint = SaCkptGetLibCheckpointByHandle(
		*checkpointHandle);
	if (libCheckpoint == NULL) {
		cl_log(LOG_ERR, 
			"Checkpoint is not open");
		return SA_ERR_INVALID_PARAM;
	}
	if (!(libCheckpoint->openFlag & SA_CKPT_CHECKPOINT_WRITE)) {
		cl_log(LOG_ERR, 
			"Checkpoint is not opened for write");
		return SA_ERR_LIBRARY;
	}
	libClient = libCheckpoint->client;
	
  	libRequest = (SaCkptLibRequestT*)ha_malloc(
					sizeof(SaCkptLibRequestT));
	clientRequest = (SaCkptClientRequestT*)ha_malloc(
					sizeof(SaCkptClientRequestT));
	secOwrtParam = (SaCkptReqSecOwrtParamT*)ha_malloc(
					sizeof(SaCkptReqSecOwrtParamT));
	
 	if ((libRequest == NULL) ||
		(clientRequest == NULL) ||
		(secOwrtParam == NULL) ) {
		cl_log(LOG_ERR, 
			"No memory in checkpoint library");
		libError = SA_ERR_NO_MEMORY;
		goto secOwrtError;
	}
	
	memset(libRequest, 0, sizeof(SaCkptLibRequestT));
	memset(clientRequest, 0, sizeof(SaCkptClientRequestT));
	memset(secOwrtParam, 0, sizeof(SaCkptReqSecOwrtParamT));
	
	libRequest->client = libClient;
	libRequest->timeoutTag = 0;
	libRequest->clientRequest = clientRequest;

	clientRequest->clientHandle = libClient->clientHandle;
	clientRequest->requestNO = SaCkptLibGetReqNO();
	clientRequest->req = REQ_SEC_OWRT;
	clientRequest->reqParamLength = sizeof(SaCkptReqSecOwrtParamT);
	clientRequest->dataLength = dataSize;
	clientRequest->reqParam = secOwrtParam;
	clientRequest->data = dataBuffer;

	secOwrtParam->checkpointHandle = *checkpointHandle;
	secOwrtParam->sectionID.idLen = sectionId->idLen;
	memcpy(secOwrtParam->sectionID.id, sectionId->id,
		sectionId->idLen);
	
	ch = libClient->channel[0];
	libError = SaCkptLibRequestSend(ch, libRequest->clientRequest);
	if (libError != SA_OK) {
		cl_log(LOG_ERR, 
			"Send section_overwrite request failed");
		goto secOwrtError;
	}

	libError = SaCkptLibResponseReceive(ch, 
		libRequest->clientRequest->requestNO,
		&clientResponse);
	if (libError != SA_OK) {
		cl_log(LOG_ERR, 
			"Receive response failed");
		goto secOwrtError;
	}
	if (clientResponse == NULL) {
		cl_log(LOG_ERR, 
			"Received null response");
		libError = SA_ERR_LIBRARY;
		goto secOwrtError;
	}
	if (clientResponse->retVal != SA_OK) {
		cl_log(LOG_ERR, 
			"Checkpoint daemon returned error");
		libError = clientResponse->retVal;
		goto secOwrtError;
	}

	libError = SA_OK;

secOwrtError:

	if (libRequest != NULL) {
		ha_free(libRequest);
	}
	
	if (clientRequest != NULL) {
		ha_free(clientRequest);
	}
	
	if (secOwrtParam != NULL) {
		ha_free(secOwrtParam);
	}

	if (clientResponse != NULL) {
		if (clientResponse->dataLength > 0) {
			ha_free(clientResponse->data);
		}
		ha_free(clientResponse);
	}

	return libError; 
}

/* 
 * read multiple section data from checkpoint.
 */
SaErrorT
saCkptCheckpointRead(
	const SaCkptCheckpointHandleT *checkpointHandle,
	SaCkptIOVectorElementT *ioVector,
	SaUint32T numberOfElements,
	SaUint32T *erroneousVectorIndex/*[out]*/)
{
	SaCkptLibCheckpointT* libCheckpoint = NULL;
	SaCkptReqSecReadParamT* readParam = NULL;
	
	SaErrorT libError = SA_OK;
	int i = 0;

	if (checkpointHandle == NULL) {
		cl_log(LOG_ERR, 
			"Null handle in checkpoint library");
		return SA_ERR_INVALID_PARAM;
	}

	libCheckpoint = SaCkptGetLibCheckpointByHandle(
		*checkpointHandle);
	if (libCheckpoint == NULL) {
		cl_log(LOG_ERR, 
			"Checkpoint is not open");
		return SA_ERR_INVALID_PARAM;
	}
	if (!(libCheckpoint->openFlag & SA_CKPT_CHECKPOINT_READ)) {
		cl_log(LOG_ERR, 
			"Checkpoint is not opened for read");
		return SA_ERR_LIBRARY;
	}
	
	readParam = (SaCkptReqSecReadParamT*)ha_malloc(
		sizeof(SaCkptReqSecReadParamT));
	if (readParam == NULL) {
		cl_log(LOG_ERR, 
			"No memory in checkpoint library");
		return SA_ERR_NO_MEMORY;
	}

	for(i=0; i<numberOfElements; i++) {
		memset(readParam, 0, sizeof(SaCkptReqSecReadParamT));
		readParam->checkpointHandle = *checkpointHandle;
		readParam->sectionID.idLen= ioVector[i].sectionId.idLen;
		memcpy(readParam->sectionID.id,
			ioVector[i].sectionId.id,
			ioVector[i].sectionId.idLen);
		readParam->offset = ioVector[i].dataOffset;
		readParam->dataSize = ioVector[i].dataSize;

		libError = saCkptCheckpointSectionRead(readParam,
			&(ioVector[i].readSize), 
			ioVector[i].dataBuffer);
		if (libError != SA_OK) {
			if (erroneousVectorIndex != NULL) {
				*erroneousVectorIndex = i;
			}
			break;
		}
	}

	ha_free(readParam);

	return libError;
}

/* 
 * synchronize the checkpoint to all replicas
 */
SaErrorT
saCkptCheckpointSynchronize(
	const SaCkptCheckpointHandleT *checkpointHandle,
	SaTimeT timeout)
{
	SaCkptLibClientT* libClient = NULL;
	SaCkptLibRequestT* libRequest = NULL;
	SaCkptClientRequestT* clientRequest = NULL;
	SaCkptReqSyncParamT* syncParam = NULL;
 	SaCkptClientResponseT* clientResponse = NULL;
	SaCkptLibCheckpointT* libCheckpoint = NULL;
	
	SaErrorT libError = SA_OK;
	IPC_Channel* ch = NULL;

	if (checkpointHandle == NULL) {
		cl_log(LOG_ERR, 
			"Null handle in checkpoint library");
		return SA_ERR_INVALID_PARAM;
	}

	libCheckpoint = SaCkptGetLibCheckpointByHandle(
		*checkpointHandle);
	if (libCheckpoint == NULL) {
		cl_log(LOG_ERR, 
			"Checkpoint is not open");
		return SA_ERR_INVALID_PARAM;
	}
	libClient = libCheckpoint->client;
	
  	libRequest = (SaCkptLibRequestT*)ha_malloc(
					sizeof(SaCkptLibRequestT));
	clientRequest = (SaCkptClientRequestT*)ha_malloc(
					sizeof(SaCkptClientRequestT));
	syncParam = (SaCkptReqSyncParamT*)ha_malloc(
					sizeof(SaCkptReqSyncParamT));
 	if ((libRequest == NULL) ||
		(clientRequest == NULL) ||
		(syncParam == NULL)) {
		cl_log(LOG_ERR, 
			"No memory in checkpoint library");
		libError = SA_ERR_NO_MEMORY;
		goto syncError;
	}
	
	memset(libRequest, 0, sizeof(SaCkptLibRequestT));
	memset(clientRequest, 0, sizeof(SaCkptClientRequestT));
	memset(syncParam, 0, sizeof(SaCkptReqSyncParamT));
	
	libRequest->client = libClient;
	libRequest->timeoutTag = 0;
	libRequest->clientRequest = clientRequest;

	clientRequest->clientHandle = libClient->clientHandle;
	clientRequest->requestNO = SaCkptLibGetReqNO();
	clientRequest->req = REQ_CKPT_SYNC;
	clientRequest->reqParamLength = sizeof(SaCkptReqSyncParamT);
	clientRequest->dataLength = 0;
	clientRequest->reqParam = syncParam;
	clientRequest->data = NULL;

	syncParam->checkpointHandle = *checkpointHandle;
	syncParam->timeout = timeout;
	
	ch = libClient->channel[0];
	libError = SaCkptLibRequestSend(ch, libRequest->clientRequest);
	if (libError != SA_OK) {
		cl_log(LOG_ERR, 
			"Send activate_checkpoint_set request failed");
		goto syncError;
	}

	libError = SaCkptLibResponseReceive(ch, 
		libRequest->clientRequest->requestNO,
		&clientResponse);
	if (libError != SA_OK) {
		cl_log(LOG_ERR, "Receive response failed");
		goto syncError;
	}
	if (clientResponse == NULL) {
		cl_log(LOG_ERR, "Received null response");
		libError = SA_ERR_LIBRARY;
		goto syncError;
	}
	if (clientResponse->retVal != SA_OK) {
		cl_log(LOG_ERR, "Checkpoint daemon returned error");
		libError = clientResponse->retVal;
		goto syncError;
	}

	libError = SA_OK;

syncError:
	
	if (libRequest != NULL) {
		ha_free(libRequest);
	}
	
	if (clientRequest != NULL) {
		ha_free(clientRequest);
	}
	
	if (syncParam != NULL) {
		ha_free(syncParam);
	}

	if (clientResponse != NULL) {
		if (clientResponse->dataLength > 0) {
			ha_free(clientResponse->data);
		}
		ha_free(clientResponse);
	}

	return libError; 
}

/* 
 * sychronize the checkpoint replica on all nodes
 */
SaErrorT
saCkptCheckpointSynchronizeAsync(
	const SaCkptHandleT *ckptHandle,
	SaInvocationT invocation,
	const SaCkptCheckpointHandleT *checkpointHandle)
{
	SaCkptLibClientT* libClient = NULL;
	SaCkptLibRequestT* libRequest = NULL;
	SaCkptClientRequestT* clientRequest = NULL;
	SaCkptReqAsyncParamT* asyncParam = NULL;
	SaCkptLibCheckpointT* libCheckpoint = NULL;
	
	SaErrorT libError = SA_OK;
	IPC_Channel* ch = NULL;

	if (ckptHandle == NULL) {
		cl_log(LOG_ERR, 
			"Null handle in checkpoint library");
		return SA_ERR_INVALID_PARAM;
	}
	
	if (checkpointHandle == NULL) {
		cl_log(LOG_ERR, 
			"Null handle in checkpoint library");
		return SA_ERR_INVALID_PARAM;
	}

	libCheckpoint = SaCkptGetLibCheckpointByHandle(
		*checkpointHandle);
	if (libCheckpoint == NULL) {
		cl_log(LOG_ERR, 
			"Checkpoint is not open");
		return SA_ERR_INVALID_PARAM;
	}
	libClient = libCheckpoint->client;
	
  	libRequest = (SaCkptLibRequestT*)ha_malloc(
					sizeof(SaCkptLibRequestT));
	clientRequest = (SaCkptClientRequestT*)ha_malloc(
					sizeof(SaCkptClientRequestT));
	asyncParam = (SaCkptReqAsyncParamT*)ha_malloc(
					sizeof(SaCkptReqAsyncParamT));
 	if ((libRequest == NULL) ||
		(clientRequest == NULL) ||
		(asyncParam == NULL)) {
		cl_log(LOG_ERR, 
			"No memory in checkpoint library");
		libError = SA_ERR_NO_MEMORY;
		goto syncError;
	}
	
	memset(libRequest, 0, sizeof(SaCkptLibRequestT));
	memset(clientRequest, 0, sizeof(SaCkptClientRequestT));
	memset(asyncParam, 0, sizeof(SaCkptReqAsyncParamT));
	
	libRequest->client = libClient;
	libRequest->timeoutTag = 0;
	libRequest->clientRequest = clientRequest;

	clientRequest->clientHandle = libClient->clientHandle;
	clientRequest->requestNO = SaCkptLibGetReqNO();
	clientRequest->req = REQ_CKPT_SYNC_ASYNC;
	clientRequest->reqParamLength = sizeof(SaCkptReqAsyncParamT);
	clientRequest->dataLength = 0;
	clientRequest->reqParam = asyncParam;
	clientRequest->data = NULL;

	asyncParam->checkpointHandle = *checkpointHandle;
	asyncParam->invocation = invocation;
	
	ch = libClient->channel[1]; /* async channel */
	libError = SaCkptLibRequestSend(ch, libRequest->clientRequest);
	if (libError != SA_OK) {
		cl_log(LOG_ERR, 
			"Send activate_checkpoint_set request failed");
		goto syncError;
	}

	libAsyncRequestList = g_list_append(libAsyncRequestList, 
		libRequest);
	
	return SA_OK;

syncError:
	
	if (libRequest != NULL) {
		ha_free(libRequest);
	}
	
	if (clientRequest != NULL) {
		ha_free(clientRequest);
	}
	
	if (asyncParam != NULL) {
		ha_free(asyncParam);
	}

	return libError; 
}

