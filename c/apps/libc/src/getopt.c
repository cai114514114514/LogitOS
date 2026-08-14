/* getopt / getopt_long -- absent from this library until now, and the first
 * wall almost every ported CLI program hits.
 *
 * WHY THIS MATCHES GLIBC AND NOT musl, WHICH IS THE SMALLER TARGET.
 * The two disagree about ARGV PERMUTATION: given `prog file -v`, glibc's
 * getopt() returns 'v' and leaves `file` at argv[optind] afterwards, while
 * musl's stops at `file` and never sees -v at all. Both are defensible -- POSIX
 * specifies musl's -- but almost every program that has ever been written
 * against getopt was written against glibc's, and a program whose options stop
 * working after the first filename does not look like a libc difference, it
 * looks like the program being broken. It also means this file can be held to a
 * REFERENCE: tests/unit/libc_getopt_test.c runs the identical argv vectors
 * through glibc on the host and diffs. Matching musl would have made every one
 * of those diffs a false positive and left this with no oracle at all.
 *
 * Supported, all of it glibc behaviour:
 *   - clustered shorts (-abc), attached and detached arguments (-ofile, -o file)
 *   - ':' after a letter = required argument, '::' = optional (attached only)
 *   - leading '+' in optstring, or POSIXLY_CORRECT in the environment: stop at
 *     the first non-option (this is why the environment work came first)
 *   - leading '-': return non-options as option character 1
 *   - leading ':': report a missing argument as ':' rather than '?', silently
 *   - '--' ends option processing
 *   - getopt_long: --name, --name=value, unambiguous abbreviations, and the
 *     flag/val pair
 *
 * Deliberately NOT supported: the 'W;' extension (-W foo == --foo), which glibc
 * documents and essentially nothing uses; getopt_long_only, same reason.
 * Neither silently misbehaves -- 'W' is treated as an ordinary option letter,
 * and getopt_long_only is simply not declared. */
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

char *optarg;
int   optind = 1;
int   opterr = 1;
/* POSIX calls optopt's value "unspecified" until the first error, so this
 * default is a choice, not a spec requirement -- and the host-diff test
 * (tests/unit/libc_getopt_test.c) caught it: glibc's actual runtime default
 * is 0, not '?'. optopt is otherwise STICKY in both implementations (set
 * only on an error path, never reset on a successful call -- see the
 * absence of an `optopt = 0` anywhere near the `optarg = NULL` reset at the
 * top of getopt_engine), so a wrong initial value here is not just cosmetic:
 * it stays wrong for every successful call a program makes before its first
 * bad option, which for most CLI programs is most of the run. */
int   optopt;

/* Where we are inside a cluster like -abc; NULL between arguments. */
static char *nextchar;

/* The permutation window. Everything in [first_nonopt, last_nonopt) is a
 * non-option we have skipped over and will rotate to the end. */
static int first_nonopt;
static int last_nonopt;

enum { PERMUTE, REQUIRE_ORDER, RETURN_IN_ORDER };
static int ordering;
static int posixly_correct;
static int initialised;

/* Rotate the two adjacent blocks
 *     [first_nonopt, last_nonopt)   the skipped non-options
 *     [last_nonopt,  optind)        the options found since
 * so the options come first. Done by three reversals rather than glibc's
 * gcd-cycle shuffle: same result, and it is obviously correct at a glance,
 * which matters more here than the constant factor on an argv. */
static void reverse(char **v, int a, int b)      /* [a, b) */
{
    while (a < b - 1) {
        char *t = v[a]; v[a] = v[b - 1]; v[b - 1] = t;
        a++; b--;
    }
}

static void exchange(char **argv)
{
    reverse(argv, first_nonopt, last_nonopt);
    reverse(argv, last_nonopt, optind);
    reverse(argv, first_nonopt, optind);
    first_nonopt += optind - last_nonopt;
    last_nonopt = optind;
}

static const char *init_scan(const char *optstring)
{
    first_nonopt = last_nonopt = optind;
    nextchar = NULL;
    posixly_correct = getenv("POSIXLY_CORRECT") != NULL;

    if (optstring[0] == '-')      { ordering = RETURN_IN_ORDER; optstring++; }
    else if (optstring[0] == '+') { ordering = REQUIRE_ORDER;   optstring++; }
    else if (posixly_correct)     { ordering = REQUIRE_ORDER; }
    else                          { ordering = PERMUTE; }
    initialised = 1;
    return optstring;
}

static void err(int silent, const char *argv0, const char *msg, const char *what, int wlen)
{
    if (silent || !opterr) return;
    fprintf(stderr, "%s: %s", argv0, msg);
    if (wlen < 0) fprintf(stderr, "%s\n", what);
    else          fprintf(stderr, "%.*s\n", wlen, what);
}

/* The common engine. `longopts` may be NULL; when it is not, a "--name" (and,
 * for getopt_long, only that -- never a bare "-name") is matched against it. */
static int getopt_engine(int argc, char *const argv[], const char *optstring,
                         const struct option *longopts, int *longindex)
{
    char **av = (char **)argv;              /* permutation writes to argv, as glibc does */
    int silent;

    optarg = NULL;

    if (optind == 0) {                      /* glibc's documented reset */
        optind = 1;
        initialised = 0;
    }
    if (!initialised)
        optstring = init_scan(optstring);
    silent = (optstring[0] == ':');
    if (silent) optstring++;

    if (nextchar == NULL || *nextchar == '\0') {
        /* Advance to the next ARGV element. */
        if (last_nonopt > optind) last_nonopt = optind;
        if (first_nonopt > optind) first_nonopt = optind;

        if (ordering == PERMUTE) {
            if (first_nonopt != last_nonopt && last_nonopt != optind) exchange(av);
            else if (last_nonopt != optind) first_nonopt = optind;

            while (optind < argc && (av[optind][0] != '-' || av[optind][1] == '\0'))
                optind++;
            last_nonopt = optind;
        }

        /* "--" ends the options; skip it and rotate the rest to the end. */
        if (optind != argc && !strcmp(av[optind], "--")) {
            optind++;
            if (first_nonopt != last_nonopt && last_nonopt != optind) exchange(av);
            else if (first_nonopt == last_nonopt) first_nonopt = optind;
            last_nonopt = argc;
            optind = argc;
        }

        if (optind == argc) {
            if (first_nonopt != last_nonopt) optind = first_nonopt;
            return -1;
        }

        if (av[optind][0] != '-' || av[optind][1] == '\0') {
            if (ordering == REQUIRE_ORDER) return -1;
            optarg = av[optind++];
            return 1;                       /* RETURN_IN_ORDER */
        }

        nextchar = av[optind] + 1 + (longopts && av[optind][1] == '-');
    }

    /* --long */
    if (longopts && av[optind][1] == '-') {
        const struct option *p, *found = NULL;
        int exact = 0, ambig = 0, idx = -1, i;
        char *eq = strchr(nextchar, '=');
        size_t nlen = eq ? (size_t)(eq - nextchar) : strlen(nextchar);

        for (p = longopts, i = 0; p->name; p++, i++) {
            if (strncmp(p->name, nextchar, nlen)) continue;
            if (strlen(p->name) == nlen) { found = p; idx = i; exact = 1; break; }
            if (!found) { found = p; idx = i; }
            else if (found->has_arg != p->has_arg || found->flag != p->flag ||
                     found->val != p->val) ambig = 1;
        }
        if (ambig && !exact) {
            err(silent, av[0], "option '--' is ambiguous -- ", nextchar, (int)nlen);
            nextchar = NULL; optind++; optopt = 0;
            return '?';
        }
        if (found) {
            optind++;
            nextchar = NULL;
            if (eq) {
                if (found->has_arg != no_argument) optarg = eq + 1;
                else {
                    err(silent, av[0], "option '--' takes no argument -- ",
                        found->name, -1);
                    optopt = found->val;
                    return '?';
                }
            } else if (found->has_arg == required_argument) {
                if (optind < argc) optarg = av[optind++];
                else {
                    err(silent, av[0], "option requires an argument -- ", found->name, -1);
                    optopt = found->val;
                    return silent ? ':' : '?';
                }
            }
            if (longindex) *longindex = idx;
            if (found->flag) { *found->flag = found->val; return 0; }
            return found->val;
        }
        /* Not a long option, and it started with "--": it cannot be a short. */
        err(silent, av[0], "unrecognized option -- ", nextchar, -1);
        nextchar = NULL; optind++; optopt = 0;
        return '?';
    }

    /* short option */
    {
        char c = *nextchar++;
        const char *t = strchr(optstring, c);

        if (*nextchar == '\0') ++optind;

        if (!t || c == ':') {
            err(silent, av[0], "invalid option -- ", nextchar - 1, 1);
            optopt = c;
            return '?';
        }
        if (t[1] == ':') {
            if (t[2] == ':') {              /* optional argument, attached only */
                if (*nextchar) { optarg = nextchar; optind++; }
                else optarg = NULL;
                nextchar = NULL;
            } else {                        /* required argument */
                if (*nextchar) { optarg = nextchar; optind++; }
                else if (optind == argc) {
                    err(silent, av[0], "option requires an argument -- ", nextchar - 1, 1);
                    optopt = c;
                    nextchar = NULL;
                    return silent ? ':' : '?';
                } else optarg = av[optind++];
                nextchar = NULL;
            }
        }
        return c;
    }
}

int getopt(int argc, char *const argv[], const char *optstring)
{ return getopt_engine(argc, argv, optstring, NULL, NULL); }

int getopt_long(int argc, char *const argv[], const char *optstring,
                const struct option *longopts, int *longindex)
{ return getopt_engine(argc, argv, optstring, longopts, longindex); }
