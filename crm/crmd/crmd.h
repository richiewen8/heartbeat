/* $Id: crmd.h,v 1.9 2004/08/18 15:20:22 andrew Exp $ */
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
#ifndef CRMD__H
#define CRMD__H

#include <ha_msg.h>

#define SYS_NAME     CRM_SYSTEM_CRMD
#define PID_FILE     WORKING_DIR "/"SYS_NAME".pid"
#define DAEMON_LOG   DEVEL_DIR"/"SYS_NAME".log"
#define DAEMON_DEBUG DEVEL_DIR"/"SYS_NAME".debug"

extern GMainLoop  *crmd_mainloop;

extern const char *crm_system_name;

extern GHashTable *ipc_clients;

#endif
