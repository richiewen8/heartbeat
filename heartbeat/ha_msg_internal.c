static const char * _ha_msg_c_Id = "$Id: ha_msg_internal.c,v 1.3 2000/07/26 05:17:19 alan Exp $";
/*
 * ha_msg_internal: heartbeat internal messaging functions
 *
 * Copyright (C) 2000 Alan Robertson <alanr@unix.sh>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/utsname.h>
#include <heartbeat.h>
#include <ha_msg.h>

#define		MINFIELDS	20
#define		CRNL		"\r\n"

/* Return the next message found in the stream and copies */
/* the iface in "iface"  */

struct ha_msg *
if_msgfromstream(FILE * f, char *iface)
{
	char		buf[MAXLINE];
	char *		getsret;
	struct ha_msg*	ret;

	(void)_ha_msg_c_Id;
	(void)_heartbeat_h_Id;
	(void)_ha_msg_h_Id;
	clearerr(f);

	if(!(getsret=fgets(buf, MAXLINE, f))) { 
		if (!ferror(f) || errno != EINTR) 
			ha_error("if_msgfromstream: cannot get message");
		return(NULL);
	}

	/* Try to find the interface in the message. */

	if (!strcmp(buf, IFACE)) {
		/* Found interface name header, get interface name. */
		if(!(getsret=fgets(buf, MAXLINE, f))) { 
			if (!ferror(f) || errno != EINTR)
				ha_error("if_msgfromstream: cannot get message");
			return(NULL);
		}
		if (iface) { 
			int len = strlen(buf);
			if(len < MAXIFACELEN) {
				strncpy(iface, buf, len);
				iface[len -1] = EOS;
			}
		}
	}

	if (strcmp(buf, MSG_START)) { 	
		/* Skip until we find a MSG_START (hopefully we skip nothing) */
		while ((getsret=fgets(buf, MAXLINE, f)) != NULL
		&&	strcmp(buf, MSG_START) != 0) {
			/* Nothing */
		}
	}

	if (getsret == NULL || (ret = ha_msg_new(0)) == NULL) {
		/* Getting an error with EINTR is pretty normal */
		if (!ferror(f) || errno != EINTR) {
			ha_error("if_msgfromstream: cannot get message");
		}
		return(NULL);
	}

	/* Add Name=value pairs until we reach MSG_END or EOF */
	while ((getsret=fgets(buf, MAXLINE, f)) != NULL
	&&	strcmp(buf, MSG_END) != 0) {

		/* Add the "name=value" string on this line to the message */
		if (ha_msg_add_nv(ret, buf) != HA_OK) {
			ha_error("NV failure (if_msgfromsteam):");
			ha_error(buf);
			ha_msg_del(ret);
			return(NULL);
		}
	}
	return(ret);
}

char *
msg2if_string(const struct ha_msg *m, const char *iface) 
{

	int	j;
	char *	buf;
	char *	bp;

	if (m->nfields <= 0) {
		ha_error("msg2if_string: Message with zero fields");
		return(NULL);
	}

	buf = ha_malloc(m->stringlen + ((strlen(iface) + sizeof(IFACE)) * sizeof(char )));

	if (buf == NULL) {
		ha_error("msg2if_string: no memory for string");
	}else{
		bp = buf;
		strcpy(buf, IFACE);
		strcat(buf, iface);
		strcat(buf, "\n");
		strcat(buf, MSG_START);
		for (j=0; j < m->nfields; ++j) {
			strcat(bp, m->names[j]);
			bp += m->nlens[j];
			strcat(bp, "=");
			bp++;
			strcat(bp, m->values[j]);
			bp += m->vlens[j];
			strcat(bp, "\n");
			bp++;
		}
		strcat(bp, MSG_END);
	}
	return(buf);
}


#define	SEQ	"seq"
#define	LOAD1	"load1"

/* The value functions are expected to return pointers to static data */
struct default_vals {
	const char *	name;
	const char * 	(*value)(void);
	int		seqfield;
};

STATIC	const char * ha_msg_seq(void);
STATIC	const char * ha_msg_timestamp(void);
STATIC	const char * ha_msg_loadavg(void);
STATIC	const char * ha_msg_from(void);
STATIC	const char * ha_msg_ttl(void);

/* Each of these functions returns static data requiring copying */
struct default_vals defaults [] = {
	{F_ORIG,	ha_msg_from,	0},
	{F_SEQ,		ha_msg_seq,	1},
	{F_TIME,	ha_msg_timestamp,0},
	{F_LOAD,	ha_msg_loadavg, 1},
	{F_TTL,		ha_msg_ttl, 0},
};

/* Reads from control fifo, and creates a new message from it */
/* (this adds a few default fields with timestamp, sequence #, etc.) */
struct ha_msg *
controlfifo2msg(FILE * f)
{
	char		buf[MAXLINE];
	char *		getsret;
	const char*	type;
	struct ha_msg*	ret;
	int		j;
	int		noseqno;

	/* Skip until we find a MSG_START (hopefully we skip nothing) */
	while ((getsret=fgets(buf, MAXLINE, f)) != NULL
	&&	strcmp(buf, MSG_START) != 0) {
		/* Nothing */
	}

	if (getsret == NULL || (ret = ha_msg_new(0)) == NULL) {
		ha_error("controlfifo2msg: cannot create message");
		return(NULL);
	}

	/* Add Name=value pairs until we reach MSG_END or EOF */
	while ((getsret=fgets(buf, MAXLINE, f)) != NULL
	&&	strcmp(buf, MSG_END) != 0) {

		/* Add the "name=value" string on this line to the message */
		if (ha_msg_add_nv(ret, buf) != HA_OK) {
			ha_error("NV failure (controlfifo2msg):");
			ha_error(buf);
			ha_msg_del(ret);
			return(NULL);
		}
	}
	if ((type = ha_msg_value(ret, F_TYPE)) == NULL) {
		ha_log(LOG_ERR, "No type (controlfifo2msg)");
		ha_msg_del(ret);
		return(NULL);
	}

	noseqno = (strncmp(type, NOSEQ_PREFIX, sizeof(NOSEQ_PREFIX)-1) == 0);

	/* Add our default name=value pairs */
	for (j=0; j < DIMOF(defaults); ++j) {

		/*
		 * Should we skip putting a sequence number on this packet?
		 *
		 * We don't want requests for retransmission to be subject
		 * to being retransmitted according to the protocol.  They
		 * need to be outside the normal retransmission protocol.
		 * To accomplish that, we avoid giving them sequence numbers.
		 */
		if (noseqno && defaults[j].seqfield) {
			continue;
		}

		/* Don't put in duplicate values already gotten */
		if (noseqno && ha_msg_value(ret, defaults[j].name) != NULL) {
			/* This keeps us from adding another "from" field */
			continue;
		}

		if (ha_msg_mod(ret, defaults[j].name, defaults[j].value())
		!=	HA_OK)  {
			ha_msg_del(ret);
			return(NULL);
		}
	}
	if (!add_msg_auth(ret)) {
		ha_msg_del(ret);
		ret = NULL;
	}

	return(ret);
}

int
add_msg_auth(struct ha_msg * m)
{
	char	msgbody[MAXMSG];
	char	authstring[MAXLINE];
	const char *	authtoken;
	char *	bp = msgbody;
	int	j;

	check_auth_change(config);
	msgbody[0] = EOS;
	for (j=0; j < m->nfields; ++j) {
		strcat(bp, m->names[j]);
		bp += m->nlens[j];
		strcat(bp, "=");
		bp++;
		strcat(bp, m->values[j]);
		bp += m->vlens[j];
		strcat(bp, "\n");
		bp++;
	}


	if ((authtoken
	=	config->authmethod->auth->auth(config->authmethod, msgbody))
	==	NULL) {
		ha_log(LOG_ERR, authstring
		,	"Cannot compute message authentication [%s/%s/%s]"
		,	config->authmethod->auth->authname
		,	config->authmethod->key
		,	msgbody);
		return(HA_FAIL);
	}

	sprintf(authstring, "%d %s", config->authnum, authtoken);

	/* It will add it if it's not there yet, or modify it if it is */

	return(ha_msg_mod(m, F_AUTH, authstring));
}

int
isauthentic(const struct ha_msg * m)
{
	char	msgbody[MAXMSG];
	char	authstring[MAXLINE];
	const char *	authtoken = 0;
	char *	bp = msgbody;
	int	j;
	int	authwhich = 0;
	struct auth_info*	which;
	
	if (m->stringlen >= sizeof(msgbody)) {
		return(0);
	}

	/* Reread authentication? */
	check_auth_change(config);

	msgbody[0] = EOS;
	for (j=0; j < m->nfields; ++j) {
		if (strcmp(m->names[j], F_AUTH) == 0) {
			authtoken = m->values[j];
			continue;
		}
		strcat(bp, m->names[j]);
		bp += m->nlens[j];
		strcat(bp, "=");
		bp++;
		strcat(bp, m->values[j]);
		bp += m->vlens[j];
		strcat(bp, "\n");
		bp++;
	}
	
	if (authtoken == NULL
	||	sscanf(authtoken, "%d %s", &authwhich, authstring) != 2) {
		ha_error("Bad/invalid auth token");
		return(0);
	}
	which = config->auth_config + authwhich;

	if (authwhich < 0 || authwhich >= MAXAUTH || which->auth == NULL) {
		ha_log(LOG_ERR
		,	"Invalid authentication type [%d] in message!"
		,	authwhich);
		return(0);
	}
		
	
	if ((authtoken = which->auth->auth(which, msgbody)) == NULL) {
		ha_error("Cannot check message authentication");
		return(0);
	}
	if (strcmp(authstring, authtoken) == 0) {
		if (DEBUGAUTH) {
			ha_log(LOG_DEBUG, "Packet authenticated");
		}
		return(1);
	}
	if (DEBUGAUTH) {
		ha_log(LOG_INFO, "Packet failed authentication check");
	}
	return(0);
}


/* Add field to say who this packet is from */
STATIC	const char *
ha_msg_from(void)
{
	static struct utsname u;
	static int uyet = 0;
	if (!uyet) {
		uname(&u);
		uyet++;
	}
	return(u.nodename);
}

/* Add sequence number field */
STATIC	const char *
ha_msg_seq(void)
{
	static char seq[32];
	static int seqno = 1;
	sprintf(seq, "%x", seqno);
	++seqno;
	return(seq);
}

/* Add local timestamp field */
STATIC	const char *
ha_msg_timestamp(void)
{
	static char ts[32];
	sprintf(ts, "%lx", time(NULL));
	return(ts);
}

/* Add load average field */
STATIC	const char *
ha_msg_loadavg(void)
{
	static char	loadavg[64];
	FILE *		fp;
	if ((fp=fopen(LOADAVG, "r")) == NULL) {
		strcpy(loadavg, "n/a");
	}else{
		fgets(loadavg, sizeof(loadavg), fp);
		fclose(fp);
	}
	loadavg[strlen(loadavg)-1] = EOS;
	return(loadavg);
}
STATIC	const char *
ha_msg_ttl(void)
{
	static char	ttl[8];
	sprintf(ttl, "%d", config->hopfudge + config->nodecount);
	return(ttl);
}

#ifdef TESTMAIN_MSGS
int
main(int argc, char ** argv)
{
	struct ha_msg*	m;
	while (!feof(stdin)) {
		if ((m=controlfifo2msg(stdin)) != NULL) {
			fprintf(stderr, "Got message!\n");	
			if (msg2stream(m, stdout) == HA_OK) {
				fprintf(stderr, "Message output OK!\n");
			}else{
				fprintf(stderr, "Could not output Message!\n");
			}
		}else{
			fprintf(stderr, "Could not get message!\n");
		}
	}
	return(0);
}
#endif
/*
 * $Log: ha_msg_internal.c,v $
 * Revision 1.3  2000/07/26 05:17:19  alan
 * Added GPL license statements to all the code.
 *
 * Revision 1.2  2000/07/19 23:03:53  alan
 * Working version of most of the API code.  It still has the security bug...
 *
 * Revision 1.1  2000/07/11 00:25:52  alan
 * Added a little more API code.  It looks like the rudiments are now working.
 *
 *
 */
