/* $Id: main.c,v 1.18 2005/02/28 10:53:25 andrew Exp $ */
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
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>

#include <apphb.h>

#include <clplumbing/ipc.h>
#include <clplumbing/Gmain_timeout.h>
#include <clplumbing/cl_log.h>
#include <clplumbing/cl_signal.h>
#include <clplumbing/lsb_exitcodes.h>
#include <clplumbing/uids.h>
#include <clplumbing/realtime.h>
#include <clplumbing/GSource.h>
#include <clplumbing/cl_poll.h>
#include <clplumbing/coredumps.h>

#include <crm/crm.h>
#include <crm/common/ctrl.h>
#include <crm/common/ipc.h>
#include <crm/common/xml.h>

#include <crmd.h>
#include <crmd_fsa.h>
#include <crmd_messages.h>

#include <crm/dmalloc_wrapper.h>

const char* crm_system_name = SYS_NAME;
#define OPTARGS	"hV"

void usage(const char* cmd, int exit_status);
int init_start(void);
void crmd_hamsg_callback(const HA_Message * msg, void* private_data);
gboolean crmd_tickle_apphb(gpointer data);
extern void init_dotfile(void);

GMainLoop*  crmd_mainloop = NULL;

int
main(int argc, char ** argv)
{
    int flag;
    int	argerr = 0;
    struct stat buf;

#ifdef DEVEL_DIR
    mkdir(DEVEL_DIR, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
#endif

    crm_log_init(crm_system_name);

    if(stat(DEVEL_DIR, &buf) != 0) {
	    cl_perror("Stat of %s failed... exiting", DEVEL_DIR);
	    exit(100);
    }
    
    while ((flag = getopt(argc, argv, OPTARGS)) != EOF) {
		switch(flag) {
			case 'V':
				cl_log_enable_stderr(1);
				alter_debug(DEBUG_INC);
				break;
			case 'h':		/* Help message */
				usage(crm_system_name, LSB_EXIT_OK);
				break;
			default:
				++argerr;
				break;
		}
    }
    
    if (optind > argc) {
	    ++argerr;
    }
    
    if (argerr) {
	    usage(crm_system_name,LSB_EXIT_GENERIC);
    }
    
    /* read local config file */
    crm_devel("Enabling coredumps");
    if(cl_enable_coredumps(1) != 0) {
	    crm_warn("Cannot enable coredumps");
    }
 
    return init_start();
}


int
init_start(void)
{
    int exit_code = 0;
    enum crmd_fsa_state state;

    fsa_state = S_STARTING;
    fsa_input_register = 0; /* zero out the regester */

    init_dotfile();
    crm_info("Starting %s", crm_system_name);
    register_fsa_input(C_STARTUP, I_STARTUP, NULL);

    state = s_crmd_fsa(C_STARTUP);
    
    if (state == S_PENDING || state == S_STARTING) {
	    /* Create the mainloop and run it... */
	    crmd_mainloop = g_main_new(FALSE);
	    crm_info("Starting %s's mainloop", crm_system_name);
	    
#ifdef REALTIME_SUPPORT
	    static int  crm_realtime = 1;
	    if (crm_realtime == 1){
		    cl_enable_realtime();
	    }else if (crm_realtime == 0){
		    cl_disable_realtime();
	    }
	    cl_make_realtime(SCHED_RR, 5, 64, 64);
#endif
	    g_main_run(crmd_mainloop);
	    return_to_orig_privs();
	    if(is_set(fsa_input_register, R_STAYDOWN)) {
		    crm_info("Inhibiting respawn by Heartbeat");
		    exit_code = 100;
	    }

    } else {
	    crm_err("Startup of %s failed.  Current state: %s",
		    crm_system_name, fsa_state2string(state));
	    exit_code = 1;
    }
    
    crm_info("[%s] stopped (%d)", crm_system_name, exit_code);
    return exit_code;
}



void
usage(const char* cmd, int exit_status)
{
    FILE* stream;

    stream = exit_status ? stderr : stdout;

    fprintf(stream, "usage: %s [-srkh]"
			"[-c configure file]\n", cmd);
/* 	fprintf(stream, "\t-d\tsets debug level\n"); */
/* 	fprintf(stream, "\t-s\tgets daemon status\n"); */
/* 	fprintf(stream, "\t-r\trestarts daemon\n"); */
/* 	fprintf(stream, "\t-k\tstops daemon\n"); */
/* 	fprintf(stream, "\t-h\thelp message\n"); */
    fflush(stream);

    exit(exit_status);
}


gboolean
crmd_tickle_apphb(gpointer data)
{
    char	app_instance[APPNAME_LEN];
    int     rc = 0;
    sprintf(app_instance, "%s_%ld", crm_system_name, (long)getpid());

    rc = apphb_hb();
    if (rc < 0) {
		cl_perror("%s apphb_hb failure", app_instance);
		exit(3);
    }
    return TRUE;
}
