#ifndef DIRCACHEP_H
#define DIRCACHEP_H

struct dirCacheEntry_t {
	dirCacheEntryType_t type;
	unsigned int beginSlot;
	unsigned int endSlot;
	wchar_t *shortName;
	wchar_t *longName;
	struct directory dir;
	int endMarkPos;
} ;

int isHashed(dirCache_t *cache, wchar_t *name);
dirCacheEntry_t *addUsedEntry(dirCache_t *Stream,
			      unsigned int begin,
			      unsigned int end,
			      wchar_t *longName, wchar_t *shortName,
			      struct directory *dir);

#endif /* DIRCACHEP_H */
