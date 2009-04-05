/*
 * libhfs - library for reading and writing Macintosh HFS volumes
 *
 * Copyright (C) 2000 Klaus Halfmann <klaus.halfmann@feri.de>
 * Original 1996-1998 Robert Leslie <rob@mars.org>
 * other work 2000 from Brad Boyer (flar@pants.nu)
 *
 * The HFS+ dates are stored as UInt32 containing the number of seconds since
 * midnight, January 1, 1904, GMT. This is slightly different from HFS, 
 * where the value represents local time. A notable exception is the
 * creationdate !. Linux uses times in GMT starting at  January 1, 1970
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */
 
#include <time.h>

    /* The number of seconds between 1.1.1904 and 1.1.1970 */
#define HFSPTIMEDIFF 2082844800U

   /* return the given apple time as UNIX time */
extern char* get_atime(UInt32 atime);

