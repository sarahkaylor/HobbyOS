/*
 * libc_getopt_test.c — races our getopt_long (hb_*) against glibc's
 * across a scenario table: short clusters, required/optional args, long
 * options with abbreviation/ambiguity/=/flag, "--", permutation rules
 * (default, '+', '-'), error returns/optopt.  opterr is silenced on both
 * sides so only parse behavior is compared.
 *
 * For each scenario we run BOTH implementations to completion on identical
 * argv copies and compare the recorded sequences of
 *   (ret, optind, optarg, optopt, longindex)
 * element by element.  glibc's getopt_long is the ground truth.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>   /* glibc's getopt_long + struct option (ground truth) */

/* Our implementation compiled as hb_* (obj/host_hb_getopt.o).  Declared
   here rather than including our getopt.h, so this TU keeps glibc's
   <getopt.h> and calls both parsers side by side. */
extern int hb_getopt_long(int argc, char *const argv[],
                          const char *optstring,
                          const struct option *longopts, int *longindex);
extern char *hb_optarg;
extern int hb_optind, hb_opterr, hb_optopt, hb_optreset;

static int flag_storage = 0;

static struct option const longopts[] = { {"alpha", no_argument, NULL, 'a'}, {"beta", required_argument, NULL, 'b'}, {"gamma", optional_argument, NULL, 'g'}, {"delta", no_argument, &flag_storage, 'd'}, {"delta2", no_argument, NULL, 'D'}, {"break", no_argument, NULL, 'k'}, {NULL, 0, NULL, 0}
};

#define MAXRES 16
#define MAXARG 8

struct scen {
  const char *name;
  const char *argv[MAXARG];
  const char *optstring;
  int use_long;
};

static struct scen scens[] = { {"plain short", {"p", "-a", "-b"}, "ab", 0}, {"cluster", {"p", "-abc", "x"}, "abc", 0}, {"cluster+arg", {"p", "-abV", "x"}, "abV:", 0}, {"short=value", {"p", "-bval"}, "b:", 0}, {"short=space arg", {"p", "-b", "val"}, "b:", 0}, {"missing arg ?", {"p", "-b"}, "b:", 0}, {"missing arg :", {"p", "-b"}, ":b:", 0}, {"optional same-cluster", {"p", "-bval"}, "b::", 0}, {"optional next-arg", {"p", "-b", "val"}, "b::", 0}, {"unknown short", {"p", "-z"}, "ab", 0}, {"appear-trailing after opt", {"p", "-a", "--"}, "a", 0}, {"long simple", {"p", "--alpha"}, "", 1}, {"long =arg", {"p", "--beta", "v"}, "", 1}, {"long =val", {"p", "--beta=v"}, "", 1}, {"long abbreviation", {"p", "--alp"}, "", 1}, {"long ambiguous", {"p", "--d"}, "", 1}, {"long exact beats prefix", {"p", "--delta"}, "", 1}, {"long no-arg with value", {"p", "--alpa=x"}, "", 1}, {"long unknown", {"p", "--nope"}, "", 1}, {"long missing arg ?", {"p", "--beta"}, "", 1}, {"long missing arg :", {"p", "--beta"}, ":b:", 1}, {"long optional", {"p", "--gamma", "x"}, "", 1}, {"long optional =", {"p", "--gamma=x"}, "", 1}, {"dashdash", {"p", "--", "-a"}, "a", 0}, {"dashdash long", {"p", "--alpha", "--", "x"}, "", 1}, {"permute non-opt", {"p", "file", "-a"}, "a", 0}, {"permute several", {"p", "f1", "f2", "-a", "-b"}, "ab", 0}, {"posix + stops at non-opt", {"p", "file", "-a"}, "+a", 0}, {"leading - returns arg", {"p", "file", "-a"}, "-a", 0}, {"short then long", {"p", "-a", "--beta", "v"}, "a", 1}, {"unknown short in cluster", {"p", "-az"}, "a", 0}, {"long flag abrev", {"p", "--del"}, "", 1}, {"after long, short cluster", {"p", "--alpha", "-ab", "x"}, "ab", 1},
};

struct rec {
  int ret;
  int optind;
  char *optarg;   /* strdup'd; NULL kept NULL */
  int optopt;
  int longindex;
};

static int n_argv(const struct scen *s) {
  int n = 0;
  while (s->argv[n]) n++;
  return n;
}

static char **dup_argv(const struct scen *s) {
  int i, n = n_argv(s);
  char **av = malloc((size_t)(n + 1) * sizeof(char *));
  for (i = 0; i < n; i++)
    av[i] = (char *)s->argv[i];   /* getopt mutates them; that's fine
                                     since we re-dup per side */
  av[n] = NULL;
  return av;
}

/* Run one implementation to completion, appending each (ret, state) into
   out[]; returns the number of results. */
static int run_one(int (*f)(int, char *const[], const char *,
                            const struct option *, int *),
                   const struct option *opts, char *const *argv, int argc,
                   const char *optstring, struct rec out[],
                   int *optindg, char **optargg, int *optoptg) {
  int n = 0;
  while (n < MAXRES) {
    int longindex = -99;
    int r;
    /* glibc's optopt/optarg are sticky across calls: reset to
       sentinels so both sides compare identically per step. */
    *optoptg = 0;
    *optargg = NULL;
    r = f(argc, argv, optstring, opts, &longindex);
    out[n].ret = r;
    out[n].optind = *optindg;
    out[n].optarg = *optargg ? strdup(*optargg) : NULL;
    out[n].optopt = *optoptg;
    out[n].longindex = (r != -1) ? longindex : -1;
    n++;
    if (r == -1)
      break;
  }
  return n;
}

static void free_recs(struct rec r[], int n) {
  int i;
  for (i = 0; i < n; i++)
    free(r[i].optarg);
}

int main(void) {
  int failures = 0, seqno = 0;
  int scen_permt = 0;
  unsigned nscen = sizeof scens / sizeof scens[0];
  unsigned s;

  opterr = 0;      /* silence glibc */
  hb_opterr = 0;   /* silence ours */

  for (s = 0; s < nscen; s++) {
    struct scen *scen = &scens[s];
    struct rec hb[MAXRES], gl[MAXRES];
    char **avh = dup_argv(scen);
    char **avg = dup_argv(scen);
    int argc = n_argv(scen);
    int nh, ng, i;
    const struct option *opts = scen->use_long ? longopts : NULL;

    seqno++;
    flag_storage = 0;

    /* Permuted runs: glibc's mid-stream optind reflects its whole-block
       exchange (an implementation artifact); only the final row's
       optind is contractual (caller continues with the trailing
       non-options).  Detect whether argv has any non-option before a
       later option. */
    {
      int pi, pj, permt = 0;
      for (pi = 1; pi < argc && !permt; pi++)
        if (scen->argv[pi][0] != '-' || scen->argv[pi][1] == '\0')
          for (pj = pi + 1; pj < argc && !permt; pj++)
            if (scen->argv[pj][0] == '-' &&
                scen->argv[pj][1] != '\0')
              permt = 1;
      scen_permt = permt;
    }

    /* reset both parsers: ours via optreset, glibc by optind=0 */
    hb_optind = 1;
    hb_optarg = NULL;
    hb_optreset = 1;
    nh = run_one(hb_getopt_long, opts, avh, argc, scen->optstring, hb,
                 &hb_optind, &hb_optarg, &hb_optopt);
    optind = 0;
    optarg = NULL;
    optopt = 0;
    ng = run_one(getopt_long, opts, avg, argc, scen->optstring, gl,
                 &optind, &optarg, &optopt);

    if (nh != ng) {
      failures++;
      if (failures <= 20)
        printf("FAIL: seq %d [%s]: result count hb=%d glibc=%d\n",
               seqno, scen->name, nh, ng);
      free_recs(hb, nh);
      free_recs(gl, ng);
      free(avh);
      free(avg);
      continue;
    }

    for (i = 0; i < nh; i++) {
      int errcmp, licmp;
      /* optopt is only meaningful on '?'/':' error rows; glibc's
         is sticky elsewhere.  longindex is only filled when a long
         option parsed. */
      errcmp = (hb[i].ret == '?' || hb[i].ret == ':');
      licmp = (hb[i].ret != '?' && hb[i].ret != ':' &&
               hb[i].ret != -1);
      if (hb[i].ret != gl[i].ret ||
          (scen_permt && hb[i].ret != -1 ?
           (0) : (hb[i].optind != gl[i].optind)) ||
          (errcmp && hb[i].optopt != gl[i].optopt) ||
          (licmp && hb[i].longindex != gl[i].longindex) ||
          (hb[i].optarg == NULL) != (gl[i].optarg == NULL) ||
          (hb[i].optarg && gl[i].optarg &&
           strcmp(hb[i].optarg, gl[i].optarg) != 0)) {
        failures++;
        if (failures <= 20)
          printf("FAIL: seq %d [%s] step %d: hb(ret=%d oi=%d "
                 "arg=%s opt=%d li=%d) glibc(ret=%d oi=%d arg=%s "
                 "opt=%d li=%d)\n",
                 seqno, scen->name, i,
                 hb[i].ret, hb[i].optind,
                 hb[i].optarg ? hb[i].optarg : "(null)",
                 hb[i].optopt, hb[i].longindex,
                 gl[i].ret, gl[i].optind,
                 gl[i].optarg ? gl[i].optarg : "(null)",
                 gl[i].optopt, gl[i].longindex);
      }
    }
    free_recs(hb, nh);
    free_recs(gl, ng);
    free(avh);
    free(avg);
  }

  if (failures == 0)
    printf("getopt test: %u scenarios, 0 failures vs glibc\n", nscen);
  else
    printf("getopt test: %u scenarios, %d failures vs glibc\n",
           nscen, failures);
  return failures ? 1 : 0;
}
