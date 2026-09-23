/*
 * HobbyOS Phase-2 sysroot: getopt.c — POSIX getopt() and GNU getopt_long().
 *
 * A self-contained implementation of the classic GNU algorithm:
 *   - argv permutation (GNU default; disabled by POSIXLY_CORRECT or a
 *     leading '+' in optstring; a leading '-' treats non-options as args)
 *   - option clustering ("-abc"), "--" terminator
 *   - required (':') and optional ("::") short-option arguments, taking
 *     the argument from the rest of the cluster when present
 *   - long options with unique-prefix abbreviation, exact-match priority,
 *     "--name=value", required/optional arguments
 *   - glibc-equivalent error messages on stderr (guarded by opterr) and
 *     glibc's return-value conventions ('?' / leading-':' -> ':')
 *
 * Host-tested byte-for-byte against glibc's getopt_long in
 * src/host/libc_getopt_test.c (with opterr silenced so error-message text
 * is not compared).  On the host this file is compiled by the standard
 * obj/host_hb_%.o pattern: the hb_ renames below apply first and
 * <getopt.h> then resolves to glibc's (same pattern as stdio.c/string.c).
 */
#ifdef HOST_TEST
#define optarg hb_optarg
#define optind hb_optind
#define optopt hb_optopt
#define opterr hb_opterr
#define optreset hb_optreset
#define getopt hb_getopt
#define getopt_long hb_getopt_long
#endif
#include <getopt.h>
#include <stdio.h>
#include <string.h>

char *optarg = NULL;
int optind = 1;
int opterr = 1;
int optopt = 0;
int optreset = 0;

static const char *nextchar = NULL;  /* pointer into current argv element */
static const char *g_progname = "";

/* Bring the next element to parse.  Returns:
 *    0  argv[optind] is an option to parse
 *    1  argv[optind] is a non-option (leading-'-' optstring mode: the
 *       caller returns it as a literal argument with ret 1)
 *   -1  no more options
 * Permutation (GNU default) rotates option elements forward one at a
 * time; it is disabled by POSIXLY_CORRECT/leading '+'. */
static int advance(int argc, char **argv, int posix_correct, int parse_nonopt)
{
    int i;

    if (optind >= argc)
        return -1;

    if (argv[optind][0] == '-' && argv[optind][1] != '\0') {
        if (argv[optind][1] == '-' && argv[optind][2] == '\0') {
            /* "--" terminates option parsing. */
            optind++;
            return -1;
        }
        return 0;              /* option-like element */
    }

    /* Non-option element at optind. */
    if (parse_nonopt)
        return 1;              /* opts "-c": caller returns it as an arg */
    if (posix_correct)
        return -1;

    /* Find the next option-like element (including "--") and rotate it
       to optind; if none, we are done. */
    for (i = optind + 1; i < argc; i++)
        if (argv[i][0] == '-' && argv[i][1] != '\0')
            break;
    if (i == argc)
        return -1;

    {
        char *tmp = argv[i];
        memmove(&argv[optind + 1], &argv[optind],
                (size_t)(i - optind) * sizeof(char *));
        argv[optind] = tmp;
    }
    return 0;
}

static int getopt_impl(int argc, char *const argv[], const char *optstring,
                       const struct option *longopts, int *longindex)
{
    int posix_correct = 0;
    int parse_nonopt = 0;
    int c;
    const char *os;

    if (optstring[0] == '+')
        posix_correct = 1;
    if (optstring[0] == '-')
        parse_nonopt = 1;

    if (optreset) {
        nextchar = NULL;
        optind = 1;
        optreset = 0;
    }

    g_progname = argv[0] ? argv[0] : "";

    /* Fetch the next argument to parse, if needed. */
    if (nextchar == NULL || *nextchar == '\0') {
        int st = advance(argc, (char **)argv, posix_correct, parse_nonopt);
        if (st == -1) {
            return -1;
        }
        if (st == 1) {
            /* Non-option as literal argument (optstring began with '-'). */
            optarg = argv[optind];
            optind++;
            return 1;
        }
        nextchar = argv[optind] + 1;
    }

    /* Long option: "--name..." */
    if (nextchar[0] == '-' && nextchar[1] != '\0') {
        const char *name = nextchar + 1;
        size_t namelen;
        const char *eq = strchr(nextchar + 1, '=');
        const struct option *p, *pfound = NULL;
        int ambig = 0, indfound = -1;

        namelen = eq ? (size_t)(eq - (nextchar + 1)) : strlen(name);
        optarg = NULL;

        for (p = longopts ? longopts : (const struct option *)0; p && p->name; p++) {
            size_t plen = strlen(p->name);
            if (plen == namelen && strncmp(p->name, name, namelen) == 0) {
                pfound = p;
                indfound = (int)(p - longopts);
                break;
            }
            if (namelen && plen > namelen &&
                strncmp(p->name, name, namelen) == 0) {
                if (pfound) {
                    ambig = 1;
                    break;
                }
                pfound = p;
                indfound = (int)(p - longopts);
            }
        }

        if (ambig) {
            if (opterr) {
                fprintf(stderr, "%s: option '--%s' is ambiguous\n",
                        g_progname, name);
            }
            nextchar = NULL;
            optind++;
            optopt = 0;
            return '?';
        }
        if (!pfound) {
            if (opterr) {
                fprintf(stderr, "%s: unrecognized option '--%s'\n",
                        g_progname, name);
            }
            nextchar = NULL;
            optind++;
            optopt = 0;
            return '?';
        }

        if (longindex)
            *longindex = indfound;
        nextchar = NULL;
        optind++;

        if (eq) {
            if (pfound->has_arg == no_argument) {
                if (opterr) {
                    fprintf(stderr,
                            "%s: option '--%s' doesn't allow an argument\n",
                            g_progname, pfound->name);
                }
                optopt = pfound->val;
                return '?';
            }
            optarg = (char *)eq + 1;
        } else if (pfound->has_arg == required_argument) {
            if (optind < argc) {
                optarg = argv[optind];
                optind++;
            } else {
                if (optstring[0] == ':') {
                    optopt = pfound->val;
                    return ':';
                }
                if (opterr) {
                    fprintf(stderr,
                            "%s: option '--%s' requires an argument\n",
                            g_progname, pfound->name);
                }
                optopt = pfound->val;
                return '?';
            }
        } else {
            optarg = NULL;   /* optional_argument, no '=' */
        }

        if (pfound->flag) {
            *pfound->flag = pfound->val;
            return 0;
        }
        return pfound->val;
    }

    /* Short option. */
    c = *nextchar;
    os = optstring;
    if (*os == '+' || *os == '-')
        os++;
    if (*os == ':')
        os++;                  /* leading ':' -> ':' on missing arg */

    for (; *os && *os != c; os++) {
        if (*os == ':')
            os++;
    }

    if (!*os) {
        /* Unknown option. */
        if (opterr) {
            fprintf(stderr, "%s: invalid option -- '%c'\n", g_progname, c);
        }
        optopt = c;
        nextchar = NULL;
        optind++;
        return '?';
    }

    if (os[1] == ':') {
        /* Option takes an argument. */
        if (os[2] == ':') {
            /* Optional argument: rest of cluster if any. */
            optarg = NULL;
            if (nextchar[1] != '\0')
                optarg = (char *)nextchar + 1;
            nextchar = NULL;
            optind++;
            return c;
        }
        /* Required argument. */
        if (nextchar[1] != '\0') {
            optarg = (char *)nextchar + 1;
            nextchar = NULL;
            optind++;
        } else if (optind + 1 < argc) {
            optarg = argv[optind + 1];
            optind += 2;
            nextchar = NULL;
        } else {
            nextchar = NULL;
            optind++;
            if (optstring[0] == ':') {
                optopt = c;
                return ':';
            }
            if (opterr) {
                fprintf(stderr,
                        "%s: option requires an argument -- '%c'\n",
                        g_progname, c);
            }
            optopt = c;
            return '?';
        }
        return c;
    }

    /* No argument: continue in the cluster.  When the cluster is
       exhausted, nextchar=NULL and optind advances to the next argv
       element (advance() then operates on it). */
    nextchar++;
    if (*nextchar == '\0') {
        nextchar = NULL;
        optind++;
    }
    return c;
}

int getopt(int argc, char *const argv[], const char *optstring)
{
    return getopt_impl(argc, argv, optstring, NULL, NULL);
}

int getopt_long(int argc, char *const argv[], const char *optstring,
                const struct option *longopts, int *longindex)
{
    return getopt_impl(argc, argv, optstring, longopts, longindex);
}
