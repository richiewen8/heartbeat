/* $Id: sect_delete1node1.c,v 1.1 2004/06/30 03:44:20 deng.pan Exp $ */
/* 
 * sec_delete1node1.c: Test data checkpoint function : saCkptSectionDelete 
 *
 * Copyright (C) 2003 Wilna Wei <willna.wei@intel.com>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
 
#define CkptOpenFlag SA_CKPT_CHECKPOINT_WRITE|SA_CKPT_CHECKPOINT_READ
#define CaseName "sect_delete1"
#define CkptCreationFlag SA_CKPT_WR_ACTIVE_REPLICA 
//#define CkptCreationFlag SA_CKPT_WR_ALL_REPLICAS 
#include "func.h"

int main(int argc, char **argv)
{
	int count =0 ;	

	if (inittest () != 0)
		{
			finalize () ;
			syslog (LOG_INFO|LOG_LOCAL7, "ckpt_fail\n") ;
			return -1 ;
		}

	/* tell monitor machine "I'm up now"*/ 	
	syslog (LOG_INFO|LOG_LOCAL7, "ckpt_start %d\n", getpid ()) ;
 	
	/* wait for node 2 (active node) ready */
	syslog (LOG_INFO|LOG_LOCAL7, "ckpt_signal %d %d\n", count++, SIGUSR1) ;
	pause () ;

	
	/* create local replica for slave node */
	if (opensync () < 0)
		{
			finalize () ;
			syslog (LOG_INFO|LOG_LOCAL7, "ckpt_fail\n") ;
			return -1 ;
		}
	
	/* wait for node 2 (active node) ready */
	syslog (LOG_INFO|LOG_LOCAL7, "ckpt_signal %d %d\n", count++, SIGUSR1) ;
	pause () ;

	if (deletesect() < 0)
		{
			finalize () ;
			syslog (LOG_INFO|LOG_LOCAL7, "ckpt_fail\n") ;
			return -1 ;
		}

	finalize1 () ;	
	
	/* wait for node 2 (active node) ready */
	syslog (LOG_INFO|LOG_LOCAL7, "ckpt_signal %d %d\n", count++, SIGUSR1) ;
	pause () ;

	return 0 ; 
}



