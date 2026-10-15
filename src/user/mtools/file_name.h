#ifndef FILE_NAME_H
#define FILE_NAME_H

/*  Copyright 2009 Alain Knaff.
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

#include "sysincludes.h"
#include "mtools.h"

/**
 * raw dos-name coming straight from the directory entry
 * MYFILE  TXT
 */
struct dos_name_t {
  char base[8] NONULLTERM;
  char ext[3] NONULLTERM;
  char sentinel;
};

size_t dos_to_wchar(doscp_t *fromDos, const char *dos, wchar_t *wchar, size_t len);
void wchar_to_dos(doscp_t *toDos, wchar_t *wchar, char *dos, size_t len, int *mangled);

doscp_t *cp_open(unsigned int codepage);
void cp_close(doscp_t *cp);

size_t wchar_to_native(const wchar_t *wchar, char *native,
		       size_t len, size_t out_len);

#define WCHAR_TO_NATIVE(wchar,native,len) \
	wchar_to_native((wchar),(native),(len),sizeof(native))

size_t native_to_wchar(const char *native, wchar_t *wchar, size_t len,
		       const char *end, int *mangled);

int match(const wchar_t *, const wchar_t *, wchar_t *, int,  int);
wchar_t *unix_name(doscp_t *fromDos,
		   const char *base, const char *ext, uint8_t Case,
		   wchar_t *answer);

/* Is this a "special" directory entry, such as . or .. ? */
int isSpecial(const char *name);

#ifdef HAVE_WCHAR_H
int isSpecialW(const wchar_t *name);
#else
#define isSpecialW isSpecial
#endif

static inline wchar_t ch_towupper(wchar_t ch)
{
        return (wchar_t) towupper( (wint_t) ch);
}

static inline wchar_t ch_towlower(wchar_t ch)
{
        return (wchar_t) towlower( (wint_t) ch);
}

#endif
