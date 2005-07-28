/* $Id: st_ttylock.h,v 1.2 2005/07/28 09:05:45 sunjd Exp $ */
/*
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef __STONITH_ST_TTYLOCK_H
#	define __STONITH_ST_TTYLOCK_H
int	st_ttylock(const char *serial_device);
int	st_ttyunlock(const char *serial_device);
#endif	/*__STONITH_ST_TTYLOCK_H*/
