/* $Id: sizeprobe.h,v 1.3 2005/05/13 18:52:06 harbourn Exp $
 * dcfldd - The Enhanced Forensic DD
 * By Nicholas Harbour
 */

/* Copyright 85, 90, 91, 1995-2001, 2005 Free Software Foundation, Inc.
   Copyright 2008                        David Loveall <dave@loveall.org>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.  */

/* GNU dd originally written by Paul Rubin, David MacKenzie, and Stuart Kemp. */

#ifndef SIZEPROBE_H
#define SIZEPROBE_H

#include "dcfldd.h"
#include <sys/types.h>

enum {PROBE_NONE = 0, PROBE_INPUT, PROBE_OUTPUT, PROBE_INPUT_PROVIDED};

extern int probe;
extern off_t probed_size;

extern void sizeprobe(int);

#endif /* SIZEPROBE_H */
