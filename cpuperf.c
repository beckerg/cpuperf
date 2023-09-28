/*
 * Copyright (c) 2021,2023 Greg Becker.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
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
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <float.h>
#include <ctype.h>
#include <sysexits.h>
#include <sys/time.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <pthread.h>
#include <regex.h>

#if __amd64__
#if __has_include(<immintrin.h>)
#include <immintrin.h>
#include <x86intrin.h>
#endif
#endif

#if __FreeBSD__
#include <pthread_np.h>
#include <sys/sysctl.h>
#include <sys/cpuset.h>
#endif

#if __linux__
#include <sched.h>
typedef cpu_set_t cpuset_t;

#define CPU_FFS(_cs)                            \
({                                              \
    size_t i;                                   \
    for (i = 0; i < CPU_SETSIZE; ++i) {         \
        if (CPU_ISSET(i, (_cs)))                \
            break;                              \
    }                                           \
    (i < CPU_SETSIZE) ? (i + 1) : 0;            \
})

#define CPU_COPY(_from, _to)                    \
({                                              \
    CPU_ZERO((_to));                            \
    CPU_OR((_to), (_to), (_from));              \
})
#endif

#include "clp.h"
#include "subr.h"

/* By default we use rdtsc() to measure timing intervals, but if
 * it's not available we'll fall back to using clock_gettime().
 */
#ifndef USE_CLOCK
#define USE_CLOCK           (!HAVE_RDTSC)
#endif

char version[] = PROG_VERSION;
bool headers, shared, testlist;
int verbosity;
uint64_t tsc_freq, seed;
time_t duration;
char *progname;
char *teststr;
char *cpustr;
bool affine;

pthread_barrier_t barrier;
volatile bool testrunning0;
volatile bool testrunning1;

struct clp_posparam posparamv[] = {
    CLP_POSPARAM("cpuid...", string, cpustr, NULL, NULL, "one or more CPU IDs"),
    CLP_POSPARAM_END
};

struct clp_option optionv[] = {
    CLP_OPTION('a',     bool,    affine, NULL, "affine each thread to one vCPU"),
    CLP_OPTION('d',   time_t,  duration, NULL, "specify max per-test duration (seconds)"),
#if !USE_CLOCK
    CLP_OPTION('f', uint64_t,  tsc_freq, NULL, "specify TSC frequency"),
#endif
    CLP_OPTION('H',     bool,   headers, NULL, "suppress headers"),

    CLP_XOPTION('l',    bool,  testlist, "dfH", "list tests",
                "testlist", NULL, NULL, clp_posparam_none),

    CLP_OPTION('S', uint64_t,      seed, NULL, "specify initial PRNG seed"),
    CLP_OPTION('s',     bool,    shared, NULL, "run shared tests"),
    CLP_OPTION('t',   string,   teststr, NULL, "specify which tests to run (via a list of extended regexes)"),

    CLP_OPTION_VERBOSITY(verbosity),
    CLP_OPTION_VERSION(version),
    CLP_OPTION_HELP,
    CLP_OPTION_END
};

struct test {
    subr_func  *func;
    uint64_t    options;
    bool        shared;
    bool        matched;
    const char *name;
    const char *desc;
};

struct test testv[] = {
    { subr_baseline,        0, 1, 1, "baseline",            "framework overhead" },
    { subr_inc_tls,         0, 0, 0, "inc-tls",             "inc tls var" },
    { subr_inc_atomic,      0, 1, 0, "inc-atomic",          "inc atomic (relaxed)" },
    { subr_inc_atomic_cst,  0, 1, 0, "inc-atomic-cst",      "inc atomic (seq cst)" },
    { subr_xoroshiro,       0, 0, 0, "prng-xoroshiro",      "128-bit prng" },
    { subr_mod128,          0, 0, 0, "prng-mod128",         "xoroshiro % 128" },
    { subr_mod127,          0, 0, 0, "prng-mod127",         "xoroshiro % 127" },

#if HAVE_RDRAND64
    { subr_rdrand64,        0, 0, 0, "cpu-rdrand64",        "64-bit prng" },
#endif

#if HAVE_RDTSC
    { subr_rdtsc,           0, 0, 0, "cpu-rdtsc",           "read time stamp counter" },
#endif

#if HAVE_RDTSCP
    { subr_rdtscp,          0, 0, 0, "cpu-rdtscp",          "serialized rdtsc+rdpid" },
#endif

#ifdef __RDPID__
    { subr_rdpid,           0, 0, 0, "cpu-rdpid",           "read processor ID" },
#endif

#if __linux__
    { subr_sched_getcpu,    0, 0, 0, "sys-sched-getcpu",    "read processor ID" },
#endif

#if __amd64__
    { subr_lsl,             0, 0, 0, "cpu-lsl",             "read processor ID" },

    { subr_cpuid,           0, 0, 0, "cpu-cpuid",           "serialize instruction execution" },
    { subr_lfence,          0, 0, 0, "cpu-lfence",          "serialize mem loads" },
    { subr_sfence,          0, 0, 0, "cpu-sfence",          "serialize mem stores" },
    { subr_mfence,          0, 0, 0, "cpu-mfence",          "serialize mem loads+stores" },
    { subr_pause,           0, 0, 0, "cpu-pause",           "spin-wait-loop enhancer" },
#endif

#ifdef CLOCK_REALTIME
    { subr_clock_real,      0, 0, 0, "sys-clock-gettime",   "real time (default)" },
#endif

#ifdef CLOCK_REALTIME_COARSE
    { subr_clock_realfast,  0, 0, 0, "sys-clock-gettime",   "real time (course)" },
#endif

#ifdef CLOCK_MONOTONIC
    { subr_clock_mono,      0, 0, 0, "sys-clock-gettime",   "monotonic time (default)" },
#endif

#ifdef CLOCK_MONOTONIC_COARSE
    { subr_clock_monofast,  0, 0, 0, "sys-clock-gettime",   "monotonic time (course)" },
#endif

    { subr_ticket_spin,     0, 1, 0, "lock-ticket-spin",          "lock+inc+unlock" },
    { subr_cmpxchg_spin,    0, 1, 0, "lock-cmpxchg-spin",         "lock+inc+unlock" },
    { subr_pthread_spin,    0, 1, 0, "lock-pthread-spin-default", "lock+inc+unlock" },
    { subr_pthread_spin,    1, 1, 0, "lock-pthread-spin-pshared", "lock+inc+unlock (pshared)" },
    { subr_pthread_spin_trylock,   0, 1, 0, "lock-pthread-spin-trylock", "trylock+inc+unlock" },

    { subr_pthread_mutex,   0, 1, 0, "lock-pthread-mutex-default", "lock+inc+unlock" },
    { subr_pthread_mutex,   1, 1, 0, "lock-pthread-mutex-pshared", "lock+inc+unlock (pshared)" },
    { subr_pthread_mutex,   2, 1, 0, "lock-pthread-mutex-robust",  "lock+inc+unlock (robust)" },
    { subr_pthread_mutex,   3, 1, 0, "lock-pthread-mutex-inherit", "lock+inc+unlock (inherit)" },
    { subr_pthread_mutex,   4, 1, 0, "lock-pthread-mutex-protect", "lock+inc+unlock (protect)" },
    { subr_pthread_mutex_trylock,  0, 1, 0, "lock-pthread-mutex-trylock", "trylock+inc+unlock" },

    { subr_binsema_mutex,   0, 1, 0, "lock-binsema-mutex",         "wait+inc+post" },
    { subr_binsema_mutex,   1, 1, 0, "lock-binsema-mutex-pshared", "wait+inc+post (pshared)" },

    { subr_pthread_rwlock_wrlock,  0, 1, 0,
      "lock-pthread-rwlock-wrlock", "wrlock+inc+unlock (no readers)" },
    { subr_pthread_rwlock_wrlock,  1, 1, 0,
      "lock-pthread-rwlock-wrlock", "wrlock+inc+unlock (no readers, pshared)" },
    { subr_pthread_rwlock_rdlock,  0, 1, 0,
      "lock-pthread-rwlock-rdlock", "rdlock+inc+unlock (no writers)" },
    { subr_pthread_rwlock_rdlock,  1, 1, 0,
      "lock-pthread-rwlock-rdlock", "rdlock+inc+unlock (no writers, pshared)" },

    { subr_sema,            0, 1, 0, "lock-semaphore",      "wait+inc+post (value=ncpus)" },

    { subr_stack_lockfree,  0, 1, 0, "stack-lockfree",      "pop+inc+push" },
    { subr_stack_mutex,     0, 1, 0, "stack-mutex",         "lock+pop+inc+push+unlock" },
    { subr_stack_sema,      0, 1, 0, "stack-sema",          "lock+pop+inc+push+unlock" },
    { subr_stack_wrlock,    0, 1, 0, "stack-wrlock",        "lock+pop+inc+push+unlock" },

    { NULL, 0, 0, 0, NULL, NULL }
};

static void
eprint(int xerrno, const char *fmt, ...)
{
    char msg[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    fprintf(stderr, "%s: %s%s%s\n",
            progname, msg,
            xerrno ? ": " : "",
            xerrno ? strerror(xerrno) : "");
}

static void
cpuset_print(const cpuset_t *mask, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);

    for (size_t i = 0; i < CPU_SETSIZE; ++i) {
        if (CPU_ISSET(i, mask))
            printf(" %zu", i);
    }
    printf("\n");
}

static void
range_decode(const char *str, cpuset_t *mask)
{
    unsigned long left, right;
    char *end;

    CPU_ZERO(mask);

    while (str) {
        while (isspace(*str) || *str == ',')
            ++str;

        if (!*str)
            break;

        errno = 0;
        left = strtoul(str, &end, 10);
        if (errno || end == str || left >= CPU_SETSIZE) {
            eprint(errno, "unable to convert [%s] to a cpu mask", str);
            exit(EX_USAGE);
        }

        while (isspace(*end))
            ++end;

        right = left;

        if (*end == '-') {
            errno = 0;
            right = strtoul(end + 1, &end, 10);
            if (errno || end == str || right >= CPU_SETSIZE) {
                eprint(errno, "unable to convert [%s] to a cpu mask", str);
                exit(EX_USAGE);
            }

            if (right < left) {
                eprint(errno, "invalid range [%s]", str);
                exit(EX_USAGE);
            }
        }

        while (left <= right) {
            CPU_SET(left, mask);
            left++;
        }

        str = end;
    }
}

/* Interval timer abstractions...
 */
struct itv {
#if HAVE_RDTSCP && !USE_CLOCK
    uint64_t        start;
#else
    struct timespec start;
#endif
};

/* Record the start time of an interval timer (serialized).
 */
static void
itv_start(struct itv *itv)
{
#if HAVE_RDTSCP && !USE_CLOCK
    uint aux;

    itv->start = __builtin_ia32_rdtscp(&aux);
#else
    clock_gettime(CLOCK_MONOTONIC, &itv->start);
#endif
}

/* Return the delta between now and a previously recorded
 * interval start time (serialized).
 */
static inline uint64_t
itv_stop(struct itv *itv)
{
#if HAVE_RDTSCP && !USE_CLOCK
    uint aux;

    return __builtin_ia32_rdtscp(&aux) - itv->start;
#else
    struct timespec *start = &itv->start;
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);

    return (now.tv_sec * 1000000000 + now.tv_nsec) -
        (start->tv_sec * 1000000000 + start->tv_nsec);
#endif
}

/* Return the current time (unserialized).
 */
static inline uint64_t
itv_now(void)
{
#if HAVE_RDTSC && !USE_CLOCK
    return __builtin_ia32_rdtsc();
#else
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);

    return (now.tv_sec * 1000000000) + now.tv_nsec;
#endif
}


static void
affinity_set(const cpuset_t *mask)
{
    int rc;

    rc = pthread_setaffinity_np(pthread_self(), sizeof(*mask), mask);
    if (rc) {
        eprint(rc, "unable to affine thread to CPUs...");
        abort();
    }
}

static void *
test_main(void *arg)
{
    struct subr_args *args = arg;
    struct subr_stats *stats;
    struct itv test_start;
    double latmin, latavg;
    double itv_latmin;
    subr_func *func;
    int rc;

    /* Rendezvous with the leader...
     */
    rc = pthread_barrier_wait(&barrier);
    if (rc > 0) {
        eprint(rc, "thread %zu unable to rendezvous with leader", args->tdnum);
        pthread_exit(NULL);
    }

    func = args->func;
    stats = args->stats;

    /* Spin here to synchronize with all test threads and maybe kick in turbo boost...
     */
    while (itv_now() < stats->delta)
        cpu_pause();

    itv_latmin = DBL_MAX;
    latmin = DBL_MAX;
    latavg = 0;

    itv_start(&test_start);

    /* Repeatedly measure the time for a single call to the test
     * function to try and determine its minimum serialized latency.
     */
    while (testrunning0) {
        struct itv start;
        uint64_t delta;

        /* First, try to measure the interval timer overhead.
         */
        itv_start(&start);
        delta = itv_stop(&start);

        if (delta < itv_latmin)
            itv_latmin = delta;

        /* Repeat with a single call to the test function.
         */
        itv_start(&start);
        func(args);
        delta = itv_stop(&start);

        if (delta < latmin)
            latmin = delta;
        latavg += delta;

        stats->calls++;
    }

    stats->delta = itv_stop(&test_start);

    stats->latavg = latavg / stats->calls;
    stats->latmin = latmin;

    if (stats->latavg < itv_latmin || stats->latmin < itv_latmin) {
        printf("itvlatmin %.2lf, latmin %.2lf, latavg %.2lf\n",
               itv_latmin, stats->latmin, stats->latavg);
    }

    /* Subtract the interval timer overhead to yield the serialized
     * average and minimum latencies of the call to the test function.
     */
    if (stats->latavg >= itv_latmin)
        stats->latavg -= itv_latmin;
    else
        stats->latavg = 0;

    if (stats->latmin >= itv_latmin)
        stats->latmin -= itv_latmin;
    else
        stats->latmin = 0;

    /* Rendezvous with the leader...
     */
    rc = pthread_barrier_wait(&barrier);
    if (rc > 0) {
        eprint(rc, "thread %zu unable to rendezvous with leader", args->tdnum);
        pthread_exit(NULL);
    }

    /* Switch to second stats buffer.
     */
    ++stats;

    /* Spin here to synchronize with all test threads and maybe kick in turbo boost...
     */
    while (itv_now() < stats->delta)
        cpu_pause();

    itv_start(&test_start);

    /* Here we repeatedly call the test function with minimal loop
     * overhead to try and determine the maximum throughput (i.e.,
     * the minimum unserialized latency).
     */
    while (testrunning1) {
        func(args);
        func(args);
        func(args);
        func(args);

        stats->calls += 4;
    }

    stats->delta = itv_stop(&test_start);

    stats->latavg = (double)stats->delta / stats->calls;
    stats->latmin = stats->latavg;

    /* Rendezvous with the leader...
     */
    rc = pthread_barrier_wait(&barrier);
    if (rc > 0) {
        eprint(rc, "thread %zu unable to rendezvous with leader", args->tdnum);
        pthread_exit(NULL);
    }

    pthread_exit(NULL);
}

int
main(int argc, char **argv)
{
    cpuset_t groupv[argc], avail_mask, leader_mask, test_mask, mask;
    size_t datavsz, groupc, tdmax;
    int calls_width, name_width;
    struct subr_data **datav;
    double cyc_baseline;
    struct test *test;
    size_t pagesz;
    long lrc;
    int rc;

    progname = strrchr(argv[0], '/');
    progname = progname ? progname + 1 : argv[0];

    lrc = sysconf(_SC_PAGESIZE);
    if (lrc == -1)
        lrc = 4096;
    pagesz = (size_t)lrc;

    headers = true;
    duration = 15;
    name_width = 8;

    rc = clp_parsev(argc, argv, optionv, posparamv);
    if (rc)
        return rc;

    if (clp_given('h', optionv, NULL) || clp_given('V', optionv, NULL))
        return 0;

    /* If user specified one or more an extended regexes of tests to run
     * then mark each test whose name matches any given regex.
     */
    if (teststr) {
        char *str = teststr, *tok;

        while (( tok = strsep(&str, ",:;\t ") )) {
            char errbuf[256];
            regex_t regex;
            bool exclude;

            exclude = (tok[0] == '!');
            if (exclude)
                tok++;

            rc = regcomp(&regex, tok, REG_EXTENDED);
            if (rc) {
                regerror(rc, &regex, errbuf, sizeof(errbuf));
                eprint(0, "unable to compile regex %s: %s", tok, errbuf);
                exit(EX_USAGE);
            }

            for (test = testv; test->name; ++test) {
                if (shared && !test->shared)
                    continue;

                rc = regexec(&regex, test->name, 0, NULL, 0);
                if (rc) {
                    if (rc == REG_NOMATCH)
                        continue;

                    regerror(rc, &regex, errbuf, sizeof(errbuf));
                    eprint(0, "unable to match regex %s: %s", tok, errbuf);
                    exit(EX_USAGE);
                }

                test->matched = !exclude;
            }

            regfree(&regex);
        }
    }
    else {
        for (test = testv; test->name; ++test)
            test->matched = shared ? test->shared : true;
    }

    /* Find the max name width of all tests we are going to run.
     */
    for (test = testv; test->name; ++test) {
        if (test->matched) {
            int w = strlen(test->name);

            if (w > name_width)
                name_width = w;
        }
    }

    if (clp_given('l', optionv, NULL)) {
        printf("%-*s  %s\n", name_width, "NAME", "DESC");

        for (test = testv; test->name; ++test) {
            if (test->matched)
                printf("%-*s  %s\n", name_width, test->name, test->desc);
        }

        return 0;
    }

    rc = pthread_getaffinity_np(pthread_self(), sizeof(avail_mask), &avail_mask);
    if (rc) {
        eprint(rc, "unable to get thread affinity mask");
        exit(EX_OSERR);
    }

    /* Build a mask of CPUs to test.  Each positional parameter may be either
     * a single CPU ID or a range separated by a single dash character.
     */
    CPU_ZERO(&leader_mask);
    CPU_ZERO(&test_mask);
    CPU_ZERO(&mask);
    groupc = 0;

    for (int i = 0; i < posparamv->argc; ++i) {
        range_decode(posparamv->argv[i], &groupv[groupc]);

        CPU_OR(&test_mask, &test_mask, &groupv[groupc]);
        groupc++;
    }

    CPU_AND(&test_mask, &test_mask, &avail_mask);

    if (CPU_COUNT(&test_mask) < 1) {
        eprint(EINVAL, "at least one CPU ID must be specified");
        exit(EX_USAGE);
    }

    /* Try to affine the leader thread to any CPU not under test.
     */
    CPU_XOR(&leader_mask, &avail_mask, &test_mask);

    if (CPU_COUNT(&leader_mask) == 0) {
        CPU_COPY(&avail_mask, &leader_mask);
    }

    if (verbosity > 1) {
        cpuset_print(&avail_mask, "avail mask:");
        cpuset_print(&leader_mask, "leader mask:");
        cpuset_print(&test_mask, "test mask:");

        for (size_t i = 0; i < groupc; ++i) {
            cpuset_print(&groupv[i], "group %zu mask:", i);
        }
    }

#if USE_CLOCK
    tsc_freq = 1000000000; /* using clock_gettime() for interval measurements */

#elif HAVE_RDTSC

#if __FreeBSD__
    if (!tsc_freq) {
        size_t valsz = sizeof(tsc_freq);

        rc = sysctlbyname("machdep.tsc_freq", &tsc_freq, &valsz, NULL, 0);
        if (rc) {
            eprint(errno, "unable to query sysctlbyname(machdep.tsc_freq)");
            exit(EX_OSERR);
        }
    }

#elif __linux__
    if (!tsc_freq) {
        char linebuf[1024];
        double bogomips;
        FILE *fp;
        int n;

        fp = fopen("/proc/cpuinfo", "r");
        if (fp) {
            while (fgets(linebuf, sizeof(linebuf), fp)) {
                n = sscanf(linebuf, "bogomips%*[^0-9]%lf", &bogomips);
                if (n == 1) {
                    tsc_freq = (bogomips * 1000000) / 2;
                    break;
                }
            }

            fclose(fp);
        }
    }
#endif
#endif

    if (!tsc_freq) {
        eprint(0, "unable to determine TSC frequency, try -f option");
        exit(EX_OSERR);
    }


    /* Allocate an array of pointers to keep track of the per-thread
     * data allocations (one pointer for each thread we're about to
     * create).
     */
    tdmax = 0;
    for (size_t i = 0; i < groupc; ++i) {
        tdmax += CPU_COUNT(&groupv[i]);
    }

    datavsz = sizeof(*datav) * (tdmax + 1);

    datav = aligned_alloc(4096, roundup(datavsz, 4096));
    if (!datav) {
        eprint(errno, "unable to alloc thread data");
        exit(EX_OSERR);
    }

    rc = pthread_barrier_init(&barrier, NULL, CPU_COUNT(&test_mask) + 1);
    if (rc) {
        eprint(errno, "unable to initialize barrier");
        exit(EX_OSERR);
    }

    if (setpriority(PRIO_PROCESS, 0, -20) && verbosity > 0)
        eprint(0, "run as root to reduce jitter");

    if (duration < 1)
        duration = 1;
    cyc_baseline = 0;
    calls_width = 0;

    if (seed == 0)
        seed = itv_now();

    for (test = testv; test->name; ++test) {
        struct subr_args *args_head, **args_nextpp, *args;
        double latavg_total, latmin_total, latavg, latmin;
        uint64_t cyc_start0, cyc_start1;
        uint64_t calls_total;
        double cycles_total;
        double cycles_avg;
        char *suspicious;
        size_t tdcnt;

        if (!test->matched)
            continue;

        memset(datav, 0, datavsz);
        testrunning0 = true;
        testrunning1 = true;
        tdcnt = 0;

        cyc_start0 = itv_now() + tsc_freq;
        cyc_start1 = itv_now() + tsc_freq * 5;

        args_head = NULL;
        args_nextpp = &args_head;

        /* Start one thread for each CPU in each thread group.
         */
        for (size_t tdgrp = 0; tdgrp < groupc; ++tdgrp) {
            struct subr_data *data;
            const cpuset_t *gmask;
            size_t align, cpu;
            cpuset_t gavail;

            gmask = groupv + tdgrp;
            CPU_COPY(gmask, &gavail);

            while (( cpu = CPU_FFS(&gavail) )) {
                size_t datagrp = shared ? tdgrp : tdcnt;

                CPU_CLR(cpu - 1, &gavail);

                /* If the -a option was given then affine this thread
                 * to only this one CPU.  Otherwise, affine this thread
                 * to the set of CPUs given by the current thread group.
                 */
                if (affine) {
                    cpuset_t tmask;

                    CPU_ZERO(&tmask);
                    CPU_SET(cpu - 1, &tmask);
                    affinity_set(&tmask);
                } else {
                    affinity_set(gmask);
                }

                align = __alignof(*args);
                if (align < 128)
                    align = 128;

                /* Each thread gets its own private args object.
                 */
                args = aligned_alloc(align, roundup(sizeof(*args), align));
                if (!args)
                    abort();

                memset(args, 0, sizeof(*args));
                args->stats[0].delta = cyc_start0;
                args->stats[1].delta = cyc_start1;
                args->func = test->func;
                args->options = test->options;
                args->seed = seed + tdcnt;
                args->tdnum = tdcnt;
                args->tdgrp = tdgrp;

                /* Append to the end of the list of args objects.
                 */
                *args_nextpp = args;
                args_nextpp = &args->next;

                /* In shared mode we create a single data object per CPU group
                 * and share it with all threads in that group.  In non-shared
                 * mode each thread gets its own private data object.
                 */
                data = datav[datagrp];
                if (!data) {
                    size_t off, n;

                    align = __alignof(*data);
                    if (align < 128)
                        align = 128;

                    if (align >= pagesz || sizeof(*data) >= pagesz - align)
                        abort();

                    /* Allocate a page, then choose an address within the page
                     * that is a multiple of "align", where the multiple is
                     * based upon the number threads we have created so far...
                     */
                    data = aligned_alloc(pagesz, roundup(sizeof(*data), pagesz));
                    if (!data)
                        abort();

                    n = (pagesz - sizeof(*data)) / align;
                    off = align * (tdcnt % n);
                    data = (void *)((char *)data + off);

                    memset(data, 0, sizeof(*data));
                    data->cpumax = shared ? CPU_COUNT(gmask) : 1;

                    datav[datagrp] = data;
                }

                args->data = data;

                subr_init(args);

                rc = pthread_create(&args->tid, NULL, test_main, args);
                if (rc) {
                    eprint(rc, "unable to create thread %zu", args->tdnum);
                    exit(EX_OSERR);
                }

                tdcnt++;
            }
        }

        if (tdcnt != tdmax) {
            printf("td %zu %zu\n", tdcnt, tdmax);
            abort();
        }

        affinity_set(&leader_mask);

        /* Rendezvous with all test threads...
         */
        rc = pthread_barrier_wait(&barrier);
        if (rc > 0) {
            eprint(rc, "leader unable to rendezvous with test threads");
            pthread_exit(NULL);
        }

        /* Spin here to synchronize with all test threads...
         */
        while (itv_now() < cyc_start0)
            cpu_pause();

        /* The first test loop obtains the minimum serialized latency
         * and runs only for a short time...
         */
        sleep(3);
        testrunning0 = false;

        /* Rendezvous with all test threads...
         */
        rc = pthread_barrier_wait(&barrier);
        if (rc > 0) {
            eprint(rc, "leader unable to rendezvous with test threads");
            pthread_exit(NULL);
        }

        /* Spin here to synchronize with all test threads...
         */
        while (itv_now() < cyc_start1)
            cpu_pause();

        /* The second test loop obtains the maximum throughput and runs
         * for the full duration specified via the -d option.
         */
        usleep(duration * 1000000 - 50000);
        testrunning1 = false;

        /* Rendezvous with all test threads...
         */
        rc = pthread_barrier_wait(&barrier);
        if (rc > 0) {
            eprint(rc, "leader unable to rendezvous with test threads");
            pthread_exit(NULL);
        }

        latavg_total = latavg = 0;
        latmin_total = DBL_MAX;
        latmin = DBL_MAX;
        cycles_total = 0;
        calls_total = 0;

        for (args = args_head; args; args = args->next) {
            struct subr_stats *stats = args->stats;
            double cycles;
            void *res;

            rc = pthread_join(args->tid, &res);
            if (rc) {
                eprint(rc, "unable to join thread %zu", args->tdnum);
                continue;
            }

            subr_fini(args);

            if (args->data->refcnt == 0)
                free((void *)rounddown((uintptr_t)args->data, pagesz));

            cycles = stats[1].delta;
            cycles_total += cycles;

            latavg = stats[1].latavg;
            if (latavg > cyc_baseline)
                latavg -= cyc_baseline;
            latavg_total += latavg;

            latmin = stats[0].latmin;
            if (latmin > cyc_baseline) {
                latmin -= cyc_baseline;
                suspicious = " ";
            } else {
                suspicious = "*";
            }
            if (latmin < latmin_total)
                latmin_total = latmin;

            calls_total += stats[1].calls;

            if (!calls_width) {
                calls_width = snprintf(NULL, 0, " %6.2lf",
                                       (stats[1].calls * CPU_COUNT(&test_mask)) / 1000000.0);
            }

            if (headers) {
                printf("\n%4s %4s %5s %*s %9s %9s %8s %8s   %7s %7s\n",
                       "", "", "MHz",
                       calls_width, "total",
                       "avg",
                       "avg/cpu", "avg/cpu", "avg/cpu",
                       "sermin", "sermin");

                printf("%4s %4s %5s %*s %9s %9s %8s %8s   %7s %7s  %-*s  %s\n",
                       "TID", "GRP",
                       (USE_CLOCK) ? "CLK" : "TSC",
                       calls_width, "MCALLS",
                       "MCALLS/s",
                       "MCALLS/s",
                       (USE_CLOCK) ? "NSECS" : "CYCLES",
                       "NSECS",
                       (USE_CLOCK) ? "NSECS" : "CYCLES",
                       "NSECS",
                       name_width, "NAME", "DESC");

                headers = false;
            }

            if (verbosity > 0) {
                printf("%4zu %4zu %5lu %*.2lf %9.2lf %9.2lf %8.1lf %8.2lf   %7.1lf%s%7.2lf  %-*s  %s\n",
                       args->tdnum,
                       args->tdgrp,
                       tsc_freq / 1000000,
                       calls_width, (stats[1].calls / 1000000.0),
                       (stats[1].calls / (cycles / tsc_freq)) / 1000000,
                       (stats[1].calls / (cycles / tsc_freq)) / 1000000,
                       latavg,
                       (latavg * 1000000000.0) / tsc_freq,
                       latmin,
                       suspicious,
                       (latmin * 1000000000.0) / tsc_freq,
                       name_width, test->name, test->desc);
            }
        }

        cycles_avg = cycles_total / CPU_COUNT(&test_mask);
        latavg = latavg_total / CPU_COUNT(&test_mask);
        latmin = latmin_total;

        printf("%4s %4s %5lu %*.2lf %9.2lf %9.2lf %8.1lf %8.2lf   %7.1lf %7.2lf  %-*s  %s\n",
               "-", "-",
               tsc_freq / 1000000,
               calls_width, (calls_total / 1000000.0),
               (calls_total / (cycles_avg / tsc_freq)) / 1000000,
               (calls_total / (cycles_avg / tsc_freq)) / (CPU_COUNT(&test_mask) * 1000000),
               latavg,
               (latavg * 1000000000.0) / tsc_freq,
               latmin,
               (latmin * 1000000000.0) / tsc_freq,
               name_width, test->name,
               (verbosity > 0) ? "(summary)\n" : test->desc);
        fflush(stdout);

        if (!cyc_baseline)
            cyc_baseline = latmin;

        while (( args = args_head )) {
            args_head = args->next;
            free(args);
        }
    }

    free(teststr);
    free(datav);

    return 0;
}
