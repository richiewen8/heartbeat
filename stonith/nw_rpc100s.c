/*
 *	Stonith module for Night/Ware RPC100S 
 *
 *      Original code from baytech.c by
 *	Copyright (c) 2000 Alan Robertson <alanr@unix.sh>
 *
 *      Modifications for NW RPC100S
 *	Copyright (c) 2000 Computer Generation Incorporated
 *               Eric Z. Ayers <eric.ayers@compgen.com>
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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <syslog.h>
#include <libintl.h>
#include <sys/wait.h>
#include <termios.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <stonith/expect.h>
#include <stonith/stonith.h>

#define	DEVICE	"NW RPC100S Power Switch"

#define N_(text)	(text)
#define _(text)		dgettext(ST_TEXTDOMAIN, text)

/*
   The Nightware RPS-100S is manufactured by:
   
      Micro Energetics Corp
      +1 703 250-3000
      http://www.nightware.com/

   Thank you to David Hicks of Micro Energetics Corp. for providing
   a demo unit to write this software.
      
   This switch has a very simple protocol, 
   You issue a command and it  gives a response.
   Sample commands are conveniently documented on a sticker on the
      bottom of the device.
      
   The switch accepts a single command of the form

   //0,yyy,zzz[/m][/h]<CR>
   
     Where yyy is the wait time before activiting the relay.
           zzz is the relay time.

     The default is that the relay is in a default state of ON, which
     means that  usually yyy is the number of seconds to wait
     before shutting off the power  and zzz is the number of seconds the
     power remains off.  There is a dip switch to change the default
     state to 'OFF'.  Don't set this switch. It will screw up this code. 

     An asterisk can be used for zzz to specify an infinite switch time.
     The /m /and /h command options will convert the specified wait and
     switch times to either minutewes or hours. 
   
   A response is either
    <cr><lf>OK<cr><lf>
       or
    <cr><lf>Invalid Entry<cr><lf>


   As far as THIS software is concerned, we have to implement 4 commands:

   status     -->    //0,0,BOGUS; # Not a real command, this is just a
                                  #   probe to see if switch is alive
   open(on)   -->    //0,0,0;     # turn power to default state (on)
   close(off) -->    //0,0,*;     # leave power off indefinitely
   reboot     -->    //0,0,10;    # immediately turn power off for 10 seconds.

   and expect the response 'OK' to confirm that the unit is operational.
*/



struct NW_RPC100S {
	const char *	WTIid;

	char *	idinfo;  /* ??? What's this for Alan ??? */
	char *	unitid;  /* ??? What's this for Alan ??? */

	int	fd;      /* FD open to the serial port */

	int	config;  /* 0 if not configured, 
				    1 if configured with st_setconffile() 
				    or st_setconfinfo()
				 */
	char *	device;  /* Serial device name to use to communicate 
                            to this RPS10
			 */

	char *  node;    /* Name of the node that this is controlling */

};

/* This string is used to identify this type of object in the config file */
static const char * WTIid = "NW_RPC100S";
static const char * NOTwtiid = "OBJECT DESTROYED: (NW RPC100S)";

/* WTIpassword - The fixed string ^B^X^X^B^X^X */
static const char WTIpassword[7] = {2,24,24,2,24,24,0}; 

#ifndef DEBUG
#define DEBUG 0
#endif
static int gbl_debug = DEBUG;

#define	ISNWRPC100S(i)	(((i)!= NULL && (i)->pinfo != NULL)	\
	&& ((struct NW_RPC100S *)(i->pinfo))->WTIid == WTIid)

#define	ISCONFIGED(i)	(ISNWRPC100S(i) && ((struct NW_RPC100S *)(i->pinfo))->config)

#ifndef MALLOC
#	define	MALLOC	malloc
#endif
#ifndef FREE
#	define	FREE	free
#endif
#ifndef MALLOCT
#	define     MALLOCT(t)      ((t *)(MALLOC(sizeof(t)))) 
#endif

#define DIMOF(a)	(sizeof(a)/sizeof(a[0]))
#define WHITESPACE	" \t\n\r\f"

#define	REPLSTR(s,v)	{					\
			if ((s) != NULL) {			\
				FREE(s);			\
				(s)=NULL;			\
			}					\
			(s) = MALLOC(strlen(v)+1);		\
			if ((s) == NULL) {			\
				syslog(LOG_ERR, _("out of memory"));\
			}else{					\
				strcpy((s),(v));		\
			}					\
			}

/*
 *	Different expect strings that we get from the NW_RPC100S
 *	Remote Power Controllers...
 */

static struct Etoken NWtokOK[] =	{ {"OK", 0, 0}, {NULL,0,0}};
static struct Etoken NWtokInvalidEntry[] = { {"Invalid Entry", 0, 0}, {NULL,0,0}};
/* Accept either a CR/NL or an NL/CR */
static struct Etoken NWtokCRNL[] =	{ {"\n\r",0,0},{"\r\n",0,0},{NULL,0,0}};

static int	RPCConnect(struct NW_RPC100S * ctx);
static int	RPCDisconnect(struct NW_RPC100S * ctx);

static int	RPCReset(struct NW_RPC100S*, int unitnum, const char * rebootid);
#if defined(ST_POWERON) 
static int	RPCOn(struct NW_RPC100S*, int unitnum, const char * rebootid);
#endif
#if defined(ST_POWEROFF) 
static int	RPCOff(struct NW_RPC100S*, int unitnum, const char * rebootid);
#endif
static int	RPCNametoOutlet ( struct NW_RPC100S * ctx, const char * host );

int  	st_setconffile(Stonith *, const char * cfgname);
int	st_setconfinfo(Stonith *, const char * info);
static int RPC_parse_config_info(struct NW_RPC100S* ctx, const char * info);
const char *
		st_getinfo(Stonith * s, int InfoType);

char **	st_hostlist(Stonith  *);
void	st_freehostlist(char **);
int	st_status(Stonith * );
int	st_reset(Stonith * s, int request, const char * host);
void	st_destroy(Stonith *);
void *	st_new(void);

/*
 *	We do these things a lot.  Here are a few shorthand macros.
 */

#define	SEND(cmd, timeout)		{                       \
	int return_val = RPCSendCommand(ctx, cmd, timeout);     \
	if (return_val != S_OK)  return return_val;                     \
}

#define	EXPECT(p,t)	{						\
			if (RPCLookFor(ctx, p, t) < 0)			\
				return(errno == ETIMEDOUT		\
			?	S_TIMEOUT : S_OOPS);			\
			}

#define	NULLEXPECT(p,t)	{						\
				if (RPCLookFor(ctx, p, t) < 0)		\
					return(NULL);			\
			}

#define	SNARF(s, to)	{						\
				if (RPCScanLine(ctx,to,(s),sizeof(s))	\
				!=	S_OK)				\
					return(S_OOPS);			\
			}

#define	NULLSNARF(s, to)	{					\
				if (RPCScanLine(ctx,to,(s),sizeof(s))	\
				!=	S_OK)				\
					return(NULL);			\
				}

/* Look for any of the given patterns.  We don't care which */

static int
RPCLookFor(struct NW_RPC100S* ctx, struct Etoken * tlist, int timeout)
{
	int	rc;
	if ((rc = ExpectToken(ctx->fd, tlist, timeout, NULL, 0)) < 0) {
		syslog(LOG_ERR, _("Did not find string: '%s' from" DEVICE ".")
		,	tlist[0].string);
		RPCDisconnect(ctx);
		return(-1);
	}
	return(rc);
}


/*
 * RPCSendCommand - send a command to the specified outlet
 */
static int
RPCSendCommand (struct NW_RPC100S *ctx, const char *command, int timeout)
{
	char            writebuf[64]; /* All commands are short.
					 They should be WAY LESS
					 than 64 chars long!
				      */
	int		return_val;  /* system call result */
	fd_set          rfds, wfds, xfds;
				     /*  list of FDs for select() */
	struct timeval 	tv;	     /*  */

	FD_ZERO(&rfds);
	FD_ZERO(&wfds);
	FD_ZERO(&xfds);

	snprintf (writebuf, sizeof(writebuf), "%s\r", command);

	if (gbl_debug) printf ("Sending %s\n", writebuf);

	/* Make sure the serial port won't block on us. use select()  */
	FD_SET(ctx->fd, &wfds);
	FD_SET(ctx->fd, &xfds);
	
	tv.tv_sec = timeout;
	tv.tv_usec = 0;
	
	return_val = select(ctx->fd+1, NULL, &wfds,&xfds, &tv);
	if (return_val == 0) {
		/* timeout waiting on serial port */
		syslog (LOG_ERR, "%s: Timeout writing to %s",
			WTIid, ctx->device);
		return S_TIMEOUT;
	} else if ((return_val == -1) || FD_ISSET(ctx->fd, &xfds)) {
		/* an error occured */
		syslog (LOG_ERR, "%s: Error before writing to %s: %s",
			WTIid, ctx->device, strerror(errno));		
		return S_OOPS;
	}

	/* send the command */
	if (write(ctx->fd, writebuf, strlen(writebuf)) != strlen(writebuf)) {
		syslog (LOG_ERR, "%s: Error writing to  %s : %s",
			WTIid, ctx->device, strerror(errno));
		return S_OOPS;
	}

	/* suceeded! */
	return S_OK;

}  /* end RPCSendCommand() */

/* 
 * RPCReset - Reset (power-cycle) the given outlet number
 *
 * This device can only control one power outlet - unitnum is ignored.
 *
 */
static int
RPCReset(struct NW_RPC100S* ctx, int unitnum, const char * rebootid)
{

	if (gbl_debug) printf ("Calling RPCReset (%s)\n", WTIid);
	
	if (ctx->fd < 0) {
		syslog(LOG_ERR, "%s: device %s is not open!", WTIid, 
		       ctx->device);
		return S_OOPS;
	}

	/* send the "toggle power" command */
	SEND("//0,0,10;\r\n", 12);

	/* Expect "OK" */
	EXPECT(NWtokOK, 5);
	if (gbl_debug) printf ("Got OK\n");
	EXPECT(NWtokCRNL, 2);
	if (gbl_debug) printf ("Got NL\n");
	
	return(S_OK);

} /* end RPCReset() */


#if defined(ST_POWERON) 
/* 
 * RPCOn - Turn OFF the given outlet number 
 */
static int
RPCOn(struct NW_RPC100S* ctx, int unitnum, const char * host)
{

	if (ctx->fd < 0) {
		syslog(LOG_ERR, "%s: device %s is not open!", WTIid, 
		       ctx->device);
		return S_OOPS;
	}

	/* send the "On" command */
	SEND("//0,0,0;\r\n", 10);

	/* Expect "OK" */
	EXPECT(NWtokOK, 5);
	EXPECT(NWtokCRNL, 2);

	return(S_OK);

} /* end RPCOn() */
#endif


#if defined(ST_POWEROFF) 
/* 
 * RPCOff - Turn Off the given outlet number 
 */
static int
RPCOff(struct NW_RPC100S* ctx, int unitnum, const char * host)
{

	if (ctx->fd < 0) {
		syslog(LOG_ERR, "%s: device %s is not open!", WTIid, 
		       ctx->device);
		return S_OOPS;
	}

	/* send the "Off" command */
	SEND("//0,0,*;\r\n", 10);

	/* Expect "OK" */
	EXPECT(NWtokOK, 5);
	EXPECT(NWtokCRNL, 2);

	return(S_OK);

} /* end RPCOff() */
#endif


/*
 * st_status - API entry point to probe the status of the stonith device 
 *           (basically just "is it reachable and functional?", not the
 *            status of the individual outlets)
 * 
 * Returns:
 *    S_OOPS - some error occured
 *    S_OK   - if the stonith device is reachable and online.
 */
int
st_status(Stonith  *s)
{
	struct NW_RPC100S*	ctx;
	
	if (gbl_debug) printf ("Calling st_status (%s)\n", WTIid);
	
	if (!ISNWRPC100S(s)) {
		syslog(LOG_ERR, "invalid argument to RPC_status");
		return(S_OOPS);
	}
	if (!ISCONFIGED(s)) {
		syslog(LOG_ERR
		,	"unconfigured stonith object in RPC_status");
		return(S_OOPS);
	}
	ctx = (struct NW_RPC100S*) s->pinfo;
	if (RPCConnect(ctx) != S_OK) {
		return(S_OOPS);
	}

	/* The "connect" really does enough work to see if the 
	   controller is alive...  It verifies that it is returning 
	   RPS-10 Ready 
	*/

	return(RPCDisconnect(ctx));
}

/*
 * st_hostlist - API entry point to return the list of hosts 
 *                 for the devices on this NW_RPC100S unit
 * 
 *               This type of device is configured from the config file,
 *                 so we don't actually have to connect to figure this
 *                 out, just peruse the 'ctx' structure.
 * Returns:
 *     NULL on error
 *     a malloced array, terminated with a NULL,
 *         of null-terminated malloc'ed strings.
 */
char **
st_hostlist(Stonith  *s)
{
	char **		ret = NULL;	/* list to return */
	struct NW_RPC100S*	ctx;

	if (gbl_debug) printf ("Calling st_hostlist (%s)\n", WTIid);
	
	if (!ISNWRPC100S(s)) {
		syslog(LOG_ERR, "invalid argument to RPC_list_hosts");
		return(NULL);
	}
	if (!ISCONFIGED(s)) {
		syslog(LOG_ERR
		,	"unconfigured stonith object in RPC_list_hosts");
		return(NULL);
	}

	ctx = (struct NW_RPC100S*) s->pinfo;

	ret = (char **)MALLOC(2*sizeof(char*));
	if (ret == NULL) {
		syslog(LOG_ERR, "out of memory");
	} else {
		ret[0]=strdup(ctx->node);
		ret[1]=NULL;
	}

	return(ret);
} /* end si_hostlist() */

/*
 * st_freehostlist - free the result from st_hostlist() 
 */
void
st_freehostlist (char ** hlist)
{
	char **	hl = hlist;
	if (hl == NULL) {
		return;
	}
	while (*hl) {
		FREE(*hl);
		*hl = NULL;
		++hl;
	}
	FREE(hlist);
}


/*
 *	Parse the given configuration information, and stash it away...
 *
 *      <info> contains the parameters specific to this type of object
 *
 *         The format of <parameters> for this module is:
 *            <serial device> <remotenode> <outlet> [<remotenode> <outlet>] ...
 *
 *      e.g. A machine named 'nodea' can kill a machine named 'nodeb' through
 *           a device attached to serial port /dev/ttyS0.
 *           A machine named 'nodeb' can kill machines 'nodea' and 'nodec'
 *           through a device attached to serial port /dev/ttyS1 (outlets 0 
 *             and 1 respectively)
 *
 *      stonith nodea NW_RPC100S /dev/ttyS0 nodeb 0 
 *      stonith nodeb NW_RPC100S /dev/ttyS0 nodea 0 nodec 1
 *
 *      Another possible configuration is for 2 stonith devices accessible
 *         through 2 different serial ports on nodeb:
 *
 *      stonith nodeb NW_RPC100S /dev/ttyS0 nodea 0 
 *      stonith nodeb NW_RPC100S /dev/ttyS1 nodec 0
 */

static int
RPC_parse_config_info(struct NW_RPC100S* ctx, const char * info)
{
	char *copy;
	char *token;

	if (ctx->config) {
		/* The module is already configured. */
		return(S_OOPS);
	}

	/* strtok() is nice to use to parse a string with 
	   (other than it isn't threadsafe), but it is destructive, so
	   we're going to alloc our own private little copy for the
	   duration of this function.
	*/

	copy = strdup(info);
	if (!copy) {
		syslog(LOG_ERR, "out of memory");
		return S_OOPS;
	}

	/* Grab the serial device */
	token = strtok (copy, " \t");
	if (!token) {
		syslog(LOG_ERR, "%s: Can't find serial device on config line '%s'",
		       WTIid, info);
		goto token_error;		
	}

	ctx->device = strdup(token);
	if (!ctx->device) {
		syslog(LOG_ERR, "out of memory");
		goto token_error;
	}


	/* Grab <nodename>  */
	token = strtok (NULL, " \t");
	if (!token) {
		syslog(LOG_ERR, "%s: Can't find node name on config line '%s'",
		       WTIid, info);
		goto token_error;		
	}

	ctx->node = strdup(token);
	if (!ctx->node) {
		syslog(LOG_ERR, "out of memory");
		goto token_error;
	}
	
		
	/* free our private copy of the string we've been destructively 
	   parsing with strtok()
	*/
	free(copy);
	ctx->config = 1;
	return S_OK;

token_error:
	free(copy);
	return(S_BADCONFIG);
}


/*
 * RPCConnect -
 *
 * Connect to the given NW_RPC100S device.  
 * Side Effects
 *    ctx->fd now contains a valid file descriptor to the serial port
 *    ??? LOCK THE SERIAL PORT ???
 *  
 * Returns 
 *    S_OK on success
 *    S_OOPS on error
 *    S_TIMEOUT if the device did not respond
 *
 */
static int
RPCConnect(struct NW_RPC100S * ctx)
{
  	  
	/* Open the serial port if it isn't already open */
	if (ctx->fd < 0) {
		struct termios tio;

		ctx->fd = open (ctx->device, O_RDWR);
		if (ctx->fd <0) {
			syslog (LOG_ERR, "%s: Can't open %s : %s",
				WTIid, ctx->device, strerror(errno));
			return S_OOPS;
		}

		/* set the baudrate to 9600 8 - N - 1 */
		memset (&tio, 0, sizeof(tio));

		/* ??? ALAN - the -tradtitional flag on gcc causes the 
		   CRTSCTS constant to generate a warning, and warnings 
                   are treated as errors, so I can't set this flag! - EZA ???
		   
                   Hmmm. now that I look at the documentation, RTS
		   is just wired high on this device! we don't need it.
		*/
		/* tio.c_cflag = B9600 | CS8 | CLOCAL | CREAD | CRTSCTS ;*/
		tio.c_cflag = B9600 | CS8 | CLOCAL | CREAD ;
		tio.c_lflag = ICANON;

		if (tcsetattr (ctx->fd, TCSANOW, &tio) < 0) {
			syslog (LOG_ERR, "%s: Can't set attributes %s : %s",
				WTIid, ctx->device, strerror(errno));
			close (ctx->fd);
			ctx->fd=-1;
			return S_OOPS;
		}
		/* flush all data to and fro the serial port before we start */
		if (tcflush (ctx->fd, TCIOFLUSH) < 0) {
			syslog (LOG_ERR, "%s: Can't flush %s : %s",
				WTIid, ctx->device, strerror(errno));
			close (ctx->fd);
			ctx->fd=-1;
			return S_OOPS;		
		}
		
	}


	/* Send a BOGUS string */
	SEND("//0,0,BOGUS;\r\n", 10);
	
	/* Should reply with "Invalid Command" */
	if (gbl_debug) printf ("Waiting for \"Invalid Entry\"n");
	EXPECT(NWtokInvalidEntry, 12);
	if (gbl_debug) printf ("Got Invalid Entry\n");
	EXPECT(NWtokCRNL, 2);
	if (gbl_debug) printf ("Got NL\n");
	   
  return(S_OK);
}

static int
RPCDisconnect(struct NW_RPC100S * ctx)
{

  if (ctx->fd >= 0) {
    /* Flush the serial port, we don't care what happens to the characters
       and failing to do this can cause close to hang.
    */
    tcflush(ctx->fd, TCIOFLUSH);
    close (ctx->fd);
  }
  ctx->fd = -1;

  return S_OK;
} 

/*
 * RPCNametoOutlet - Map a hostname to an outlet number on this stonith device.
 *
 * Returns:
 *     0 on success ( the outlet number on the RPS10 - there is only one )
 *     -1 on failure (host not found in the config file)
 * 
 */
static int
RPCNametoOutlet ( struct NW_RPC100S * ctx, const char * host )
{
	if (!strcmp(ctx->node, host))
		return 0;

	return -1;
}


/*
 *	st_reset - API call to Reset (reboot) the given host on 
 *          this Stonith device.  This involves toggling the power off 
 *          and then on again, OR just calling the builtin reset command
 *          on the stonith device.
 */
int
st_reset(Stonith * s, int request, const char * host)
{
	int	rc = S_OK;
	int	lorc = S_OK;
	int outletnum = -1;
	struct NW_RPC100S*	ctx;
	
	if (gbl_debug) printf ("Calling st_reset (%s)\n", WTIid);
	
	if (!ISNWRPC100S(s)) {
		syslog(LOG_ERR, "invalid argument to RPC_reset_host");
		return(S_OOPS);
	}
	if (!ISCONFIGED(s)) {
		syslog(LOG_ERR
		,	"unconfigured stonith object in RPC_reset_host");
		return(S_OOPS);
	}
	ctx = (struct NW_RPC100S*) s->pinfo;

	if ((rc = RPCConnect(ctx)) != S_OK) {
		return(rc);
	}

	outletnum = RPCNametoOutlet(ctx, host);

	if (outletnum < 0) {
		syslog(LOG_WARNING, _("%s %s "
				      "doesn't control host [%s]."), 
		       ctx->idinfo, ctx->unitid, host);
		RPCDisconnect(ctx);
		return(S_BADHOST);
	}

	switch(request) {

#if defined(ST_POWERON) 
		case ST_POWERON:
			rc = RPCOn(ctx, outletnum, host);
			break;
#endif
#if defined(ST_POWEROFF)
		case ST_POWEROFF:
			rc = RPCOff(ctx, outletnum, host);
			break;
#endif
	case ST_GENERIC_RESET:
		rc = RPCReset(ctx, outletnum, host);
		break;
	default:
		rc = S_INVAL;
		break;
	}

	lorc = RPCDisconnect(ctx);

	return(rc != S_OK ? rc : lorc);
}

/*
 *	Parse the information in the given configuration file,
 *	and stash it away...
 */
int
st_setconffile(Stonith* s, const char * configname)
{
	FILE *	cfgfile;

	char	RPCid[256];

	struct NW_RPC100S*	ctx;

	if (!ISNWRPC100S(s)) {
		syslog(LOG_ERR, "invalid argument to RPC_set_configfile");
		return(S_OOPS);
	}
	ctx = (struct NW_RPC100S*) s->pinfo;

	if ((cfgfile = fopen(configname, "r")) == NULL)  {
		syslog(LOG_ERR, _("Cannot open %s"), configname);
		return(S_BADCONFIG);
	}

	while (fgets(RPCid, sizeof(RPCid), cfgfile) != NULL){

		RPC_parse_config_info(ctx, RPCid);
	}
	return(S_BADCONFIG);
}

/*
 *	st_setconfinfo - API entry point to process one line of config info 
 *       for this particular device.
 *
 *      Parse the config information in the given string, and stash it away...
 *
 */
int
st_setconfinfo(Stonith* s, const char * info)
{
	struct NW_RPC100S* ctx;

	if (!ISNWRPC100S(s)) {
		syslog(LOG_ERR, "RPC_provide_config_info: invalid argument");
		return(S_OOPS);
	}
	ctx = (struct NW_RPC100S *)s->pinfo;

	return(RPC_parse_config_info(ctx, info));
}

/*
 * st_getinfo - API entry point to retrieve something from the handle
 */
const char *
st_getinfo(Stonith * s, int reqtype)
{
	struct NW_RPC100S* ctx;
	char *		ret;

	if (!ISNWRPC100S(s)) {
		syslog(LOG_ERR, "RPC_idinfo: invalid argument");
		return NULL;
	}
	/*
	 *	We look in the ST_TEXTDOMAIN catalog for our messages
	 */
	ctx = (struct NW_RPC100S *)s->pinfo;

	switch (reqtype) {
		case ST_DEVICEID:
			ret = ctx->idinfo;
			break;

		case ST_CONF_INFO_SYNTAX:
			ret = _("<serial_device> <node>\n"
			"All tokens are white-space delimited.\n");
			break;

		case ST_CONF_FILE_SYNTAX:
			ret = _("<serial_device> <node>\n"
			"All tokens are white-space delimited.\n"
			"Blank lines and lines beginning with # are ignored");
			break;

		default:
			ret = NULL;
			break;
	}
	return ret;
}

/*
 * st_destroy - API entry point to destroy a NW_RPC100S Stonith object.
 */
void
st_destroy(Stonith *s)
{
	struct NW_RPC100S* ctx;

	if (!ISNWRPC100S(s)) {
		syslog(LOG_ERR, "nw_rpc100s_del: invalid argument");
		return;
	}
	ctx = (struct NW_RPC100S *)s->pinfo;

	ctx->WTIid = NOTwtiid;

	/*  close the fd if open and set ctx->fd to invalid */
	RPCDisconnect(ctx);
	
	if (ctx->device != NULL) {
		FREE(ctx->device);
		ctx->device = NULL;
	}
	if (ctx->idinfo != NULL) {
		FREE(ctx->idinfo);
		ctx->idinfo = NULL;
	}
	if (ctx->unitid != NULL) {
		FREE(ctx->unitid);
		ctx->unitid = NULL;
	}
}

/* 
 * st_new - API entry point called to create a new NW_RPC100S Stonith device
 *          object. 
 */
void *
st_new(void)
{
	struct NW_RPC100S*	ctx = MALLOCT(struct NW_RPC100S);

	if (ctx == NULL) {
		syslog(LOG_ERR, "out of memory");
		return(NULL);
	}
	memset(ctx, 0, sizeof(*ctx));
	ctx->WTIid = WTIid;
	ctx->fd = -1;
	ctx->config = 0;
	ctx->device = NULL;
	ctx->node = NULL;
	ctx->idinfo = NULL;
	ctx->unitid = NULL;
	REPLSTR(ctx->idinfo, DEVICE);
	REPLSTR(ctx->unitid, "unknown");

	return((void *)ctx);
}
