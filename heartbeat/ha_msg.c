static const char * _ha_msg_c_Id = "$Id: ha_msg.c,v 1.27 2002/09/17 20:48:06 alan Exp $";
/*
 * Heartbeat messaging object.
 *
 * Copyright (C) 2000 Alan Robertson <alanr@unix.sh>
 *
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

#include <portability.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/utsname.h>
#include <heartbeat.h>
#include <ha_msg.h>
#include <hb_proc.h>
#include <unistd.h>
#include <signal.h>

#define		MINFIELDS	20
#define		CRNL		"\r\n"


/* Create a new (empty) message */
struct ha_msg *
ha_msg_new(nfields)
{
	struct ha_msg *	ret;

	(void)_heartbeat_h_Id;
	(void)_ha_msg_c_Id;
	(void)_ha_msg_h_Id;
	ret = MALLOCT(struct ha_msg);
	if (ret) {
		ret->nfields = 0;
		ret->nalloc	= MINFIELDS;
		ret->names	= (char **)ha_malloc(sizeof(char *)*MINFIELDS);
		ret->nlens	= (int *)ha_malloc(sizeof(int)*MINFIELDS);
		ret->values	= (char **)ha_malloc(sizeof(char *)*MINFIELDS);
		ret->vlens	= (int *)ha_malloc(sizeof(int)*MINFIELDS);
		ret->stringlen	= sizeof(MSG_START)+sizeof(MSG_END)-1;
		if (ret->names == NULL || ret->values == NULL
		||	ret->nlens == NULL || ret->vlens == NULL) {
			ha_error("ha_msg_new: out of memory for ha_msg");
			ha_msg_del(ret);
			ret = NULL;
		}else if (curproc) {
			curproc->allocmsgs++;
			curproc->totalmsgs++;
			curproc->lastmsg = time(NULL);
		}
	}
	return(ret);
}

/* Delete (destroy) a message */
void
ha_msg_del(struct ha_msg *msg)
{
	if (msg) {
		int	j;
		if (curproc) {
			curproc->allocmsgs--;
		}
		if (msg->names) {
			for (j=0; j < msg->nfields; ++j) {
				if (msg->names[j]) {
					ha_free(msg->names[j]);
					msg->names[j] = NULL;
				}
			}
			ha_free(msg->names);
			msg->names = NULL;
		}
		if (msg->values) {
			for (j=0; j < msg->nfields; ++j) {
				if (msg->values[j]) {
					ha_free(msg->values[j]);
					msg->values[j] = NULL;
				}
			}
			ha_free(msg->values);
			msg->values = NULL;
		}
		if (msg->nlens) {
			ha_free(msg->nlens);
			msg->nlens = NULL;
		}
		if (msg->vlens) {
			ha_free(msg->vlens);
			msg->vlens = NULL;
		}
		msg->nfields = -1;
		msg->nalloc = -1;
		msg->stringlen = -1;
		ha_free(msg);
	}
}

/* Add a null-terminated name and value to a message */
int
ha_msg_add(struct ha_msg * msg, const char * name, const char * value)
{
	return(ha_msg_nadd(msg, name, strlen(name), value, strlen(value)));
}

/* Add a name/value pair to a message (with sizes for name and value) */
int
ha_msg_nadd(struct ha_msg * msg, const char * name, int namelen
		,	const char * value, int vallen)
{
	int	next;
	char *	cpname;
	char *	cpvalue;
	int	startlen = sizeof(MSG_START)-1;
	int	newlen = msg->stringlen + (namelen+vallen+2);
				/* 2 == "=" + "\n" */

	if (!msg || (msg->nfields >= msg->nalloc)
	||	msg->names == NULL || msg->values == NULL) {
		ha_error("ha_msg_nadd: cannot add field to ha_msg");
		return(HA_FAIL);
	}
	if (name == NULL || value == NULL
	||	namelen <= 0 || vallen < 0 || newlen >= MAXMSG) {
		ha_error("ha_msg_nadd: cannot add name/value to ha_msg");
		return(HA_FAIL);
	}

	if (namelen >= startlen && strncmp(name, MSG_START, startlen) == 0) {
		ha_error("ha_msg_nadd: illegal field");
		return(HA_FAIL);
	}
		
	if (memchr(value, '\n', vallen) != NULL) {
		ha_log(LOG_ERR
		,	"ha_msg_nadd: newline in value. name [%s]"
		" value [%s]", name, value);
	}

	if ((cpname = ha_malloc(namelen+1)) == NULL) {
		ha_error("ha_msg_nadd: no memory for string (name)");
		return(HA_FAIL);
	}
	if ((cpvalue = ha_malloc(vallen+1)) == NULL) {
		ha_free(cpname);
		ha_error("ha_msg_nadd: no memory for string (value)");
		return(HA_FAIL);
	}
	/* Copy name, value, appending EOS to the end of the strings */
	strncpy(cpname, name, namelen);		cpname[namelen] = EOS;
	strncpy(cpvalue, value, vallen);	cpvalue[vallen] = EOS;

	next = msg->nfields;
	msg->values[next] = cpvalue;
	msg->vlens[next] = vallen;
	msg->names[next] = cpname;
	msg->nlens[next] = namelen;
	msg->stringlen = newlen;
	msg->nfields++;
	return(HA_OK);
}

/* Add a "name=value" line to the name, value pairs in a message */
int
ha_msg_add_nv(struct ha_msg* msg, const char * nvline, const char * bufmax)
{
	int		namelen;
	const char *	valp;
	int		vallen;

	if (!nvline) {
		ha_error("ha_msg_add_nv: NULL nvline");
		return(HA_FAIL);
	}
	/* How many characters before the '='? */
	if ((namelen = strcspn(nvline, EQUAL)) <= 0
	||	nvline[namelen] != '=') {
		ha_log(LOG_WARNING, "ha_msg_add_nv: line doesn't contain '='");
		ha_log(LOG_INFO, nvline);
		return(HA_FAIL);
	}
	valp = nvline + namelen +1; /* Point just *past* the '=' */
	if (valp >= bufmax)		return HA_FAIL;
	vallen = strcspn(valp, CRNL);
	if ((valp + vallen) >= bufmax)	return HA_FAIL;

	/* Call ha_msg_nadd to actually add the name/value pair */
	return(ha_msg_nadd(msg, nvline, namelen, valp, vallen));
	
}

/* Return the value associated with a particular name */
const char *
ha_msg_value(const struct ha_msg * msg, const char * name)
{
	int	j;
	if (!msg || !msg->names || !msg->values) {
		ha_error("ha_msg_value: NULL msg");
		return(NULL);
	}

	for (j=0; j < msg->nfields; ++j) {
		if (strcmp(name, msg->names[j]) == 0) {
			return(msg->values[j]);
		}
	}
	return(NULL);
}


/* Modify the value associated with a particular name */
int
ha_msg_mod(struct ha_msg * msg, const char * name, const char * value)
{
	int	j;

	if (msg == NULL || name == NULL || value == NULL) {
		ha_error("ha_msg_mod: NULL input.");
		return HA_FAIL;
	}
	for (j=0; j < msg->nfields; ++j) {
		if (strcmp(name, msg->names[j]) == 0) {
			char *	newv = ha_malloc(strlen(value)+1);
			int	newlen;
			int	sizediff = 0;
			if (newv == NULL) {
				ha_error("ha_msg_mod: out of memory");
				return(HA_FAIL);
			}
			ha_free(msg->values[j]);
			msg->values[j] = newv;
			newlen = strlen(value);
			sizediff = newlen - msg->vlens[j];
			msg->stringlen += sizediff;
			msg->vlens[j] = newlen;
			strcpy(newv, value);
			return(HA_OK);
		}
	}
	return(ha_msg_add(msg, name, value));
}

/* Return the next message found in the stream */
struct ha_msg *
msgfromstream(FILE * f)
{
	char		buf[MAXLINE];
	const char *	bufmax = buf + sizeof(buf);
	char *		getsret;
	struct ha_msg*	ret;

	clearerr(f);
	/* Skip until we find a MSG_START (hopefully we skip nothing) */
	while ((getsret=fgets(buf, MAXLINE, f)) != NULL
	&&	strcmp(buf, MSG_START) != 0) {
		/* Nothing */
	}

	if (getsret == NULL || (ret = ha_msg_new(0)) == NULL) {
		/* Getting an error with EINTR is pretty normal */
		/* (so is EOF) */
		if ((!ferror(f) || errno != EINTR) && !feof(f)) {
			ha_error("msgfromstream: cannot get message");
		}
		return(NULL);
	}

	/* Add Name=value pairs until we reach MSG_END or EOF */
	while ((getsret=fgets(buf, MAXLINE, f)) != NULL
	&&	strcmp(buf, MSG_END) != 0) {

		/* Add the "name=value" string on this line to the message */
		if (ha_msg_add_nv(ret, buf, bufmax) != HA_OK) {
			ha_log(LOG_ERR, "NV failure (msgfromsteam): [%s]"
			,	buf);
			ha_msg_del(ret); ret=NULL;
			return(NULL);
		}
	}
	return(ret);
}


/* Writes a message into a stream - used for serial lines */
int	
msg2stream(struct ha_msg* m, FILE * f)
{
	char *	s  = msg2string(m);
	if (s != NULL) {
		int	rc = HA_OK;
		if (fputs(s, f) == EOF) {
			rc = HA_FAIL;
			ha_perror("msg2stream: fputs failure");
		}
		if (fflush(f) == EOF) {
			ha_perror("msg2stream: fflush failure");
			rc = HA_FAIL;
		}
		ha_free(s);
		return(rc);
	}else{
		return(HA_FAIL);
	}
}

/* Converts a string (perhaps gotten via UDP) into a message */
struct ha_msg *
string2msg(const char * s, size_t length)
{
	struct ha_msg*	ret;
	int		startlen;
	int		endlen;
	const char *	sp = s;
	const char *	smax = s + length;

	if ((ret = ha_msg_new(0)) == NULL) {
		return(NULL);
	}

	startlen = sizeof(MSG_START)-1;
	if (strncmp(sp, MSG_START, startlen) != 0) {
		/* This can happen if the sender gets killed */
		/* at just the wrong time... */
		ha_log(LOG_WARNING, "string2msg: no MSG_START");
		return(NULL);
	}else{
		sp += startlen;
	}

	endlen = sizeof(MSG_END)-1;

	/* Add Name=value pairs until we reach MSG_END or end of string */

	while (*sp != EOS && strncmp(sp, MSG_END, endlen) != 0) {

		if (sp >= smax)		return(NULL);
		/* Skip over initial CR/NL things */
		sp += strspn(sp, CRNL);
		if (sp >= smax)		return(NULL);

		/* End of message marker? */
		if (strncmp(sp, MSG_END, endlen) == 0) {
			break;
		}
		/* Add the "name=value" string on this line to the message */
		if (ha_msg_add_nv(ret, sp, smax) != HA_OK) {
			ha_error("NV failure (string2msg):");
			ha_error(s);
			ha_msg_del(ret);
			return(NULL);
		}
		if (sp >= smax)		return(NULL);
		sp += strcspn(sp, CRNL);
	}
	return(ret);
}


/* Converts a message into a string (for sending out UDP interface) */
char *
msg2string(const struct ha_msg *m)
{
	int	j;
	char *	buf;
	char *	bp;

	if (m->nfields <= 0) {
		ha_error("msg2string: Message with zero fields");
		return(NULL);
	}

	buf = ha_malloc(m->stringlen);

	if (buf == NULL) {
		ha_error("msg2string: no memory for string");
	}else{
		bp = buf;
		strcpy(buf, MSG_START);
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

void
ha_log_message (const struct ha_msg *m)
{
	int	j;

	ha_log(LOG_INFO, "MSG: Dumping message with %d fields", m->nfields);

	for (j=0; j < m->nfields; ++j) {
		ha_log(LOG_INFO, "MSG[%d]: [%s=%s]",j, m->names[j], m->values[j]);
	}
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
 * $Log: ha_msg.c,v $
 * Revision 1.27  2002/09/17 20:48:06  alan
 * Put in a check for NULL in ha_msg_mod().
 *
 * Revision 1.26  2002/08/10 02:13:32  alan
 * Better error logging when ha_msg functions are given bad name/value pairs.
 *
 * Revision 1.25  2002/07/08 04:14:12  alan
 * Updated comments in the front of various files.
 * Removed Matt's Solaris fix (which seems to be illegal on Linux).
 *
 * Revision 1.24  2002/04/13 22:35:08  alan
 * Changed ha_msg_add_nv to take an end pointer to make it safer.
 * Added a length parameter to string2msg so it would be safer.
 * Changed the various networking plugins to use the new string2msg().
 *
 * Revision 1.23  2002/04/11 05:57:44  alan
 * Made some of the debugging output clearer.
 *
 * Revision 1.22  2002/02/21 21:43:33  alan
 * Put in a few fixes to make the client API work more reliably.
 * Put in a few changes to the process exit handling code which
 * also cause heartbeat to (attempt to) restart when it finds one of it's
 * own processes dies.  Restarting was already broken :-(
 *
 * Revision 1.21  2002/02/14 14:09:29  alan
 * Put in a change requested by Ram Pai to allow message values to be
 * empty strings.
 *
 * Revision 1.20  2001/10/24 20:46:28  alan
 * A large number of patches.  They are in these categories:
 * 	Fixes from Matt Soffen
 * 	Fixes to test environment things - including changing some ERRORs to
 * 		WARNings and vice versa.
 * 	etc.
 *
 * Revision 1.19  2001/08/21 15:37:13  alan
 * Put in code to make sure the calls in msg2stream get checked for errors...
 *
 * Revision 1.18  2001/06/19 13:56:28  alan
 * FreeBSD portability patch from Matt Soffen.
 * Mainly added #include "portability.h" to lots of files.
 * Also added a library to Makefile.am
 *
 * Revision 1.17  2001/06/12 17:05:47  alan
 * Fixed bug reported by Emily Ratliff <ratliff@austin.ibm.com>
 * In ha_msg_mod() the code fails to update the stringlen value for
 * fields modified by the input parameters.
 * This could potentially cause a crash.
 * Thanks to Emily for reporting this bug!
 *
 * Revision 1.16  2001/05/11 14:55:06  alan
 * Followed David Lee's suggestion about splitting out all the heartbeat process
 * management stuff into a separate header file...
 * Also changed to using PATH_MAX for maximum pathname length.
 *
 * Revision 1.15  2000/07/26 05:17:19  alan
 * Added GPL license statements to all the code.
 *
 * Revision 1.14  2000/07/19 23:03:53  alan
 * Working version of most of the API code.  It still has the security bug...
 *
 * Revision 1.13  2000/07/11 14:42:42  alan
 * More progress on API code.
 *
 * Revision 1.12  2000/07/11 00:25:52  alan
 * Added a little more API code.  It looks like the rudiments are now working.
 *
 * Revision 1.11  2000/05/11 22:47:50  alan
 * Minor changes, plus code to put in hooks for the new API.
 *
 * Revision 1.10  2000/04/12 23:03:49  marcelo
 * Added per-link status instead per-host status. Now we will able
 * to develop link<->service dependacy scheme.
 *
 * Revision 1.9  1999/11/22 20:28:23  alan
 * First pass of putting real packet retransmission.
 * Still need to request missing packets from time to time
 * in case retransmit requests get lost.
 *
 * Revision 1.8  1999/10/25 15:35:03  alan
 * Added code to move a little ways along the path to having error recovery
 * in the heartbeat protocol.
 * Changed the code for serial.c and ppp-udp.c so that they reauthenticate
 * packets they change the ttl on (before forwarding them).
 *
 * Revision 1.7  1999/10/10 20:11:56  alanr
 * New malloc/free (untested)
 *
 * Revision 1.6  1999/10/05 06:00:55  alanr
 * Added RPM Cflags to Makefiles
 *
 * Revision 1.5  1999/10/03 03:13:43  alanr
 * Moved resource acquisition to 'heartbeat', also no longer attempt to make the FIFO, it's now done in heartbeat.  It should now be possible to start it up more readily...
 *
 * Revision 1.4  1999/09/29 03:22:05  alanr
 * Added the ability to reread auth config file on SIGHUP
 *
 * Revision 1.3  1999/09/26 21:59:58  alanr
 * Allow multiple auth strings in auth file... (I hope?)
 *
 * Revision 1.2  1999/09/26 14:01:01  alanr
 * Added Mijta's code for authentication and Guenther Thomsen's code for serial locking and syslog reform
 *
 * Revision 1.9  1999/09/16 05:50:20  alanr
 * Getting ready for 0.4.3...
 *
 * Revision 1.8  1999/08/25 06:34:26  alanr
 * Added code to log outgoing messages in a FIFO...
 *
 * Revision 1.7  1999/08/18 04:28:48  alanr
 * added function to dump a message to the log...
 *
 * Revision 1.6  1999/08/17 03:46:48  alanr
 * added log entry...
 *
 */
