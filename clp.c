/*
 * Copyright (c) 2015-2016,2021 Greg Becker.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $Id: clp.c 386 2016-01-27 13:25:47Z greg $
 */

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <inttypes.h>
#include <float.h>
#include <assert.h>
#include <ctype.h>
#include <getopt.h>
#include <math.h>
#include <sys/file.h>
#include <sys/param.h>

#include "clp.h"

#ifndef clp_suftab_default
#define clp_suftab_default  clp_suftab_combo
#endif

/* International System of Units suffixes...
 */
struct clp_suftab clp_suftab_si = {
    .list = "kMGTPEZY",
    .mult = { 1e3, 1e6, 1e9, 1e12, 1e15, 1e18, 1e21, 1e24 }
};

/* International Electrotechnical Commission suffixes...
 */
struct clp_suftab clp_suftab_iec = {
    .list = "KMGTPEZY",
    .mult = { 0x1p10, 0x1p20, 0x1p30, 0x1p40, 0x1p50, 0x1p60, 0x1p70, 0x1p80 }
};

struct clp_suftab clp_suftab_combo = {
    .list = "kmgtpezyKMGTPEZYbw",
    .mult = {
        0x1p10, 0x1p20, 0x1p30, 0x1p40, 0x1p50, 0x1p60, 0x1p70, 0x1p80,
        1e3, 1e6, 1e9, 1e12, 1e15, 1e18, 1e21, 1e24,
        512, sizeof(int)
    }
};

struct clp_suftab clp_suftab_none = {
    .list = ""
};

struct clp_suftab clp_suftab_time = {
    .list = "smhdwyc",
    .mult = { 1, 60, 3600, 86400, 86400 * 7, 86400 * 365, 86400 * 365 * 100ul }
};


struct clp_posparam clp_posparam_none[] = {
    { .name = NULL }
};


#ifdef CLP_DEBUG
static int clp_debug;

/* Called via the dprint() macro..
 */
static void __printflike(4, 5)
clp_dprint_impl(const char *file, int line, const char *func, const char *fmt, ...)
{
    va_list ap;

    if (file && func)
        fprintf(stdout, "  +%-4d %-6s %-12s  ", line, file, func);

    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
}

/* dprint() prints a debug message to stdout if (clp_debug >= lvl).
 */
#define clp_dprint(_level, ...)                                         \
do {                                                                    \
    if (clp_debug >= (_level)) {                                        \
        clp_dprint_impl(__FILE__, __LINE__, __func__, __VA_ARGS__);     \
    }                                                                   \
} while (0)

#else

#define clp_dprint(_lvl, ...) do { } while (0)
#endif /* CLP_DEBUG */

/* Render an error message for emission at the end of clp_parsev().
 *
 * If on entry clp->errbuf[0] is a punctuation character (e.g., ",")
 * then clp->errbuf will be saved and then appended to the string
 * produced by the given format.  If on entry clp->errbuf[0] is NUL
 * and errno is set then strerror() will be appended to the string
 * produced by the given format.
 */
void
clp_eprint(struct clp *clp, const char *fmt, ...)
{
    char suffix[sizeof(clp->errbuf) / 2];
    int xerrno = errno;
    va_list ap;
    int n;

    if (clp->errbuf[0] && !ispunct(clp->errbuf[0]))
        return;

    strncpy(suffix, clp->errbuf, sizeof(suffix) - 1);
    suffix[sizeof(suffix) - 1] = '\000';

    va_start(ap, fmt);
    n = vsnprintf(clp->errbuf, sizeof(clp->errbuf), fmt, ap);
    va_end(ap);

    if (n > 0 && (size_t)n < sizeof(clp->errbuf)) {
        snprintf(clp->errbuf + n, sizeof(clp->errbuf) - n, "%s%s",
                 (xerrno && !suffix[0]) ? ": " : "",
                 (xerrno && !suffix[0]) ? strerror(xerrno) : suffix);
    }

    errno = xerrno;
}

static bool
clp_optopt_valid(int c)
{
    return isgraph(c) && !strchr(":?-", c);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"

/* An option's conversion procedure is called each time the option is seen on
 * the command line.  The conversions for bool, string, open, fopen, and incr
 * require special handling, while those for simple integer types (int, long,
 * int64_t, ...) are handled by functions generated via CLP_CVT_TMPL().
 *
 * Note:  These functions are not type safe, but are typically invoked by clp
 * from a type-safe context.
 */
int
clp_cvt_bool(struct clp *clp, const char *optarg, int flags, void *parms, void *dst)
{
    bool *result = dst;

    *result = ! *result;

    return 0;
}

int
clp_cvt_string(struct clp *clp, const char *optarg, int flags, void *parms, void *dst)
{
    char **result = dst;

    if (!result) {
        errno = EINVAL;
        return EX_DATAERR;
    }

    *result = strdup(optarg);

    return *result ? 0 : EX_OSERR;
}

int
clp_cvt_open(struct clp *clp, const char *optarg, int flags, void *parms, void *dst)
{
    int *result = dst;

    if (!result) {
        errno = EINVAL;
        return EX_DATAERR;
    }

    *result = open(optarg, flags ? flags : O_RDONLY, 0644);

    return (*result >= 0) ? 0 : EX_NOINPUT;
}

int
clp_cvt_fopen(struct clp *clp, const char *optarg, int flags, void *parms, void *dst)
{
    const char *mode = parms ? parms : "r";
    FILE **result = dst;

    if (!result) {
        errno = EINVAL;
        return EX_DATAERR;
    }

    *result = fopen(optarg, mode);

    return *result ? 0 : EX_NOINPUT;
}

int
clp_cvt_incr(struct clp *clp, const char *optarg, int flags, void *parms, void *dst)
{
    int *result = dst;

    if (!result) {
        errno = EINVAL;
        return EX_DATAERR;
    }

    *result += 1;

    return 0;
}

/* The following macros will explode into a bunch of conversion functions.
 * With any luck the unused functions will be eliminated by the linker.
 */
CLP_CVT_TMPL(char,      char,       CHAR_MIN,   CHAR_MAX,    clp_suftab_default);
CLP_CVT_TMPL(u_char,    u_char,     0,          UCHAR_MAX,   clp_suftab_default);

CLP_CVT_TMPL(short,     short,      SHRT_MIN,   SHRT_MAX,    clp_suftab_default);
CLP_CVT_TMPL(u_short,   u_short,    0,          USHRT_MAX,   clp_suftab_default);

CLP_CVT_TMPL(int,       int,        INT_MIN,    INT_MAX,     clp_suftab_default);
CLP_CVT_TMPL(u_int,     u_int,      0,          UINT_MAX,    clp_suftab_default);

CLP_CVT_TMPL(long,      long,       LONG_MIN,   LONG_MAX,    clp_suftab_default);
CLP_CVT_TMPL(u_long,    u_long,     0,          ULONG_MAX,   clp_suftab_default);

CLP_CVT_TMPL(float,     float,      -FLT_MAX,   FLT_MAX,     clp_suftab_default);
CLP_CVT_TMPL(double,    double,     -DBL_MAX,   DBL_MAX,     clp_suftab_default);

CLP_CVT_TMPL(int8_t,    int8_t,     INT8_MIN,   INT8_MAX,    clp_suftab_default);
CLP_CVT_TMPL(uint8_t,   uint8_t,    0,          UINT8_MAX,   clp_suftab_default);

CLP_CVT_TMPL(int16_t,   int16_t,    INT16_MIN,  INT16_MAX,   clp_suftab_default);
CLP_CVT_TMPL(uint16_t,  uint16_t,   0,          UINT16_MAX,  clp_suftab_default);

CLP_CVT_TMPL(int32_t,   int32_t,    INT32_MIN,  INT32_MAX,   clp_suftab_default);
CLP_CVT_TMPL(uint32_t,  uint32_t,   0,          UINT32_MAX,  clp_suftab_default);

CLP_CVT_TMPL(int64_t,   int64_t,    INT64_MIN,  INT64_MAX,   clp_suftab_default);
CLP_CVT_TMPL(uint64_t,  uint64_t,   0,          UINT64_MAX,  clp_suftab_default);

CLP_CVT_TMPL(intmax_t,  intmax_t,   INTMAX_MIN, INTMAX_MAX,  clp_suftab_default);
CLP_CVT_TMPL(uintmax_t, uintmax_t,  0,          UINTMAX_MAX, clp_suftab_default);

CLP_CVT_TMPL(intptr_t,  intptr_t,   INTPTR_MIN, INTPTR_MAX,  clp_suftab_default);
CLP_CVT_TMPL(uintptr_t, uintptr_t,  0,          UINTPTR_MAX, clp_suftab_default);

CLP_CVT_TMPL(size_t,    size_t,     0,          SIZE_MAX,    clp_suftab_default);
CLP_CVT_TMPL(time_t,    time_t,     0,          LONG_MAX,    clp_suftab_time);

CLP_GET_TMPL(bool,      bool);
CLP_GET_TMPL(incr,      int);
CLP_GET_TMPL(open,      int);
CLP_GET_TMPL(fopen,     FILE *);
CLP_GET_TMPL(string,    char *);


int
clp_cvt_subcmd(struct clp *clp, const char *str, int flags, void *parms, void *dst)
{
    struct clp_subcmd *subcmd = parms;
    struct clp_subcmd *match = NULL;

    if (subcmd) {
        for (; subcmd->name; ++subcmd) {
            char *pc = strstr(subcmd->name, str);

            if (!pc || pc != subcmd->name)
                continue;

            if (match) {
                clp_eprint(clp, "ambiguous subcommand '%s', use -h for help", str);
                errno = EINVAL;
                return EX_USAGE;
            }

            match = subcmd; // found a full or partial match
        }
    }

    if (!match) {
        clp_eprint(clp, "invalid subcommand '%s', use -h for help", str);
        errno = EINVAL;
        return EX_USAGE;
    }

    *(void **)dst = match;

    return 0;
}

int
clp_action_subcmd(struct clp_posparam *param)
{
    struct clp_subcmd *subcmd = *(void **)param->cvtdst;
    int rc;

    rc = clp_parsev(param->argc, param->argv, subcmd->optionv, subcmd->posparamv);

    return rc ?: -1;
}

#pragma GCC diagnostic pop

struct clp_option *
clp_find(int c, struct clp_option *optionv)
{
    if (optionv) {
        struct clp_option *o;

        for (o = optionv; o->optopt > 0; ++o) {
            if (o->optopt == c)
                return o;
        }
    }

    return NULL;
}


struct clp_option *
clp_given(int c, struct clp_option *optionv, void *dst)
{
    struct clp_option *o = clp_find(c, optionv);

    if (o && o->given) {
        if (dst && o->getfunc)
            o->getfunc(o, dst);

        return o;
    }

    return NULL;
}

/* Return true if the two specified options are mutually exclusive.
 */
static int
clp_excludes2(const struct clp_option *l, const struct clp_option *r)
{
    if (l && r && l != r) {
        if (l->excludes) {
            if (l->excludes[0] == '^') {
                if (!strchr(l->excludes, r->optopt)) {
                    return true;
                }
            } else {
                if (l->excludes[0] == '*' || strchr(l->excludes, r->optopt)) {
                    return true;
                }
            }
        }
        if (r->excludes) {
            if (r->excludes[0] == '^') {
                if (!strchr(r->excludes, l->optopt)) {
                    return true;
                }
            } else {
                if (r->excludes[0] == '*' || strchr(r->excludes, l->optopt)) {
                    return true;
                }
            }
        }
        if (l->paramv && r->paramv && l->paramv != r->paramv) {
            return true;
        }
    }

    return false;
}

/* Return the opt letter of any option that is mutually exclusive
 * with the specified option and which appeared on the command line
 * at least the specified number of times.
 */
struct clp_option *
clp_excludes(struct clp_option *first, const struct clp_option *option, int given)
{
    struct clp_option *o;

    for (o = first; o->optopt > 0; ++o) {
        if (o->given >= given && clp_excludes2(option, o)) {
            return o;
        }
    }

    return NULL;
}

/* Trim leading white space from given string.  Returns NULL
 * if resulting string is zero length.  Typically, str is
 * either NULL or a zero length string on entry.
 */
static const char *
clp_trim(const char *str)
{
    while (str && isspace(str[0]))
        ++str;

    return (str && !str[0] ? NULL : str);
}

/* Return the count of leading open brackets, and the given name copied
 * to buf[] and stripped of brackets and leading/trailing white space.
 */
int
clp_unbracket(const char *name, char *buf, size_t bufsz)
{
    char *bufbase = buf;
    int obrackets = 0;

    if (!name || !buf || bufsz < 1) {
        abort();
    }

    // Count open brackets up to first non-space character.
    while (isspace(*name) || *name == '[') {
        obrackets += (*name == '[');
        ++name;
    }

    strncpy(buf, name, bufsz - 1);
    buf[bufsz - 1] = '\000';

    // Terminate buf at first bracket
    while (*buf && *buf != ']' && *buf != '[')
        ++buf;
    *buf = '\000';

    // Eliminate trailing white space
    while (buf-- > bufbase && isspace(*buf))
        *buf = '\000';

    return obrackets;
}

static struct clp_subcmd *
clp_subcmd(const struct clp_posparam *param)
{
    struct clp_subcmd *subcmd = NULL;

    if (param && param->cvtsubcmd)
        subcmd = param->cvtparms;

    return (subcmd && subcmd->name) ? subcmd : NULL;
}

/* Lexical string comparator for qsort (e.g., AaBbCcDd...)
 */
static int
clp_string_cmp(const void *lhs, const void *rhs)
{
    const char *l = (const char *)lhs;
    const char *r = (const char *)rhs;
    int lc = tolower(*l);
    int rc = tolower(*r);

    if (lc == rc) {
        return (isupper(*l) ? -1 : 1);
    }

    return (lc - rc);
}

/* Print just the usage line, i.e., lines of the general form
 *   "usage: progname [options] args..."
 */
static void  __attribute__((__nonnull__(1)))
clp_usage(struct clp *clp, const struct clp_option *limit)
{
    struct clp_posparam *paramv = clp->paramv;
    char *pc_optarg, *pc_opt, *pc;
    struct clp_posparam *param;
    struct clp_option *o;
    char *pc_excludes;
    FILE *fp = stdout;

    if (clp->optionc > CLP_OPTION_MAX) {
        abort();
    }

    if (limit) {
        bool limit_excl_all = limit->excludes && strchr("*^", limit->excludes[0]);

        if (!limit_excl_all && !limit->paramv) {
            return;
        }

        if (limit->paramv) {
            paramv = limit->paramv;
        }
    }

    char excludes_buf[clp->optionc + 1];
    char optarg_buf[clp->optionc + 1];
    char opt_buf[clp->optionc + 1];

    pc_excludes = excludes_buf;
    pc_optarg = optarg_buf;
    pc_opt = opt_buf;

    /* Build three lists of option characters:
     *
     * 1) excludes_buf[] contains all the options that might exclude
     * or be excluded by another option that share the same parameter vector.
     *
     * 2) optarg_buf[] contains all the options not in (1) that require
     * an argument.
     *
     * 3) opt_buf[] contains all the rest not covered by (1) or (2).
     *
     * Note: if 'limit' is not NULL, then only options that share
     * the same paramv or have a NULL paramv may appear in one of
     * the three lists.
     */
    for (o = clp->optionv; o->optopt > 0; ++o) {
        if (!clp_optopt_valid(o->optopt))
            continue;

        o->help = clp_trim(o->help);

        if (limit) {
            if (clp_excludes2(limit, o)) {
                continue;
            }
        } else if (o->paramv) {
            continue;
        }

        if (o != limit && o->help) {
            if (o->excludes) {
                *pc_excludes++ = o->optopt;
            } else {
                struct clp_option *x = clp_excludes(clp->optionv, o, 0);

                if (x && x->paramv == o->paramv) {
                    *pc_excludes++ = o->optopt;
                } else if (o->argname) {
                    *pc_optarg++ = o->optopt;
                } else {
                    *pc_opt++ = o->optopt;
                }
            }
        }
    }

    *pc_excludes = '\000';
    *pc_optarg = '\000';
    *pc_opt = '\000';

    qsort(opt_buf, strlen(opt_buf), 1, clp_string_cmp);
    qsort(optarg_buf, strlen(optarg_buf), 1, clp_string_cmp);
    qsort(excludes_buf, strlen(excludes_buf), 1, clp_string_cmp);

    if (limit)
        clp_dprint(1, "  limit -%c:\n", limit->optopt);
    clp_dprint(1, "  has excludes: %s\n", excludes_buf);
    clp_dprint(1, "  has optarg:   %s\n", optarg_buf);
    clp_dprint(1, "  bool opts:    %s\n", opt_buf);

    /* Now print out the usage line in the form of:
     *
     * usage: basename [mandatory-opt] [bool-opts] [opts-with-args] [excl-opts] [posparams...]
     */

    /* [mandatory-opt]
     */
    fprintf(fp, "usage: %s", clp->basename);
    if (limit) {
        fprintf(fp, " -%c", limit->optopt);
    }

    /* [bool-opts]
     */
    if (opt_buf[0]) {
        fprintf(fp, " [-%s]", opt_buf);
    }

    /* [opts-with-args]
     */
    for (pc = optarg_buf; *pc; ++pc) {
        o = clp_find(*pc, clp->optionv);
        if (o) {
            fprintf(fp, " [-%c %s]", o->optopt, o->argname);
        }
    }

    /* Generate the mutually exclusive option usage message...
     * [excl-args]
     */
    if (excludes_buf[0]) {
        char *listv[clp->optionc + 1];
        int listc = 0;
        char *cur;
        int i;

        /* Build a vector of strings where each string contains
         * mutually exclusive options.
         */
        for (cur = excludes_buf; *cur; ++cur) {
            struct clp_option *l = clp_find(*cur, clp->optionv);
            char buf[1024], *pc_buf;
            char *tmp;

            pc_buf = buf;

            for (pc = excludes_buf; *pc; ++pc) {
                if (cur == pc) {
                    *pc_buf++ = *pc;
                } else {
                    struct clp_option *r = clp_find(*pc, clp->optionv);

                    if (clp_excludes2(l, r)) {
                        *pc_buf++ = *pc;
                    }
                }
            }

            *pc_buf = '\000';

            tmp = strdup(buf);
            if (!tmp)
                abort();

            listv[listc++] = tmp;
        }

        /* Eliminate duplicate strings.
         */
        for (i = 0; i < listc; ++i) {
            int j;

            for (j = i + 1; j < listc; ++j) {
                if (listv[i] && listv[j]) {
                    if (0 == strcmp(listv[i], listv[j])) {
                        free(listv[j]);
                        listv[j] = NULL;
                    }
                }
            }
        }

        /* Ensure that all options within a list are mutually exclusive.
         */
        for (i = 0; i < listc; ++i) {
            if (listv[i]) {
                for (pc = listv[i]; *pc; ++pc) {
                    struct clp_option *l = clp_find(*pc, clp->optionv);
                    char *pc2;

                    for (pc2 = listv[i]; *pc2; ++pc2) {
                        if (pc2 != pc) {
                            struct clp_option *r = clp_find(*pc2, clp->optionv);

                            if (!clp_excludes2(l, r)) {
                                free(listv[i]);
                                listv[i] = NULL;
                                goto next;
                            }
                        }
                    }
                }
            }

          next:
            continue;
        }

        /* Now, print out the remaining strings of mutually exclusive options.
         */
        for (i = 0; i < listc; ++i) {
            if (listv[i]) {
                const char *bar = " [";

                for (pc = listv[i]; *pc; ++pc) {
                    o = clp_find(*pc, clp->optionv);
                    if (o->argname) {
                        fprintf(fp, "%s-%c %s", bar, *pc, o->argname);
                    } else {
                        fprintf(fp, "%s-%c", bar, *pc);
                    }

                    bar = " | ";
                }

                fprintf(fp, "]");

                free(listv[i]);
            }
        }
    }

    /* Finally, print out all the positional parameters.
     * [posparams...]
     */
    if (paramv) {
        int noptional = 0;

        for (param = paramv; param->name; ++param) {
            char namebuf[128];
            int isopt;

            isopt = clp_unbracket(param->name, namebuf, sizeof(namebuf));

            if (isopt) {
                ++noptional;
            }

            fprintf(fp, "%s%s", isopt ? " [" : " ", namebuf);

            if (param[1].name) {
                isopt = clp_unbracket(param[1].name, namebuf, sizeof(namebuf));
            }

            /* If we're at the end of the list or the next parameter
             * is not optional then print all the closing brackets.
             */
            if (!param[1].name || !isopt) {
                for (; noptional > 0; --noptional) {
                    fputc(']', fp);
                }
            }
        }
    }

    fprintf(fp, "%s\n", paramv ? "" : " [args...]");
}

int
clp_version(struct clp_option *option)
{
    printf("%s\n", (char *)option->cvtdst);

    return 0;
}

/* Lexical option comparator for qsort (e.g., AaBbCcDd...)
 */
static int
clp_help_cmp(const void *lhs, const void *rhs)
{
    struct clp_option const *l = *(struct clp_option * const *)lhs;
    struct clp_option const *r = *(struct clp_option * const *)rhs;

    int lc = tolower(l->optopt);
    int rc = tolower(r->optopt);

    if (lc == rc) {
        return (isupper(l->optopt) ? -1 : 1);
    }

    return (lc - rc);
}

/* Print the entire help message, for example:
 *
 * usage: prog [-v] [-i intarg] src... dst
 * usage: prog -h
 * usage: prog -V
 * -h         print this help list
 * -i intarg  specify an integer argument
 * -V         print version
 * -v         increase verbosity
 * src...  specify one or more source files
 * dst     specify destination directory
 */
int
clp_help(struct clp_option *opthelp)
{
    struct clp_posparam *paramv, *param;
    struct clp_option *option;
    int subcmd_width, width;
    size_t optionc;
    bool longhelp;
    struct clp *clp;
    FILE *fp;

    /* opthelp is the option that triggered clp into calling clp_help().
     * Ususally -h, but the user could have changed it...
     */
    if (!opthelp) {
        return 0;
    }

    clp = opthelp->clp;

    /* Create an array of pointers to options and sort it.
     */
    struct clp_option *optionv[clp->optionc];

    for (size_t i = optionc = 0; i < clp->optionc; ++i) {
        struct clp_option *o = clp->optionv + i;

        if (clp_optopt_valid(o->optopt))
            optionv[optionc++] = o;
    }

    qsort(optionv, optionc, sizeof(optionv[0]), clp_help_cmp);

    /* Print the default usage line.
     */
    fp = opthelp->priv ? opthelp->priv : stdout;
    clp_usage(clp, NULL);

    /* Print usage lines for each option that has positional parameters
     * different than the default usage.
     * Also, determine the width of the longest combination of option
     * argument and long option names.
     */
    longhelp = (opthelp->longidx >= 0);
    width = 0;

    for (size_t i = 0; i < optionc; ++i) {
        int len = 0;

        option = optionv[i];

        if (!option->help) {
            continue;
        }

        clp_usage(clp, option);

        if (option->argname) {
            len += strlen(option->argname) + 1;
        }
        if (longhelp && option->longopt) {
            len += strlen(option->longopt) + 4;
        }
        if (len > width) {
            width = len;
        }
    }

    /* Print a line of help for each option.
     */
    for (size_t i = 0; i < optionc; ++i) {
        char buf[width + 8];

        option = optionv[i];

        if (!option->help)
            continue;

        buf[0] = '\000';

        if (longhelp && option->longopt) {
            strcat(buf, ", --");
            strcat(buf, option->longopt);
        }
        if (option->argname) {
            strcat(buf, " ");
            strcat(buf, option->argname);
        }

        fprintf(fp, "-%c%-*s  %s\n", option->optopt, width, buf, option->help);
    }

    /* Determine the width of the longest positional parameter name.
     */
    subcmd_width = 0;
    width = 0;

    for (paramv = clp->params; paramv; paramv = paramv->next) {
        for (param = paramv; param->name; ++param) {
            struct clp_subcmd *subcmd = clp_subcmd(param);
            char namebuf[width + 128];
            int namelen;

            for (; subcmd && subcmd->name; ++subcmd) {
                subcmd->help = clp_trim(subcmd->help);
                if (subcmd->help) {
                    namelen = strlen(namebuf);
                    if (namelen > subcmd_width) {
                        subcmd_width = namelen;
                    }
                }
            }

            param->help = clp_trim(param->help);
            if (!param->help)
                continue;

            clp_unbracket(param->name, namebuf, sizeof(namebuf));
            namelen = strlen(namebuf);

            if (namelen > width) {
                width = namelen;
            }
        }
    }

    if (!clp->paramv)
        fprintf(fp, "%-*s  %s\n", width, "args...", "zero or more positional arguments");

    /* Print a line of help for each positional paramter.
     */
    for (paramv = clp->params; paramv; paramv = paramv->next) {
        for (param = paramv; param->name; ++param) {
            struct clp_subcmd *subcmd;
            char namebuf[width + 1];
            char *comma = "";

            clp_unbracket(param->name, namebuf, sizeof(namebuf));

            param->help = clp_trim(param->help);

            subcmd = clp_subcmd(param);
            if (!subcmd) {
                if (param->help)
                    fprintf(fp, "%-*s  %s\n", width, namebuf, param->help);
                continue;
            }

            /* Print subcommand help.  If caller didn't provide a help
             * string for the subcommand posparam then we build one
             * from the list of subcommands.
             */
            if (param->help) {
                fprintf(fp, "%s  %s\n", namebuf, param->help);
            } else {
                fprintf(fp, "%s  one of {", namebuf);

                for (subcmd = param->cvtparms; subcmd->name; ++subcmd) {
                    subcmd->help = clp_trim(subcmd->help);
                    if (subcmd->help) {
                        fprintf(fp, "%s%s", comma, subcmd->name);
                        comma = ", ";
                    }
                }
                fprintf(fp, "}\n");
            }

            for (subcmd = param->cvtparms; subcmd->name; ++subcmd) {
                if (subcmd->help)
                    fprintf(fp, "  %-*s  %s\n", subcmd_width, subcmd->name, subcmd->help);
            }
        }
    }

    return 0;
}

/* Determine the minimum and maximum number of arguments that the
 * given posparam vector could consume.
 */
static void
clp_posparam_minmax(struct clp_posparam *paramv, int *posminp, int *posmaxp)
{
    struct clp_posparam *param;

    if (!paramv || !posminp || !posmaxp) {
        abort();
    }

    *posminp = 0;
    *posmaxp = 0;

    for (param = paramv; param->name; ++param) {
        struct clp_subcmd *subcmd = clp_subcmd(param);
        char namebuf[128];
        int isoptional;
        int len;

        isoptional = clp_unbracket(param->name, namebuf, sizeof(namebuf));

        param->posmin = isoptional ? 0 : 1;

        len = strlen(namebuf);
        if (len >= 3 && 0 == strncmp(namebuf + len - 3, "...", 3)) {
            param->posmax = CLP_POSPARAM_MAX;
        } else {
            param->posmax = subcmd ? CLP_POSPARAM_MAX : 1;
        }

        *posminp += param->posmin;
        *posmaxp += param->posmax;

        if (subcmd)
            break;
    }
}

static int
clp_parsev_impl(struct clp *clp, int argc, char **argv)
{
    struct clp_option *options_head, **options_tail;
    struct clp_posparam **params_tail;
    struct clp_posparam *paramv;
    struct clp_option *o;
    int posmin, posmax;
    char *pc;
    int rc;

    params_tail = &clp->params;
    if (*params_tail) {
        params_tail = &(*params_tail)->next;
    }

    options_tail = &options_head;
    *options_tail = NULL;

    pc = clp->optstring;
    *pc++ = '+';    // Enable POSIXLY_CORRECT semantics
    *pc++ = ':';    // Disable getopt error reporting
    *pc = '\000';

    /* Generate the optstring and the long options table
     * from the given vector of options.
     */
    if (clp->optionv) {
        struct option *longopt = clp->longopts;

        for (o = clp->optionv; o->optopt > 0; ++o) {
            if (!clp_optopt_valid(o->optopt)) {
                clp_dprint(1, "invalid option %d (index %ld) ignored\n",
                           o->optopt, o - clp->optionv);
                continue;
            }

            if (strchr(clp->optstring + 2, o->optopt)) {
                clp_dprint(1, "duplicate option %d (index %ld) ignored\n",
                           o->optopt, o - clp->optionv);
                continue;
            }

            *pc++ = o->optopt;
            if (o->argname)
                *pc++ = ':';
            *pc = '\000';

            if (o->longopt) {
                longopt->name = o->longopt;
                longopt->val = o->optopt;
                longopt->has_arg = o->argname ? required_argument : no_argument;

                ++longopt;
            }

            if (o->paramv) {
                *params_tail = o->paramv;
                params_tail = &o->paramv->next;
                *params_tail = NULL;
            }
        }

        if (longopt > clp->longopts) {
            memset(longopt, 0, sizeof(*longopt));
            *pc++ = 'W';
            *pc++ = ';';
            *pc = '\000';
        }
    }

    char usehelp[] = ", use -h for help";
    if (clp->opthelp > 0) {
        usehelp[7] = clp->opthelp;
    } else {
        usehelp[0] = '\000';
    }

    options_tail = &options_head;
    *options_tail = NULL;

    paramv = clp->paramv;

    /* Reset getopt_long()...
     */
#ifdef _OPTRESET_DECLARED
    optreset = 1;
#endif

    optind = 1;

    while (1) {
        struct clp_option *x;
        int curind = optind;
        int longidx = -1;
        int c;

        errno = 0;

        c = getopt_long(argc, argv, clp->optstring, clp->longopts, &longidx);

        if (-1 == c) {
            break;
        } else if ('?' == c) {
            clp_eprint(clp, "invalid option %s%s", argv[curind], usehelp);
            return EX_USAGE;
        } else if (':' == c) {
            clp_eprint(clp, "option %s requires a parameter%s", argv[curind], usehelp);
            return EX_USAGE;
        }

        /* Look up the option.  This should not fail unless someone perturbs
         * the option vector that was passed in to us.
         */
        o = clp_find(c, clp->optionv);
        if (!o) {
            clp_eprint(clp, "+%d %s: program error: unexpected option %s",
                       __LINE__, __FILE__, argv[curind]);
            return EX_SOFTWARE;
        }

        /* See if this option is excluded by any other option given so far...
         */
        x = clp_excludes(clp->optionv, o, 1);
        if (x) {
            clp_eprint(clp, "option -%c excludes -%c%s", x->optopt, c, usehelp);
            return EX_USAGE;
        }

        /* Build a list of after procs to run after option processing
         */
        if (o->after && !o->given) {
            o->next = NULL;
            *options_tail = o;
            options_tail = &o->next;
        }

        o->longidx = longidx;
        o->optarg = optarg;
        ++o->given;

        if (o->paramv)
            paramv = o->paramv;

        if (o->cvtfunc) {
            if (o->given > 1 && o->cvtdst) {
                if (o->cvtfunc == clp_cvt_string) {
                    free(*(void **)o->cvtdst);
                    *(void **)o->cvtdst = NULL;
                }
            }

            rc = o->cvtfunc(clp, optarg, o->cvtflags, o->cvtparms, o->cvtdst);
            if (rc) {
                if (rc > 0) {
                    char optstr[] = { o->optopt, '\000' };

                    clp_eprint(clp, "unable to convert '%s%s %s'",
                               (longidx >= 0) ? "--" : "-",
                               (longidx >= 0) ? o->longopt : optstr,
                               optarg);
                }

                return (rc > 0) ? rc : 0;
            }
        }

        if (o->action) {
            rc = o->action(o);
            if (rc)
                return (rc > 0) ? rc : 0;
        }
    }

    posmin = posmax = 0;
    argc -= optind;
    argv += optind;

    /* Only check positional parameter counts if paramv is not NULL.
     * This allows the caller to prevent parameter processing by clp
     * and handle it themselves.
     */
    if (paramv) {
        clp_posparam_minmax(paramv, &posmin, &posmax);

        if (argc < posmin) {
            const char *more = argc ? " more" : "";

            clp_eprint(clp, "%d%s positional argument%s required%s",
                       posmin - argc,
                       (posmin - argc) > 0 ? more : "",
                       (posmin - argc) > 1 ? "s" : "",
                       usehelp);
            return EX_USAGE;
        }
        else if (argc > posmax) {
            clp_eprint(clp, "%d extraneous positional argument%s detected%s",
                       argc - posmax,
                       argc - posmax > 1 ? "s" : "",
                       usehelp);
            return EX_USAGE;
        }
    }

    /* Call each given option's after() procedure now that all options have
     * been processed and the command line syntax has been verified.
     */
    while (options_head) {
        rc = options_head->after(options_head);
        if (rc)
            return (rc > 0) ? rc : 0;

        options_head = options_head->next;
    }

    if (paramv) {
        struct clp_posparam *param;
        int i;

        /* Distribute the remaining arguments to the positional parameters
         * using a greedy approach.
         */
        for (param = paramv; param->name && argc > 0; ++param) {
            param->argv = argv;
            param->argc = 0;

            if (param->posmin == 1) {
                param->argc = 1;
                if (param->posmax > 1) {
                    param->argc += argc - posmin;
                }
                --posmin;
            }
            else if (argc > posmin) {
                if (param->posmax > 1) {
                    param->argc = argc - posmin;
                } else {
                    param->argc = 1;
                }
            }

            clp_dprint(1, "argc=%d posmin=%d argv=%s param=%s %d,%d,%d\n",
                       argc, posmin, *argv, param->name, param->posmin,
                       param->posmax, param->argc);

            argv += param->argc;
            argc -= param->argc;

            if (clp_subcmd(param))
                break;
        }

        if (argc > 0) {
            clp_dprint(1, "args left over: argc=%d posmin=%d argv=%s\n",
                       argc, posmin, *argv);
        }

        /* Call each parameter's convert() procedure for each given argument.
         */
        for (param = paramv; param->name; ++param) {
            for (i = 0; i < param->argc; ++i) {
                if (param->cvtfunc) {
                    rc = param->cvtfunc(clp, param->argv[i], param->cvtflags,
                                        param->cvtparms, param->cvtdst);
                    if (rc) {
                        if (rc > 0)
                            clp_eprint(clp, "unable to convert '%s'", param->argv[i]);
                        return (rc > 0) ? rc : 0;
                    }
                }

                if (param->action) {
                    rc = param->action(param);
                    if (rc)
                        return (rc > 0) ? rc : 0;
                }
            }
        }

        /* Call each filled parameter's after() procedure.
         */
        for (param = paramv; param->name; ++param) {
            if (param->after && param->argc > 0) {
                rc = param->after(param);
                if (rc)
                    return (rc > 0) ? rc : 0;
            }
        }
    }

    return 0;
}

/* Like clp_parsev(), but takes a string instead of a vector.
 * Uses strsep() to break the line up by the given delimiters.
 */
int
clp_parsel(const char *line, const char *delim,
           struct clp_option *optionv,
           struct clp_posparam *paramv)
{
    char **argv;
    int argc;
    int rc;

    rc = clp_breakargs(line, delim, &argc, &argv);
    if (rc)
        return rc;

    rc = clp_parsev(argc, argv, optionv, paramv);

    free(argv);

    return rc;
}

/* Parse a vector of strings as specified by the given option
 * and parameter vectors (either or both of which may be nil).
 *
 * On error, returns a suggested exit code from sysexits.h.
 */
int
clp_parsev(int argc, char **argv,
           struct clp_option *optionv,
           struct clp_posparam *paramv)
{
    struct clp clp;
    size_t sz;
    int rc;

#ifdef CLP_DEBUG
    char *env = getenv("CLP_DEBUG");

    if (env) {
        clp_debug = strtol(env, NULL, 0);
    }
#endif

    if (argc < 1 || !argv)
        return 0;

    memset(&clp, 0, sizeof(clp));

    clp.basename = __func__;
    if (argc > 0) {
        clp.basename = strrchr(argv[0], '/');
        clp.basename = (clp.basename ? clp.basename + 1 : argv[0]);
    }

    /* Validate options and initialize/reset from previous run.
     */
    if (optionv) {
        struct clp_option *o;

        clp.optionv = optionv;

        for (o = optionv; o->optopt > 0; ++o) {
            o->clp = &clp;
            o->given = 0;
            o->optarg = NULL;
            o->next = NULL;

            /* Trim leading whitespace and set to NULL if empty.
             */
            o->help = clp_trim(o->help);
            o->argname = clp_trim(o->argname);
            o->excludes = clp_trim(o->excludes);

            if (o->cvtfunc && !o->cvtdst) {
                o->cvtdst = memset(o->cvtdstbuf, 0, sizeof(o->cvtdstbuf));
            }

            if (o->cvtfunc == clp_cvt_bool || o->cvtfunc == clp_cvt_incr) {
                o->argname = NULL;
            } else if (!o->longopt && o->argname && strlen(o->argname) > 1) {
                o->longopt = o->argname;
            }

            if (o->after == clp_help) {
                clp.opthelp = o->optopt;
            }

            ++clp.optionc;
        }

        if (clp.optionc > CLP_OPTION_MAX) {
            fprintf(stderr, "%s: invalid optionc %zu\n", clp.basename, clp.optionc);
            abort();
        }
    }

    /* Validate positional parameters and initialize/reset from previous run.
     */
    if (paramv) {
        struct clp_posparam *param;

        clp.paramv = paramv;
        clp.params = paramv;

        for (param = paramv; param->name; ++param) {
            param->clp = &clp;
            param->posmin = 0;
            param->posmax = 0;
            param->argc = 0;
            param->argv = NULL;
            param->next = NULL;

            if (param->cvtfunc && !param->cvtdst) {
                param->cvtdst = memset(param->cvtdstbuf, 0, sizeof(param->cvtdstbuf));
            }
        }
    }

    /* Allocate a single chunk of memory to hold all the long options
     * and the getopt option string.
     */
    sz = (clp.optionc + 1) * sizeof(*clp.longopts);
    sz += clp.optionc * 2 + 8;

    clp.longopts = calloc(sz, 1);
    if (!clp.longopts) {
        clp_eprint(&clp, "+%d %s: malloc(%zu) longopts failed",
                   __LINE__, __FILE__, sz);
        return EX_OSERR;
    }

    clp.optstring = (char *)(clp.longopts + clp.optionc + 1);
    clp.optstring[0] = '\000';

    rc = clp_parsev_impl(&clp, argc, argv);

    if (rc && clp.errbuf[0])
        fprintf(stderr, "%s: %s\n", clp.basename, clp.errbuf);

    free(clp.longopts);
    clp.optstring = NULL;
    clp.longopts = NULL;

    return rc;
}

/* Create a vector of strings from words in src.
 *
 * Words are delimited by any character from delim (or isspace() if delim
 * is nil) and delimiters are elided.  Delimiters that are escaped by a
 * backslash and/or occur within quoted strings lose their significance
 * as delimiters and hence are retained with the word in which they appear.
 *
 * If delim is nil, then all whitespace between words is elided, which is
 * to say that zero-length strings between delimiters are always elided.
 * If delim is not nil, then zero-length strings between non-whitespace
 * delimiters are always preserved.
 *
 * For example:
 *
 *    src = :one\, two,, , "four,five" :
 *    delim = ,:
 *
 *    argc = 6;
 *    argv[0] = ""
 *    argv[1] = "one, two"
 *    argv[2] = ""
 *    argv[3] = " "
 *    argv[4] = " four,five "
 *    argv[5] = ""
 *    argv[6] = NULL
 *
 * On success, argc and argv are returned via *argcp and *argvp respectively
 * (if not nil), and argv[argc] is always set to NULL.  If argvp is not nil
 * then *argvp must always be freed by the caller, even if *argcp is zero.
 *
 * On failure, errno is set and an exit code from sysexits.h is returned.
 */
int
clp_breakargs(const char *src, const char *delim, int *argcp, char ***argvp)
{
    bool backslash, dquote, squote;
    char **argv, *prev, *dst;
    int argcmax, argc;
    const char *pc;
    size_t argvsz;

    if (argcp)
        *argcp = 0;
    if (argvp)
        *argvp = NULL;

    if (!src) {
        errno = EINVAL;
        return EX_DATAERR;
    }

    /* Allocate enough space to hold a pointer for every non-alpha
     * character in src[] plus a copy of the entire source string.
     * This will generally waste a bit of space, but it greatly
     * simplifies cleanup.
     */
    argcmax = 4;
    for (pc = src; *pc; ++pc)
        argcmax += !isalpha(*pc);
    argvsz = sizeof(*argv) * argcmax + (pc - src + 1);

    argv = malloc(argvsz);
    if (!argv) {
        errno = ENOMEM;
        return EX_OSERR;
    }

    backslash = dquote = squote = false;
    dst = (char *)(argv + argcmax);
    prev = dst;
    argc = 0;

    while (1) {
        if (backslash) {
            backslash = false;

            /* TODO: Should we convert printf escapes or leave
             * unconverted in dst?
             */
            switch (*src) {
            case 'a': *dst++ = '\a'; break;
            case 'b': *dst++ = '\b'; break;
            case 'f': *dst++ = '\f'; break;
            case 'n': *dst++ = '\n'; break;
            case 'r': *dst++ = '\r'; break;
            case 't': *dst++ = '\t'; break;
            case 'v': *dst++ = '\v'; break;

            default:
                if (isdigit(*src)) {
                    char *end;

                    *dst++ = strtoul(src, &end, 8); // TODO: Test me...
                    src = end;
                    continue;
                }

                *dst++ = *src;
                break;
            }
        }
        else if (*src == '\\') {
            backslash = true;
        }
        else if (*src == '"') {
            if (squote) {
                *dst++ = *src;
            } else {
                dquote = !dquote;
            }
        }
        else if (*src == '\'') {
            if (dquote) {
                *dst++ = *src;
            } else {
                squote = !squote;
            }
        }
        else if (dquote || squote) {
            *dst++ = *src;
        }
        else if (!delim && (!*src || isspace(*src))) {
            if (dst > prev) {
                argv[argc++] = prev;
                *dst++ = '\000';
                prev = dst;
            }
            // else elides leading whitespace and NUL characters...
        } else if (delim && (pc = strchr(delim, *src))) {
            if (dst > prev || !isspace(*pc)) {
                argv[argc++] = prev;
                *dst++ = '\000';
                prev = dst;
            }
        } else {
            *dst++ = *src;
        }

        if (!*src++)
            break;
    }

    if (dquote || squote) {
        free(argv);
        errno = EBADMSG;
        return EX_DATAERR;
    }

    if (dst > prev) {
        argv[argc++] = prev;
        *dst++ = '\000';
    }

    assert(argc < argcmax);
    argv[argc] = NULL;

    if (argcp) {
        *argcp = argc;
    }
    if (argvp) {
        *argvp = argv;
    } else {
        free(argv);
    }

    return 0;
}
