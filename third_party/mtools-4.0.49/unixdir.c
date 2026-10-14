/*  Copyright 1998-2002,2009 Alain Knaff.
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
#include "stream.h"
#include "mtools.h"
#include "file.h"
#include "htable.h"
#include "mainloop.h"
#include "file_name.h"

#ifdef HAVE_READDIR

#ifdef HAVE_DIRENT_H
#include <dirent.h>
#endif

#ifdef HAVE_DIRECT_H
#include <direct.h>
#endif

#else

/* So far only supported platform that has no readdir is AT&T UnixPC.
 * Here, the following works: */
struct dirent {
	ino_t d_ino;
	char d_name[15]; /* One more than physically read, in order
			  * to have a terminating character */
};

typedef struct DIR {
	int fd;
	struct dirent entry;
} DIR;

static DIR *opendir(const char *filename) {
	DIR *dirp;
	int fd = open(filename, O_RDONLY);
	if(fd < 0)
		return NULL;
	dirp = calloc(1, sizeof(DIR));
	dirp->fd = fd;
	return dirp;
}

static struct dirent *readdir(DIR *dirp) {
	int n = read(dirp->fd, &dirp->entry, 16);
	if(n <= 0)
		return NULL;
	dirp->entry.d_name[14]='\0'; /* Terminating null */
	return &dirp->entry;
}

static int closedir(DIR* dirp) {
	close(dirp->fd);
	free(dirp);
	return 0;
}

#endif

typedef struct Dir_t {
	struct Stream_t head;

	struct MT_STAT statbuf;
	char *pathname;
	DIR *dir;
} Dir_t;

static int get_dir_data(Stream_t *Stream, time_t *date, mt_off_t *size,
			int *type, unsigned int *address)
{
	DeclareThis(Dir_t);

	if(date)
		*date = This->statbuf.st_mtime;
	if(size)
		*size = This->statbuf.st_size;
	if(type)
		*type = 1;
	if(address)
		*address = 0;
	return 0;
}

static int dir_free(Stream_t *Stream)
{
	DeclareThis(Dir_t);

	Free(This->pathname);
	closedir(This->dir);
	return 0;
}

static Class_t DirClass = {
	0, /* read */
	0, /* write */
	0, /* pread */
	0, /* pwrite */
	0, /* flush */
	dir_free, /* free */
	0, /* get_geom */
	get_dir_data ,
	0, /* pre-allocate */
	0, /* get_dosConvert */
	0 /* discard */
};

int unix_dir_loop(Stream_t *Stream, MainParam_t *mp)
{
	DeclareThis(Dir_t);
	struct dirent *entry;
	char *newName;
	int ret=0;

	while((entry=readdir(This->dir)) != NULL) {
		if(got_signal)
			break;
		if(isSpecial(entry->d_name))
			continue;
		newName = malloc(strlen(This->pathname) + 1 +
				 strlen(entry->d_name) + 1);
		if(!newName) {
			ret = ERROR_ONE;
			break;
		}
		strcpy(newName, This->pathname);
		strcat(newName, "/");
		strcat(newName, entry->d_name);

		ret |= unix_loop(Stream, mp, newName, 0);
		free(newName);
	}
	return ret;
}

Stream_t *OpenDir(const char *filename)
{
	Dir_t *This;

	This = New(Dir_t);
	init_head(&This->head, &DirClass, NULL);
	This->pathname = malloc(strlen(filename)+1);
	if(This->pathname == NULL) {
		Free(This);
		return NULL;
	}
	strcpy(This->pathname, filename);

	if(MT_STAT(filename, &This->statbuf) < 0) {
		Free(This->pathname);
		Free(This);
		return NULL;
	}

	This->dir = opendir(filename);
	if(!This->dir) {
		Free(This->pathname);
		Free(This);
		return NULL;
	}

	return &This->head;
}
