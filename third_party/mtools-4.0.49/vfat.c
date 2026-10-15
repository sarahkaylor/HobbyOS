/*  Copyright 1995 David C. Niemi
 *  Copyright 1996-2003,2005,2007-2009 Alain Knaff.
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
 *
 * vfat.c
 *
 * Miscellaneous VFAT-related functions
 */

#include "sysincludes.h"
#include "mtools.h"
#include "vfat.h"
#include "file.h"
#include "dirCache.h"
#include "dirCacheP.h"
#include "file_name.h"
#include "stream.h"

/* #define DEBUG */


#define VSE1SIZE 5
#define VSE2SIZE 6
#define VSE3SIZE 2


struct vfat_subentry {
	unsigned char id;		/* 0x40 = last; & 0x1f = VSE ID */
	unsigned char text1[VSE1SIZE*2];
	unsigned char attribute;	/* 0x0f for VFAT */
	unsigned char hash1;		/* Always 0? */
	unsigned char sum;		/* Checksum of short name */
	unsigned char text2[VSE2SIZE*2];
	unsigned char sector_l;		/* 0 for VFAT */
	unsigned char sector_u;		/* 0 for VFAT */
	unsigned char text3[VSE3SIZE*2];
};

#define VSE_LAST 0x40
#define VSE_MASK 0x1f

struct vfat_state {
	wchar_t name[VBUFSIZE];
	int status; /* is now a bit map of 32 bits */
	unsigned int subentries;
	unsigned char sum; /* no need to remember the sum for each entry,
			    * it is the same anyways */
	int present;
};

const char *short_illegals=";+=[]',\"*\\<>/?:|";
const char *long_illegals = "\"*\\<>/?:|\005";

/* Format a number into a string, without terminating the string */
static void fmt_num(unsigned int num, char *base, int end, char prefix) {
	for(;end;end--) {
		base[end-1] = '0' + num % 10;
		num = num / 10;
	}
	base[end] = prefix;
}

/* Automatically derive a new name */
static void autorename(char *name,
		       char tilda, char dot, const char *illegals,
		       int limit, int bump)
{
	int tildapos, dotpos;
	unsigned int seqnum=0, maxseq=0;
	char *p;

#ifdef DEBUG
	printf("In autorename for name=%s.\n", name);
#endif
	tildapos = -1;

	for(p=name; *p ; p++)
		if (strchr(illegals, *p)) {
			*p = '_';
			bump = 0;
		}

	/* First step: find out whether the proposed name already ends
	   in a sequence number, and fill in tildapos if so */
	for(dotpos=0;
	    name[dotpos] && dotpos < limit && name[dotpos] != dot ;
	    dotpos++) {
		if(name[dotpos] == tilda) {
			tildapos = dotpos;
			seqnum = 0;
			maxseq = 1;
		} else if (name[dotpos] >= '0' && name[dotpos] <= '9') {
			seqnum = seqnum * 10 + (uint8_t)(name[dotpos] - '0');
			maxseq = maxseq * 10;
		} else
			tildapos = -1; /* "sequence number" followed
					* by non-numeric characters,
					* i.e. consider as not present */
	}
	if(tildapos == -1) {
		/* if no sequence number found at step 1, make space for one */
		if(dotpos > limit - 2) {
			tildapos = limit - 2;
			dotpos = limit;
		} else {
			tildapos = dotpos;
			dotpos += 2;
		}
		seqnum = 1;
	} else {
		if(bump)
			seqnum++;
		if(seqnum > 999999) {
			seqnum = 1;
			tildapos = dotpos - 2;
			/* produces a short sequence number within the former
			 * big sequence number: A~9999~1 TST */
		}
		if (seqnum == maxseq) {
			/* series of nines => make space for one more
			 * digit */
			if(dotpos >= limit)
				tildapos--;
			else
				dotpos++;
		}
	}

	if((bump && seqnum == 1) || seqnum > 1 || mtools_numeric_tail)
		fmt_num(seqnum, name+tildapos, dotpos-tildapos, tilda);

	/* terminate the string if long name */
	if(!dot)
		name[dotpos]='\0';
#ifdef DEBUG
	printf("Out autorename for name=%s.\n", name);
#endif
}


void autorename_short(dos_name_t *name, int bump)
{
	autorename(name->base, '~', ' ', short_illegals, 8, bump);
}

void autorename_long(char *name, int bump)
{
	autorename(name, '-', '\0', long_illegals, 255, bump);
}

/* If null encountered, set *end to 0x40 and write nulls rest of way
 * 950820: Win95 does not like this!  It complains about bad characters.
 * So, instead: If null encountered, set *end to 0x40, write the null, and
 * write 0xff the rest of the way (that is what Win95 seems to do; hopefully
 * that will make it happy)
 */
/* Always return num */
static int unicode_write(wchar_t *in, unsigned char *out, int num, int *end_p)
{
	int j;

	for (j=0; j<num; ++j) {
		if (*end_p)
			/* Fill with 0xff */
			out[0] = out[1] = 0xff;
		else {
			/* TODO / FIXME : handle case where wchat has more
			 * than 2 bytes (i.e. bytes 2 or 3 are set.
			 * ==> generate surrogate pairs?
			 */
			out[1] = (*in & 0xffff) >> 8;
			out[0] = *in & 0xff;
			if (! *in) {
				*end_p = VSE_LAST;
			}
		}

		++out;
		++out;
		++in;
	}
	return num;
}


static inline int unicode_read(unsigned char *in,
				   wchar_t *out, int num)
{
	wchar_t *end_out = out+num;

	while(out < end_out) {
#ifdef HAVE_WCHAR_H
		*out = in[0] | ((in[1]) << 8);
#else
		if (in[1])
			*out = '_';
		else
			*out = (char) in[0];
#endif
		++out;
		++in;
		++in;
	}
	return num;
}


static void clear_vfat(struct vfat_state *v)
{
	v->subentries = 0;
	v->status = 0;
	v->present = 0;
}


/* sum_shortname
 *
 * Calculate the checksum that results from the short name in *dir.
 *
 * The sum is formed by circularly right-shifting the previous sum
 * and adding in each character, from left to right, padding both
 * the name and extension to maximum length with spaces and skipping
 * the "." (hence always summing exactly 11 characters).
 *
 * This exact algorithm is required in order to remain compatible
 * with Microsoft Windows-95 and Microsoft Windows NT 3.5.
 * Thanks to Jeffrey Richter of Microsoft Systems Journal for
 * pointing me to the correct algorithm.
 *
 * David C. Niemi (niemi@tuxers.net) 95.01.19
 */
static inline unsigned char sum_shortname(const dos_name_t *dn)
{
	unsigned char sum;
	const char *name=dn->base;
	const char *end = name+11;

	for (sum=0; name<end; ++name)
		sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1)
			+ (uint8_t) *name;
	return(sum);
}

/* check_vfat
 *
 * Inspect a directory and any associated VSEs.
 * Return 1 if the VSEs comprise a valid long file name,
 * 0 if not.
 */
static inline void check_vfat(struct vfat_state *v, struct directory *dir)
{
	dos_name_t dn;

	if (! v->subentries) {
#ifdef DEBUG
		fprintf(stderr, "check_vfat: no VSEs.\n");
#endif
		return;
	}

	memcpy(dn.base, (char *)dir->name, 8);
	memcpy(dn.ext, (char *)dir->ext, 3);

	if (v->sum != sum_shortname(&dn))
		return;

	if( (v->status & ((1<<v->subentries) - 1)) != (1<<v->subentries) - 1)
		return; /* missing entries */

	/* zero out byte following last entry, for good measure */
	v->name[VSE_NAMELEN * v->subentries] = 0;
	v->present = 1;
}

unsigned int write_vfat(Stream_t *Dir, dos_name_t *shortname, char *longname,
			unsigned int start,
			direntry_t *mainEntry)
{
	struct vfat_subentry *vse;
	uint8_t vse_id, num_vses;
	wchar_t *c;
	direntry_t entry;
	dirCache_t *cache;
	wchar_t unixyName[13];
	doscp_t *cp = GET_DOSCONVERT(Dir);

	wchar_t wlongname[MAX_VNAMELEN+1];
	size_t wlen;

	if(longname) {
#ifdef DEBUG
		printf("Entering write_vfat with longname=\"%s\", start=%d.\n",
		       longname,start);
#endif
		entry.Dir = Dir;
		vse = (struct vfat_subentry *) &entry.dir;
		/* Fill in invariant part of vse */
		vse->attribute = 0x0f;
		vse->hash1 = vse->sector_l = vse->sector_u = 0;
		vse->sum = sum_shortname(shortname);
#ifdef DEBUG
		printf("Wrote checksum=%d for shortname %s.%s\n",
		       vse->sum,shortname->base,shortname->ext);
#endif

		wlen = native_to_wchar(longname, wlongname, MAX_VNAMELEN,
				       0, 0);
		num_vses = (uint8_t)((wlen + VSE_NAMELEN - 1)/VSE_NAMELEN);
		for (vse_id = num_vses; vse_id; --vse_id) {
			int end = 0;

			c = wlongname + (vse_id - 1) * VSE_NAMELEN;

			c += unicode_write(c, vse->text1, VSE1SIZE, &end);
			c += unicode_write(c, vse->text2, VSE2SIZE, &end);
			c += unicode_write(c, vse->text3, VSE3SIZE, &end);

			vse->id = (vse_id == num_vses) ? (vse_id | VSE_LAST) : vse_id;
#ifdef DEBUG
			printf("Writing longname=(%s), VSE %d (%13s) at %d, end = %d.\n",
			       longname, vse_id, longname + (vse_id-1) * VSE_NAMELEN,
			       start + num_vses - vse_id, start + num_vses);
#endif

			setEntryToPos(&entry, start + num_vses - vse_id);
			low_level_dir_write(&entry);
		}
	} else {
		num_vses = 0;
		wlongname[0]='\0';
	}
	cache = allocDirCache(Dir, start + num_vses + 1);
	if(!cache) {
		fprintf(stderr, "Out of memory error\n");
		exit(1);
	}
	unix_name(cp, shortname->base, shortname->ext, 0, unixyName);
	addUsedEntry(cache, start, start + num_vses + 1, wlongname, unixyName,
		     &mainEntry->dir);
	low_level_dir_write(mainEntry);
	return start + num_vses;
}

void dir_write(direntry_t *entry)
{
	dirCacheEntry_t *dce;
	dirCache_t *cache;

	if(isRootEntry(entry)) {
		fprintf(stderr, "Attempt to write root directory pointer\n");
		exit(1);
	}

	cache = allocDirCache(entry->Dir, getNextEntryAsPos(entry));
	if(!cache) {
		fprintf(stderr, "Out of memory error in dir_write\n");
		exit(1);
	}
	dce = cache->entries[entry->entry];
	if(dce) {
		if(entry->dir.name[0] == DELMARK) {
			addFreeEntry(cache, dce->beginSlot, dce->endSlot);
		} else {
			dce->dir = entry->dir;
		}
	}
	low_level_dir_write(entry);
}


/*
 * The following function translates a series of vfat_subentries into
 * data suitable for a dircache entry
 */
static inline void parse_vses(direntry_t *entry,
				  struct vfat_state *v)
{
	struct vfat_subentry *vse;
	unsigned char id, last_flag;
	wchar_t *c;

	vse = (struct vfat_subentry *) &entry->dir;

	id = vse->id & VSE_MASK;
	last_flag = (vse->id & VSE_LAST);
	if (id > MAX_VFAT_SUBENTRIES) {
		fprintf(stderr, "parse_vses: invalid VSE ID %d at %d.\n",
			id, entry->entry);
		return;
	}

/* 950819: This code enforced finding the VSEs in order.  Well, Win95
 * likes to write them in *reverse* order for some bizarre reason!  So
 * we pretty much have to tolerate them coming in any possible order.
 * So skip this check, we'll do without it (What does this do, Alain?).
 *
 * 950820: Totally rearranged code to tolerate any order but to warn if
 * they are not in reverse order like Win95 uses.
 *
 * 950909: Tolerate any order. We recognize new chains by mismatching
 * checksums. In the event that the checksums match, new entries silently
 * overwrite old entries of the same id. This should accept all valid
 * entries, but may fail to reject invalid entries in some rare cases.
 */

	/* bad checksum, begin new chain */
	if(v->sum != vse->sum) {
		clear_vfat(v);
		v->sum = vse->sum;
	}

#ifdef DEBUG
	if(v->status & (1 << (id-1)))
		fprintf(stderr,
			"parse_vses: duplicate VSE %d\n", vse->id);
#endif

	v->status |= 1 << (id-1);
	if(last_flag)
		v->subentries = id;

#ifdef DEBUG
	if (id > v->subentries)
		/* simple test to detect entries preceding
		 * the "last" entry (really the first) */
		fprintf(stderr,
			"parse_vses: new VSE %d sans LAST flag\n",
			vse->id);
#endif

	c = &(v->name[VSE_NAMELEN * (id-1)]);
	c += unicode_read(vse->text1, c, VSE1SIZE);
	c += unicode_read(vse->text2, c, VSE2SIZE);
	c += unicode_read(vse->text3, c, VSE3SIZE);
#ifdef DEBUG
	printf("Read VSE %d at %d, subentries=%d, = (%13ls).\n",
	       id,entry->entry,v->subentries,&(v->name[VSE_NAMELEN * (id-1)]));
#endif
	if (last_flag)
		*c = '\0';	/* Null terminate long name */
}

/**
 * Read one complete entry from directory (main name plus any VSEs
 * belonging to it)
 */
static dirCacheEntry_t *vfat_lookup_loop_common(doscp_t *cp,
						direntry_t *direntry,
						dirCache_t *cache,
						int lookForFreeSpace,
						int *io_error)
{
	wchar_t newfile[13];
	unsigned int initpos = getNextEntryAsPos(direntry);
	struct vfat_state vfat;
	wchar_t *longname;
	int error;
	int endmarkSeen = 0;

	/* not yet cached */
	*io_error = 0;
	clear_vfat(&vfat);
	while(1) {
		++direntry->entry;
		if(!dir_read(direntry, &error)){
			if(error) {
			    *io_error = error;
			    return NULL;
			}
			addFreeEndEntry(cache, initpos,
					getEntryAsPos(direntry),
					endmarkSeen);
			return addEndEntry(cache, getEntryAsPos(direntry));
		}

		if (endmarkSeen || direntry->dir.name[0] == ENDMARK){
				/* the end of the directory */
			if(lookForFreeSpace) {
				endmarkSeen = 1;
				continue;
			}
			return addEndEntry(cache, getEntryAsPos(direntry));
		}
		if(direntry->dir.name[0] != DELMARK &&
		   direntry->dir.attr == 0x0f)
			parse_vses(direntry, &vfat);
		else
			/* the main entry */
			break;
	}

	/* If we get here, it's a short name FAT entry, maybe erased.
	 * thus we should make sure that the vfat structure will be
	 * cleared before the next loop run */

	/* deleted file */
	if (direntry->dir.name[0] == DELMARK) {
		return addFreeEntry(cache, initpos,
				    getNextEntryAsPos(direntry));
	}

	check_vfat(&vfat, &direntry->dir);
	if(!vfat.present)
		vfat.subentries = 0;

	/* mark space between last entry and this one as free */
	addFreeEntry(cache, initpos,
		     getEntryAsPos(direntry) - vfat.subentries);

	if (direntry->dir.attr & 0x8){
		/* Read entry as a label */
		wchar_t *ptr = newfile;
		if (direntry->dir.name[0] == '\x05') {
			ptr += dos_to_wchar(cp, "\xE5", ptr, 1);
			ptr += dos_to_wchar(cp, direntry->dir.name+1, ptr, 7);
		} else {
			ptr += dos_to_wchar(cp, direntry->dir.name, ptr, 8);
		}
		ptr += dos_to_wchar(cp, direntry->dir.ext, ptr, 3);
		*ptr = '\0';
	} else
		unix_name(cp,
			  direntry->dir.name,
			  direntry->dir.ext,
			  direntry->dir.Case,
			  newfile);

	if(vfat.present)
		longname = vfat.name;
	else
		longname = 0;

	return addUsedEntry(cache, getEntryAsPos(direntry) - vfat.subentries,
			    getNextEntryAsPos(direntry), longname,
			    newfile, &direntry->dir);
}

static inline dirCacheEntry_t *vfat_lookup_loop_for_read(doscp_t *cp,
							     direntry_t *direntry,
							     dirCache_t *cache,
							     int *io_error)
{
	int initpos = direntry->entry + 1;
	dirCacheEntry_t *dce;

	*io_error = 0;
	dce = cache->entries[initpos];
	if(dce) {
		setEntryToPos(direntry, dce->endSlot - 1);
		return dce;
	} else {
		return vfat_lookup_loop_common(cp,
					       direntry, cache, 0, io_error);
	}
}


typedef enum result_t {
	RES_NOMATCH,
	RES_MATCH,
	RES_END,
	RES_ERROR
} result_t;


/*
 * 0 does not match
 * 1 matches
 * 2 end
 */
static result_t checkNameForMatch(struct direntry_t *direntry,
				  dirCacheEntry_t *dce,
				  const wchar_t *filename,
				  int length,
				  int flags)
{
	switch(dce->type) {
		case DCET_FREE:
			return RES_NOMATCH;
		case DCET_END:
			return RES_END;
		case DCET_USED:
			break;
#ifdef DEBUG
# if defined HAVE_PRAGMA_DIAGNOSTIC && defined __clang__
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wcovered-switch-default"
# endif
		default:
			fprintf(stderr, "Unexpected entry type %d\n",
				dce->type);
			return RES_ERROR;
# if defined HAVE_PRAGMA_DIAGNOSTIC && defined __clang__
#  pragma clang diagnostic pop
# endif
#endif
	}

	direntry->dir = dce->dir;

	/* make sure the entry is of an accepted type */
	if((direntry->dir.attr & 0x8) && !(flags & ACCEPT_LABEL))
		return RES_NOMATCH;


	/*---------- multiple files ----------*/
	if(!((flags & MATCH_ANY) ||
	     (dce->longName &&
	      match(dce->longName, filename, direntry->name, 0, length)) ||
	     match(dce->shortName, filename, direntry->name, 1, length))) {

		return RES_NOMATCH;
	}

	/* entry of non-requested type, has to come after name
	 * checking because of clash handling */
	if(IS_DIR(direntry) && !(flags & ACCEPT_DIR)) {
		if(!(flags & (ACCEPT_LABEL|MATCH_ANY|NO_MSG))) {
			char tmp[4*13+1];
			WCHAR_TO_NATIVE(dce->shortName,tmp,13);
			fprintf(stderr, "Skipping \"%s\", is a directory\n",
				tmp);
		}
		return RES_NOMATCH;
	}

	if(!(direntry->dir.attr & (ATTR_LABEL | ATTR_DIR)) &&
	   !(flags & ACCEPT_PLAIN)) {
		if(!(flags & (ACCEPT_LABEL|MATCH_ANY|NO_MSG))) {
			char tmp[4*13+1];
			WCHAR_TO_NATIVE(dce->shortName,tmp,13);
			fprintf(stderr,
				"Skipping \"%s\", is not a directory\n",
				tmp);
		}
		return RES_NOMATCH;
	}

	return RES_MATCH;
}


int vfat_lookup_zt(direntry_t *direntry, const char *filename,
		   int flags, char *shortname, size_t shortname_size,
		   char *longname, size_t longname_size) {
	return vfat_lookup(direntry, filename, strlen(filename),
			   flags, shortname, shortname_size,
			   longname, longname_size);
}

/*
 * vfat_lookup looks for filenames in directory dir.
 * if a name if found, it is returned in outname
 * if applicable, the file is opened and its stream is returned in File
 */

int vfat_lookup(direntry_t *direntry, const char *filename,
		size_t length,
		int flags, char *shortname, size_t shortname_size,
		char *longname, size_t longname_size)
{
	dirCacheEntry_t *dce;
	result_t result;
	dirCache_t *cache;
	int io_error;
	wchar_t wfilename[MAX_VNAMELEN+1];
	doscp_t *cp = GET_DOSCONVERT(direntry->Dir);

	if(filename != NULL)
		length = native_to_wchar(filename, wfilename, MAX_VNAMELEN,
					 filename+length, 0);
	else
		length = 0;

	if (isNotFound(direntry))
		return -1;

	cache = allocDirCache(direntry->Dir, getNextEntryAsPos(direntry));
	if(!cache) {
		fprintf(stderr, "Out of memory error in vfat_lookup [0]\n");
		exit(1);
	}

	do {
		dce = vfat_lookup_loop_for_read(cp, direntry, cache, &io_error);
		if(!dce) {
			if (io_error)
				return -2;
			fprintf(stderr, "Out of memory error in vfat_lookup\n");
			exit(1);
		}
		result = checkNameForMatch(direntry, dce,
					   wfilename,
					   (int) length, flags);
	} while(result == RES_NOMATCH);

	if(result == RES_MATCH){
		if(longname){
			if(dce->longName)
				wchar_to_native(dce->longName, longname,
						MAX_VNAMELEN, longname_size);
			else
				*longname ='\0';
		}
		if(shortname)
			wchar_to_native(dce->shortName, shortname,
					12, shortname_size);
		direntry->beginSlot = dce->beginSlot;
		direntry->endSlot = dce->endSlot-1;
		return 0; /* file found */
	} else {
		direntry->entry = NOT_FOUND_ENTRY;
		return -1; /* no file found */
	}
}

static inline dirCacheEntry_t *vfat_lookup_loop_for_insert(doscp_t *cp,
							   direntry_t *direntry,
							   unsigned int initpos,
							   dirCache_t *cache)
{
	dirCacheEntry_t *dce;
	int io_error;

	dce = cache->entries[initpos];
	if(dce && dce->type != DCET_END) {
		return dce;
	} else {
		setEntryForIteration(direntry, initpos);
		dce = vfat_lookup_loop_common(cp,
					      direntry, cache, 1, &io_error);
		if(!dce) {
			if (io_error) {
				return NULL;
			}
			fprintf(stderr,
				"Out of memory error in vfat_lookup_loop\n");
			exit(1);
		}
		return cache->entries[initpos];
	}
}

static void accountFreeSlots(struct scan_state *ssp, dirCacheEntry_t *dce)
{
	if(ssp->got_slots)
		return;

	if(ssp->free_end != dce->beginSlot) {
		ssp->free_start = dce->beginSlot;
	}
	ssp->free_end = dce->endSlot;

	if(ssp->free_end - ssp->free_start >= ssp->size_needed) {
		ssp->got_slots = 1;
		ssp->slot = ssp->free_start + ssp->size_needed - 1;
	}
}

static void clear_scan(wchar_t *longname, int use_longname,
		       struct scan_state *s)
{
	s->shortmatch = s->longmatch = -1;
	s->free_end = s->got_slots = s->free_start = 0;

	if (use_longname & 1)
		s->size_needed = (unsigned)
			(1 + (wcslen(longname) + VSE_NAMELEN - 1)/VSE_NAMELEN);
	else
                s->size_needed = 1;
}

/* lookup_for_insert replaces the old scandir function.  It directly
 * calls into vfat_lookup_loop, thus eliminating the overhead of the
 * normal vfat_lookup
 */
int lookupForInsert(Stream_t *Dir,
		    struct direntry_t *direntry,
		    dos_name_t *dosname,
		    const char *longname,
		    struct scan_state *ssp,
		    int ignore_entry,
		    int source_entry,
		    int pessimisticShortRename,
		    int use_longname,
		    int ignore_match)
{
	direntry_t entry;
	int ignore_short_match;
	dirCacheEntry_t *dce;
	dirCache_t *cache;
	unsigned int pos; /* position _before_ the next answered entry */
	wchar_t shortName[13];
	wchar_t wlongname[MAX_VNAMELEN+2];
	doscp_t *cp = GET_DOSCONVERT(Dir);

	/* Deliberately convert one character too much in order
	 * to catch file names that are too long */
	if(native_to_wchar(longname, wlongname, MAX_VNAMELEN+1, 0, 0) >
	   MAX_VNAMELEN) {
	    fprintf(stderr, "Name %s too long\n", longname);
	    return 0; /* Single file error */
	}
	    
	clear_scan(wlongname, use_longname, ssp);

	ignore_short_match = (ignore_entry == -2 ) || ignore_match;

	initializeDirentry(&entry, Dir);
	ssp->match_free = 0;

	/* hash bitmap of already encountered names.  Speeds up batch appends
	 * to huge directories, because in the best case, we only need to scan
	 * the new entries rather than the whole directory */
	cache = allocDirCache(Dir, 1);
	if(!cache) {
		fprintf(stderr, "Out of memory error in lookupForInsert\n");
		exit(1);
	}

	if(!ignore_short_match)
		unix_name(cp, dosname->base, dosname->ext, 0, shortName);

	pos = cache->nrHashed;
	if(source_entry >= 0 ||
	   (pos && isHashed(cache, wlongname))) {
		pos = 0;
	} else if(pos && !ignore_short_match && isHashed(cache, shortName)) {
		if(pessimisticShortRename) {
			ssp->shortmatch = -2;
			return 1;
		}
		pos = 0;
	} else if(growDirCache(cache, pos) < 0) {
		fprintf(stderr, "Out of memory error in vfat_looup [0]\n");
		exit(1);
	}
	do {
		dce = vfat_lookup_loop_for_insert(cp, &entry, pos, cache);
		switch(dce->type) {
			case DCET_FREE:
				accountFreeSlots(ssp, dce);
				break;
			case DCET_USED:
				if(!(dce->dir.attr & 0x8) &&
				   (signed int)dce->endSlot-1 == source_entry)
				   accountFreeSlots(ssp, dce);

				/* labels never match, neither does the
				 * ignored entry */
				if( (dce->dir.attr & 0x8) ||
				    ((signed int)dce->endSlot-1==ignore_entry))
					break;

				if(ignore_match)
				    break;

				/* check long name */
				if((dce->longName &&
				    !wcscasecmp(dce->longName, wlongname)) ||
				   (dce->shortName &&
				    !wcscasecmp(dce->shortName, wlongname))) {
					ssp->longmatch =
						(int) (dce->endSlot - 1);
					/* long match is a reason for
					 * immediate stop */
					direntry->beginSlot = dce->beginSlot;
					direntry->endSlot = dce->endSlot - 1;
					return 1;
				}

				/* Long name or not, always check for
				 * short name match */
				if (!ignore_short_match &&
				    !wcscasecmp(shortName, dce->shortName))
					ssp->shortmatch =
						(int) (dce->endSlot - 1);
				break;
			case DCET_END:
				break;
		}
		pos = dce->endSlot;
	} while(dce->type != DCET_END);
	if (ssp->shortmatch > -1)
		return 1;
	ssp->max_entry = dce->beginSlot;
	if (ssp->got_slots)
		return 6;	/* Success */

	/* Need more room.  Can we grow the directory? */
	if(!isRootDir(Dir))
		return 5;	/* OK, try to grow the directory */

	fprintf(stderr, "No directory slots\n");
	return -1;
}


/* End vfat.c */
