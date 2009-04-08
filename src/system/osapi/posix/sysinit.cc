/* 
 *	HT Editor
 *	sysinit.cc - POSIX-specific initialization
 *
 *	Copyright (C) 1999-2002 Stefan Weyergraf
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License version 2 as
 *	published by the Free Software Foundation.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 *
 *	You should have received a copy of the GNU General Public License
 *	along with this program; if not, write to the Free Software
 *	Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "stdafx.h"

#ifndef TARGET_COMPILER_VC

#include <signal.h>
#include <unistd.h>
#include <sys/types.h>

#include "system/sys.h"

bool initOSAPI()
{
	setuid(getuid());
	return true;
}

void doneOSAPI()
{
}

#endif
