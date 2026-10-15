#ifndef MTOOLS_VFAT_H
#define MTOOLS_VFAT_H

/*  Copyright 1995 David C. Niemi
 *  Copyright 1996-1998,2000-2003,2005,2007-2009 Alain Knaff.
 *  This file is part of mtools.
 *
 *  Mtools is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Mtools is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Mtools.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "msdos.h"

/* #define MAX_VFAT_SUBENTRIES 32 */ /* Theoretical max # of VSEs */
#define MAX_VFAT_SUBENTRIES 20		/* Max useful # of VSEs */
#define VSE_NAMELEN 13

/* Enough size for a worst case number of full VSEs plus a null */
#define VBUFSIZE ((MAX_VFAT_SUBENTRIES*VSE_NAMELEN) + 1)

/* Max legal length of a VFAT long name */
#define MAX_VNAMELEN (255)

/* Max length of VFAT long name in multibyte string */
#define MAX_VNAMELEN_MB (5*MAX_VNAMELEN)

#include "stream.h"

struct scan_state {
	int match_free;
	int shortmatch;
	int longmatch;
	unsigned int free_start;
	unsigned int free_end;
	unsigned int slot;
	int got_slots;
	unsigned int size_needed;
	unsigned int max_entry;
};

#include "mtoolsDirentry.h"

void autorename_short(struct dos_name_t *, int);
void autorename_long(char *, int);

#define DO_OPEN 1 /* open all files that are found */
#define ACCEPT_LABEL 0x08
#define ACCEPT_DIR 0x10
#define ACCEPT_PLAIN 0x20
#define MATCH_ANY 0x40
#define NO_MSG 0x80
#define NO_DOTS 0x100 /* accept no dots if matched by wildcard */
#define DO_OPEN_DIRS 0x400 /* open all directories that are found */
#define OPEN_PARENT 0x1000  /* in target lookup, open parent
			     * instead of file itself */

#define DEFERABLE 0x2000 /* When copying from a source with wildcards
			  * to a destination that is not a directory, defer the
			  * copy until the directory has been scanned fully, to
			  * make sure that no multiple files match the wildcard */

#endif
