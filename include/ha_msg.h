/* $Id: ha_msg.h,v 1.39 2004/10/24 13:00:12 lge Exp $ */
/*
 * Intracluster message object (struct ha_msg)
 *
 * Copyright (C) 1999, 2000 Alan Robertson <alanr@unix.sh>
 * This software licensed under the GNU LGPL.
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

#ifndef _HA_MSG_H
#	define _HA_MSG_H 1
#include <stdio.h>
#include <clplumbing/cl_log.h>
#include <clplumbing/ipc.h>
#include <clplumbing/longclock.h>
#include <uuid/uuid.h>

#define	HA_FAIL		0
#define	HA_OK		1

enum cl_netstring_type{
	FT_STRING,
	FT_BINARY,
	FT_STRUCT,
	FT_LIST
};

enum cl_msgfmt{
	MSGFMT_NVPAIR,
	MSGFMT_NETSTRING
};


#define NEEDHEAD	1
#define NOHEAD		0
#define HA_MSG_ASSERT(X)  do{  if(!(X)){				\
	cl_log(LOG_ERR, "Assertion failed on line %d in file \"%s\""    \
	, __LINE__, __FILE__);					         \
	exit(1);		   				         \
	}								\
    }while(0)
	
typedef struct hb_msg_stats_s {
	unsigned long		totalmsgs;	/* Total # of messages */
						/* ever handled */
	unsigned long		allocmsgs;	/* # Msgs currently allocated */
	longclock_t		lastmsg;
}hb_msg_stats_t;

struct ha_msg {
	int	nfields;
	int	nalloc;
	size_t	stringlen;	/* #bytes needed to convert this to a string
				 * including the '\0' character at the end. */
	size_t  netstringlen;
	char **	names;
	int  *	nlens;
	void **	values;
	size_t*	vlens;
	int *	types;
};



struct fieldtypefuncs_s{

	/* memfree frees the memory involved*/
	void (*memfree)(void*);

	/* dup makes a complete copy of the field*/
	void* (*dup)(const void*, size_t);

	/* display printout the field*/
	void (*display)(int, char* , void*);

	/* add the field into a message*/
	int (*addfield) (struct ha_msg* msg, char* name, size_t namelen,
			 void* value, size_t vallen, int depth);

	/* return the string length required to add this field*/
	int (*stringlen) (size_t namlen, size_t vallen, const void* value);

	/* return the netstring length required to add this field*/
	int (*netstringlen) (size_t namlen, size_t vallen, const void* value);
	
	/* print the field into the provided buffer, convert it first */
	/* if ncecessary*/
	int (*tostring)(char*, char*, void* ,size_t,int);
	
	/* print the field into the provided buffer*/
	int (*tonetstring)(char*, char*, void*, size_t, size_t*);

	/* convert the given string to a field
	   note: this functions involves allocate memory for 
	   for the field
	*/
	int (*stringtofield)(void*, size_t, int depth, void**, size_t* );

	/* convert the given netstring to a field
	   note: this functions involves allocate memory for 
	   for the field
	*/
	int (*netstringtofield)(const void*, size_t, void**, size_t*);
};

extern struct fieldtypefuncs_s fieldtypefuncs[4];


#define	IFACE		"!^!\n"  
#define	MSG_START	">>>\n"
#define	MSG_END		"<<<\n"
#define	MSG_START_NETSTRING	"###\n"
#define	MSG_END_NETSTRING	"%%%\n"
#define	EQUAL		"="

#define	MAXMSG	40000	/* Maximum string length for a message */
#define MAXDEPTH 10     /* Maximum recursive message depth */
#define MAXLENGTH	1024

	/* Common field names for our messages */
#define	F_TYPE		"t"		/* Message type */
#define	F_SUBTYPE	"subt"		/* Message type */
#define	F_ORIG		"src"		/* Real Originator */
#define	F_ORIGUUID	"srcuuid"	/* Real Originator uuid*/
#define	F_NODE		"node"		/* Node being described */
#define	F_TO		"dest"		/* Destination (optional) */
#define F_TOUUID	"destuuid"	/* Destination uuid(optional) */
#define	F_STATUS	"st"		/* New status (type = status) */
#define	F_CLIENTNAME	"cn"		/* Client name */
#define	F_CLIENTSTATUS	"cs"		/* Client status */
#define	F_TIME		"ts"		/* Timestamp */
#define F_SEQ		"seq"		/* Sequence number */
#define	F_LOAD		"ld"		/* Load average */
#define	F_COMMENT	"info"		/* Comment */
#define	F_TTL		"ttl"		/* Time To Live */
#define F_AUTH		"auth"		/* Authentication string */
#define F_HBGENERATION	"hg"		/* Heartbeat generation number */
#define F_FIRSTSEQ	"firstseq"	/* Lowest seq # to retransmit */
#define F_LASTSEQ	"lastseq"	/* Highest seq # to retransmit */
#define F_RESOURCES	"rsc_hold"	/* What resources do we hold? */
#define F_FROMID	"from_id"	/* from Client id */
#define F_TOID		"to_id"		/* To client id */
#define F_PID		"pid"		/* PID of client */
#define F_UID		"uid"		/* uid of client */
#define F_GID		"gid"		/* gid of client */
#define F_ISSTABLE	"isstable"	/* true/false for RESOURCES */
#define F_APIREQ	"reqtype"	/* API request type for "hbapi" */
#define F_APIRESULT	"result"	/* API request result code */
#define F_IFNAME	"ifname"	/* Interface name */
#define F_PNAME		"pname"		/* Parameter name */
#define F_PVALUE	"pvalue"	/* Parameter name */
#define F_DEADTIME	"deadtime"	/* Dead time interval in ms. */
#define F_KEEPALIVE	"keepalive"	/* Keep alive time interval in ms. */
#define F_LOGFACILITY	"logfacility"	/* Suggested cluster syslog facility */
#define F_NODETYPE	"nodetype"	/* Type of node */
#define F_RTYPE		"rtype"		/* Resource type */
#define F_ORDERSEQ	"oseq"		/* Order Sequence number */
#define F_DT		"dt"		/* Dead time field for heartbeat*/

	/* Message types */
#define	T_STATUS	"status"	/* Status (heartbeat) */
#define	T_IFSTATUS	"ifstat"	/* Interface status */
#define	T_ASKRESOURCES	"ask_resources"	/* Let other node ask my resources */
#define T_ASKRELEASE	"ip-request"	/* Please give up these resources... */
#define T_ACKRELEASE	"ip-request-resp"/* Resources given up... */
#define	T_QCSTATUS	"query-cstatus"	/* Query client status */
#define	T_RCSTATUS	"respond-cstatus"/* Respond client status */
#define	T_STONITH	"stonith"	/* Stonith return code */
#define T_SHUTDONE	"shutdone"	/* External Shutdown complete */
#define T_CRM		"crmd"		/* Cluster resource manager message */

#define T_APIREQ	"hbapi-req"	/* Heartbeat API request */
#define T_APIRESP	"hbapi-resp"	/* Heartbeat API response */
#define T_APICLISTAT	"hbapi-clstat"	/* Client status notification" */

#define	NOSEQ_PREFIX	"NS_"		/* PREFIX: Give no sequence number    */
	/* Used for messages which can't be retransmitted		      */
	/* Either they're protocol messages or from dumb (ping) endpoints     */
#define	T_REXMIT	NOSEQ_PREFIX "rexmit"    	 /* Rexmit request    */
#define	T_NAKREXMIT	NOSEQ_PREFIX "nak_rexmit"	/* NAK Rexmit request */
#define	T_NS_STATUS	NOSEQ_PREFIX "st"		/* ping status        */

/* Messages associated with nice_failback */
#define T_STARTING      "starting"      /* Starting Heartbeat		*/
					/* (requesting resource report)	*/
#define T_RESOURCES	"resource"      /* Resources report		*/

/* Messages associated with stonith completion results */
#define T_STONITH_OK		"OK"  	  /* stonith completed successfully */
#define T_STONITH_BADHOST	"badhost" /* stonith failed */
#define T_STONITH_BAD		"bad"	  /* stonith failed */
#define T_STONITH_NOTCONFGD	"n_stnth" /* no stonith device configured */
#define T_STONITH_UNNEEDED	"unneeded" /* STONITH not required */

/* Set up message statistics area */
void cl_msg_setstats(volatile hb_msg_stats_t* stats);

/* Allocate new (empty) message */
struct ha_msg *	ha_msg_new(int nfields);

/* Free message */
void		ha_msg_del(struct ha_msg *msg);

/* Copy message */
struct ha_msg*	ha_msg_copy(const struct ha_msg *msg);

/*Add a null-terminated name and binary value to a message*/
int		ha_msg_addbin(struct ha_msg * msg, const char * name, 
				  const void * value, size_t vallen);

int		ha_msg_adduuid(struct ha_msg * msg, const char * name, 
				  uuid_t	uuid);

/* Add null-terminated name and a value to the message */
int		ha_msg_add(struct ha_msg * msg
		,	const char* name, const char* value);

/* Modify null-terminated name and a value to the message */
int		cl_msg_modstring(struct ha_msg * msg,
			   const char* name, 
			   const char* value);
int		cl_msg_modbin(struct ha_msg * msg,
			      const char* name, 
			      const void* value, 
			      size_t vlen);

int		cl_msg_moduuid(struct ha_msg * msg, const char * name, 
				uuid_t	uuid);

int		cl_msg_modstruct(struct ha_msg * msg,
				 const char* name, 
				 const struct ha_msg* value);
#define ha_msg_mod(msg, name, value) cl_msg_modstring(msg, name, value)


/* Add name, value (with known lengths) to the message */
int		ha_msg_nadd(struct ha_msg * msg, const char * name, int namelen
		,	const char * value, int vallen);

/* Add a name/value/type to a message (with sizes for name and value) */
int		ha_msg_nadd_type(struct ha_msg * msg, const char * name, int namelen
				 ,	const char * value, int vallen, int type);

/* Add name=value string to a message */
int		ha_msg_add_nv(struct ha_msg* msg, const char * nvline, const char * bufmax);

	
/* Return value associated with particular name */
#define ha_msg_value(m,name) cl_get_string(m, name)

/* Reads an IPC stream -- converts it into a message */
struct ha_msg *	msgfromIPC(IPC_Channel * f);

IPC_Message * ipcmsgfromIPC(IPC_Channel * ch);

/* Reads a stream -- converts it into a message */
struct ha_msg *	msgfromstream(FILE * f);

/* Reads a stream with string format--converts it into a message */
struct ha_msg *	msgfromstream_string(FILE * f);

/* Reads a stream with netstring format--converts it into a message */
struct ha_msg * msgfromstream_netstring(FILE * f);

/* Same as above plus copying the iface name to "iface" */
struct ha_msg * if_msgfromstream(FILE * f, char *iface);

/* Writes a message into a stream */
int		msg2stream(struct ha_msg* m, FILE * f);

/* Converts a message into a string and adds the iface name on start */
char *     msg2if_string(const struct ha_msg *m, const char * iface);

/* Converts a string gotten via UDP into a message */
struct ha_msg *	string2msg(const char * s, size_t length);

/* Converts a message into a string */
char *		msg2string(const struct ha_msg *m);

/* Converts a message into a string in the provided buffer with certain 
depth and with or without start/end */
int		msg2string_buf(const struct ha_msg *m, char* buf,
			       size_t len, int depth, int needhead);

/* Converts a message into wire format */
char*		msg2wirefmt(const struct ha_msg *m, size_t* );

/* Converts wire format data into a message */
struct ha_msg*	wirefmt2msg(const char* s, size_t length);

/* Convets wire format data into an IPC message */
IPC_Message*	wirefmt2ipcmsg(void* p, size_t len, IPC_Channel* ch);

/* Converts an ha_msg into an IPC message */
IPC_Message* hamsg2ipcmsg(struct ha_msg* m, IPC_Channel* ch);

/* Converts an IPC message into an ha_msg */
struct ha_msg* ipcmsg2hamsg(IPC_Message*m);

/* Outputs a message to an IPC channel */
int msg2ipcchan(struct ha_msg*m, IPC_Channel*ch);

/* Outpus a message to an IPC channel without authencating 
the message */
struct ha_msg* msgfromIPC_noauth(IPC_Channel * ch);

/* Reads from control fifo, and creates a new message from it */
/* This adds the default sequence#, load avg, etc. to the message */
struct ha_msg *	controlfifo2msg(FILE * f);

/* Check if the message is authenticated */
gboolean	isauthentic(const struct ha_msg * msg);

/* Get the required string length for the given message */ 
int get_stringlen(const struct ha_msg *m);

/* Get the requried netstring length for the given message*/
int get_netstringlen(const struct ha_msg *m);

/* Add a child message to a message as a field */
int ha_msg_addstruct(struct ha_msg * msg, const char * name, void* ptr);

/* Get binary data from a message */
const void * cl_get_binary(const struct ha_msg *msg, const char * name, size_t * vallen);

/* Get uuid data from a message */
int cl_get_uuid(const struct ha_msg *msg, const char * name, uuid_t retval);

/* Get string data from a message */
const char * cl_get_string(const struct ha_msg *msg, const char *name);

/* Get the type for a field from a message */
int cl_get_type(const struct ha_msg *msg, const char *name);

/* Get a child message from a message*/
struct ha_msg *cl_get_struct(const struct ha_msg *msg, const char* name);

/* Log the contents of a  message */
void cl_log_message (const struct ha_msg *m);

/* Supply messaging system with old style authentication/authorization method */
void cl_set_oldmsgauthfunc(gboolean (*authfunc)(const struct ha_msg*));

/* Set default messaging format */
void cl_set_msg_format(enum cl_msgfmt mfmt);

/* Add a string to a list*/
int cl_msg_list_add_string(struct ha_msg* msg, const char* name, const char* value);

/* Return length of a list*/
int cl_msg_list_length(struct ha_msg* msg, const char* name);

/* Return nth element of a list*/
void* cl_msg_list_nth_data(struct ha_msg* msg, const char* name, int n);


/*internal use for list type*/
size_t string_list_pack_length(const GList* list);
int string_list_pack(GList* list, char* buf, char* maxp);
GList* string_list_unpack(const char* packed_str_list, size_t length);
void list_cleanup(GList* list);

#endif /* __HA_MSG_H */
