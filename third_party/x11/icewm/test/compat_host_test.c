/* Host golden test for the IceWM lane's libc gap fillers (icewm/compat/):
 * fnmatch / wordexp / glob compared against glibc.  Built and run by
 * build-icewm.sh (_iw_hosttest) with the host cc; the compat sources are
 * compiled with -Dname=hb_name so both implementations link side by side.
 * argv[1] = a directory tree to glob in (the extracted IceWM lib/). */
#define _GNU_SOURCE
#include <glob.h>
#include <fnmatch.h>
#include <wordexp.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
int hb_fnmatch(const char*, const char*, int);
typedef struct { size_t we_wordc; char **we_wordv; size_t we_offs; } hbw_t;
int hb_wordexp(const char*, hbw_t*, int); void hb_wordfree(hbw_t*);
typedef struct { size_t gl_pathc; char **gl_pathv; size_t gl_offs; int gl_flags; } hbg_t;
int hb_glob(const char*, int, int (*)(const char*, int), hbg_t*); void hb_globfree(hbg_t*);
/* hb glob flag values (icewm/include/glob.h) */
#define HB_GLOB_MARK 0x0002
#define HB_GLOB_NOCHECK 0x0010
int main(int argc, char **argv) {
  const char *pats[] = {"*.xpm","a?c","[a-c]*","[!a]*","*/*.png","\\*x","*","[[:digit:]]*","icons/*_16x16.*",".*","*a*b*c", "[]a]", "x[", 0};
  const char *strs[] = {"close.xpm","abc","bcd","a","icons/x.png","*x","","9z",".hidden","icons/run_16x16.xpm","aXbYc","]","x[", "a/b", 0};
  int fl[] = {0, FNM_PATHNAME, FNM_PERIOD, FNM_PATHNAME|FNM_PERIOD, FNM_NOESCAPE, FNM_CASEFOLD};
  int bad=0, n=0;
  for (int i=0;pats[i];i++) for(int j=0;strs[j];j++) for(int k=0;k<6;k++){ n++;
    int a=fnmatch(pats[i],strs[j],fl[k])!=0, b=hb_fnmatch(pats[i],strs[j],fl[k])!=0;
    if(a!=b){bad++; printf("fnmatch diff %s %s %d glibc=%d hb=%d\n",pats[i],strs[j],fl[k],a,b);} }
  setenv("HOME","/home",1); setenv("FOO","a b",1);
  const char *w[] = {"XEYESR.BIN -geometry 100x100", "'a b' \"c d\" e\\ f", "~/x $FOO \"$FOO\" ${FOO}x", "a|b", "$(ls)", "`ls`", "  lead  trail ", "\"unterminated", 0};
  for (int i=0;w[i];i++){ wordexp_t g; hbw_t h; memset(&g,0,sizeof g); memset(&h,0,sizeof h);
    int a=wordexp(w[i],&g,WRDE_NOCMD), b=hb_wordexp(w[i],&h,WRDE_NOCMD); n++;
    int same = (a==0)==(b==0);
    if (same && a==0) { same = g.we_wordc==h.we_wordc; for(size_t q=0;same&&q<g.we_wordc;q++) same=!strcmp(g.we_wordv[q],h.we_wordv[q]); }
    if(!same){bad++; printf("wordexp diff [%s] glibc=%d/%zu hb=%d/%zu\n",w[i],a,a?0:g.we_wordc,b,b?0:h.we_wordc);}
    if (!a) wordfree(&g);
    if (!b) hb_wordfree(&h);
  }
  if (argc > 1) {
    const char *gp[] = {"themes/*/default.theme", "icons/*_16x16.xpm", "th*/ice*", "nothing*", "themes/win95/*.xpm", "*", 0};
    char buf[4096];
    for (int i = 0; gp[i]; i++) for (int m = 0; m < 2; m++) {
      snprintf(buf, sizeof buf, "%s/%s", argv[1], gp[i]);
      glob_t g; hbg_t h; memset(&g, 0, sizeof g); memset(&h, 0, sizeof h);
      int a = glob(buf, m ? GLOB_MARK : 0, 0, &g), b = hb_glob(buf, m ? HB_GLOB_MARK : 0, 0, &h); n++;
      int same = (a == 0) == (b == 0);
      if (same && a == 0) { same = g.gl_pathc == h.gl_pathc; for (size_t q = 0; same && q < g.gl_pathc; q++) same = !strcmp(g.gl_pathv[q], h.gl_pathv[q]); }
      if (!same) { bad++; printf("glob diff [%s] mark=%d glibc=%d/%zu hb=%d/%zu\n", gp[i], m, a, a ? 0 : g.gl_pathc, b, b ? 0 : h.gl_pathc); }
      if (!a) globfree(&g);
      if (!b) hb_globfree(&h);
    }
  }
  printf("icewm compat host test: %d cases, %d diffs\n", n, bad); return bad!=0;
}
