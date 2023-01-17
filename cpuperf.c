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

#if __has_include(<immintrin.h>)
#include <immintrin.h>
#include <x86intrin.h>
#endif

#if __FreeBSD__
#include <pthread_np.h>
#include <sys/cpuset.h>
#include <sys/sysctl.h>
#endif

#if __linux__
#include <sched.h>
typedef cpu_set_t cpuset_t;

#define CPU_FFS(_cs)                            \
({                                              \
    size_t i;                                   \
    for (i = 0; i < CPU_SETSIZE; ++i)           \
        if (CPU_ISSET(i, (_cs)))                \
            break;                              \
    i;                                          \
})
#endif

#ifndef __aligned
#define __aligned(_size)    __attribute__((__aligned__(_size)))
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
uint64_t tsc_freq;
time_t duration;
char *progname;
char *teststr;
int verbosity;
int left;

volatile bool testrunning0;
volatile bool testrunning1;

struct clp_posparam posparamv[] = {
    CLP_POSPARAM("cpuid...", string, left, NULL, NULL, "one or more CPU IDs"),
    CLP_POSPARAM_END
};

struct clp_option optionv[] = {
    CLP_OPTION('d',   time_t,  duration, NULL, "specify max per-test duration (seconds)"),
#if !USE_CLOCK
    CLP_OPTION('f', uint64_t,  tsc_freq, NULL, "specify TSC frequency"),
#endif
    CLP_OPTION('H',     bool,   headers, NULL, "suppress headers"),
    CLP_OPTION('s',     bool,    shared, NULL, "run shared tests"),
    CLP_OPTION('t',   string,   teststr, NULL, "specify tests to run"),

    CLP_XOPTION('l',    bool,  testlist, NULL, "list tests",
                "testlist", NULL, NULL, clp_posparam_none),

    CLP_OPTION_VERBOSITY(verbosity),
    CLP_OPTION_VERSION(version),
    CLP_OPTION_HELP,
    CLP_OPTION_END
};

struct test {
    subr_func  *func;
    bool        shared;
    bool        matched;
    const char *name;
    const char *desc;
};

struct stats {
    uint64_t    start;
    uint64_t    stop;
    double      latmin;
    double      latavg;
    uint64_t    calls;
};

struct tdargs {
    struct testdata *data;
    pthread_t        tid;
    size_t           cpu;
    struct test     *test;
    struct stats     stats[2];

    struct testdata testdata __aligned(128);
};

struct test testv[] = {
    { subr_baseline,        1, 1, "baseline",            "baseline" },
    { subr_inc_tls,         0, 0, "inc-tls",             "inc tls var" },
    { subr_inc_atomic,      1, 0, "inc-atomic",          "inc atomic (relaxed)" },
    { subr_inc_atomic_cst,  1, 0, "inc-atomic-cst",      "inc atomic (seq cst)" },
    { subr_xoroshiro,       0, 0, "prng-xoroshiro",      "128-bit prng" },
    { subr_mod128,          0, 0, "prng-mod128",         "xoroshiro % 128" },
    { subr_mod127,          0, 0, "prng-mod127",         "xoroshiro % 127" },
#if HAVE_RDRAND64
    { subr_rdrand64,        0, 0, "cpu-rdrand64",        "64-bit prng" },
#endif
#if HAVE_RDTSC
    { subr_rdtsc,           0, 0, "cpu-rdtsc",           "rdtsc" },
#endif
#if HAVE_RDTSCP
    { subr_rdtscp,          0, 0, "cpu-rdtscp",          "rdtsc+rdpid" },
#endif
#ifdef __RDPID__
    { subr_rdpid,           0, 0, "cpu-rdpid",           "rdpid" },
#endif
#if __linux__
    { subr_sched_getcpu,    0, 0, "sys-sched-getcpu",    "rdpid" },
#endif
#if __amd64__
    { subr_lsl,             0, 0, "cpu-lsl",             "rdpid" },

    { subr_cpuid,           0, 0, "cpu-cpuid",           "serialize execution" },
    { subr_lfence,          0, 0, "cpu-lfence",          "serialize mem loads" },
    { subr_sfence,          0, 0, "cpu-sfence",          "serialize mem stores" },
    { subr_mfence,          0, 0, "cpu-mfence",          "serialize mem loads+stores" },
    { subr_pause,           0, 0, "cpu-pause",           "spin-wait-loop enhancer" },
#endif
    { subr_clock,           0, 0, "sys-clock-gettime",   "monotonic" },
    { subr_ticket,          1, 0, "lock-ticket",         "lock+inc+unlock" },
    { subr_spin,            1, 0, "lock-spin-cmpxchg",   "lock+inc+unlock" },
    { subr_ptspin,          1, 0, "lock-spin-pthread",   "lock+inc+unlock" },
    { subr_mutex,           1, 0, "lock-mutex-pthread",  "lock+inc+unlock" },
    { subr_sema,            1, 0, "lock-semaphore",      "wait+inc+post (uncontended)" },
    { subr_slstack,         1, 0, "stack-spinlock",      "pop+inc+push" },
    { subr_lfstack,         1, 0, "stack-lockfree",      "pop+inc+push" },
    { NULL, 0, 0, NULL, NULL }
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
cpuset_print(const char *msg, cpuset_t *mask)
{
    printf("%s: ", msg);

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


/* Time interval measurement abstractions...
 */
static inline uint64_t
itv_cycles(void)
{
#if HAVE_RDTSC && !USE_CLOCK
    return __rdtsc();
#else
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);

    return now.tv_sec * 1000000000 + now.tv_nsec;
#endif
}

static inline uint64_t
itv_start(void)
{
#if HAVE_RDTSC && !USE_CLOCK
    __asm__ volatile ("cpuid" ::: "eax","ebx","ecx","edx","memory");
#endif

    return itv_cycles();
}

static inline uint64_t
itv_stop(void)
{
#if HAVE_RDTSCP && !USE_CLOCK
    uint aux;

    return __rdtscp(&aux);
#else
    return itv_cycles();
#endif
}


static void *
test_main(void *arg)
{
    struct tdargs *args = arg;
    double latmin, latavg;
    struct testdata *data;
    struct stats *stats;
    struct test *test;
    subr_func *func;
    cpuset_t nmask;
    u_long iters;
    int rc;

    CPU_ZERO(&nmask);
    CPU_SET(args->cpu, &nmask);

    rc = pthread_setaffinity_np(pthread_self(), sizeof(nmask), &nmask);
    if (rc) {
        eprint(EINVAL, "unable to affine thread %lu to CPU %zu", args->tid, args->cpu);
        pthread_exit(NULL);
    }

    test = args->test;
    func = test->func;
    data = args->data;
    stats = args->stats;

    latmin = DBL_MAX;
    latavg = 0;
    iters = 0;

    /* Spin here to synchronize with all threads and maybe kick in turbo boost...
     */
    while (itv_cycles() < stats->start) {
        if (itv_cycles() % 8 == 0) {
            cpu_pause();
        }
    }

    stats->start = itv_start();

    while (testrunning0) {
        uint64_t start, stop;

        start = itv_start();

        func(data);

        stop = itv_stop();

        latavg += stop - start;
        if (stop - start < latmin)
            latmin = stop - start;

        ++iters;
    }

    stats->stop = itv_stop();

    stats->latavg = latavg / iters;
    stats->latmin = latmin;
    stats->calls = iters;


    latmin = DBL_MAX;
    latavg = 0;
    iters = 0;

    ++stats;

    /* Spin here to synchronize with all threads and maybe kick in turbo boost...
     */
    while (itv_cycles() < stats->start)
        cpu_pause();

    stats->start = itv_start();

    while (testrunning1) {
        func(data);
        func(data);
        func(data);
        func(data);
        ++iters;
    }

    stats->stop = itv_stop();

    stats->latavg = (stats->stop - stats->start) / (iters * 4.0);
    stats->latmin = stats->latavg;
    stats->calls = iters * 4;

    pthread_exit(NULL);
}

int
main(int argc, char **argv)
{
    cpuset_t avail_mask, test_mask, mask;
    struct tdargs *tdargsv;
    double cyc_baseline;
    struct test *test;
    size_t tdargsvsz;
    int calls_width;
    int name_width;
    int rc;

    progname = strrchr(argv[0], '/');
    progname = progname ? progname + 1 : argv[0];

    headers = true;
    duration = 10;
    name_width = 8;

    rc = clp_parsev(argc, argv, optionv, posparamv);
    if (rc)
        return rc;

    if (clp_given('h', optionv, NULL) || clp_given('V', optionv, NULL))
        return 0;

    /* If user specified a list of tests to run then mark each test
     * whose name partially matches the given list.
     */
    if (teststr) {
        char *str = teststr, *tok;

        while (( tok = strsep(&str, ",:;\t ") )) {
            for (test = testv; test->name; ++test) {
                if (!test->matched) {
                    if (shared && !test->shared)
                        continue;

                    test->matched = strstr(test->name, tok);
                }
            }
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
        eprint(rc, "unable to get cpu affinity");
        exit(EX_OSERR);
    }

    /* Build a mask of CPUs to test.  Each positional parameter may be either
     * a single CPU ID or a range separated by a single dash character.
     */
    CPU_ZERO(&mask);
    for (int i = 0; i < posparamv->argc; ++i)
        range_decode(posparamv->argv[i], &mask);

    CPU_AND(&test_mask, &avail_mask, &mask);

    if (CPU_COUNT(&test_mask) < 1) {
        eprint(EINVAL, "at least one cpu ID must be specified");
        exit(EX_USAGE);
    }

    if (verbosity > 0 || CPU_COUNT(&test_mask) < CPU_COUNT(&mask))
        cpuset_print("testing cpus", &test_mask);

    /* Try to run the leader thread on any CPU not given on the command line.
     */
    CPU_XOR(&mask, &avail_mask, &test_mask);

    if (CPU_COUNT(&mask) > 0) {
        rc = pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask);
        if (rc) {
            eprint(rc, "unable to set leader cpu affinity");
        }
    }

    if (verbosity > 0)
        cpuset_print("leader cpus", &mask);


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

    tdargsvsz = sizeof(*tdargsv) * posparamv->argc;

    tdargsv = aligned_alloc(4096, roundup(tdargsvsz, 4096));
    if (!tdargsv) {
        eprint(errno, "unable to alloc thread args");
        exit(EX_OSERR);
    }

    if (duration < 1)
        duration = 1;
    cyc_baseline = 0;
    calls_width = 0;

    if (setpriority(PRIO_PROCESS, 0, -20) && verbosity > 0)
        eprint(0, "run as root to reduce jitter");

    for (test = testv; test->name; ++test) {
        double cptavgtot, cptmintot, cptavg, cptmin;
        struct testdata *shared_data = NULL;
        uint64_t cyc_start, calls_total;
        //double cyclestot;
        char *suspicious;

        if (!test->matched)
            continue;

        memset(tdargsv, 0, tdargsvsz);
        cyc_start = itv_cycles() + tsc_freq;
        testrunning0 = true;
        testrunning1 = true;

        if (shared && test->shared && !shared_data) {
            struct tdargs *args = tdargsv + CPU_FFS(&test_mask);

            shared_data = &args->testdata;
        }

        /* Start one test thread for each cpu given on the command line.
         */
        for (size_t i = 0; i < CPU_SETSIZE; ++i) {
            struct tdargs *args = tdargsv + i;

            if (!CPU_ISSET(i, &test_mask))
                continue;

            args->cpu = i;
            args->stats[0].start = cyc_start;
            args->stats[1].start = cyc_start + (duration / 2.0) + tsc_freq;
            args->test = test;

            args->data = &args->testdata;
            atomic_store(&args->data->refcnt, 0);
            args->data->cpumax = CPU_COUNT(&test_mask);

            if (shared_data)
                args->data = shared_data;

            subr_init(args->data, test->func);

            rc = pthread_create(&args->tid, NULL, test_main, args);
            if (rc) {
                eprint(rc, "unable to create pthread %d for cpu %zu", i, args->cpu);
                exit(EX_OSERR);
            }
        }

        cptavgtot = cptavg = 0;
        cptmintot = DBL_MAX;
        cptmin = DBL_MAX;
        //cyclestot = 0;
        calls_total = 0;

        sleep((duration / 2.0) + 1);
        testrunning0 = false;

        sleep(duration + 1);
        testrunning1 = false;

        for (size_t i = 0; i < CPU_SETSIZE; ++i) {
            struct tdargs *args = tdargsv + i;
            struct stats *stats = args->stats;
            //double cycles;
            void *res;

            if (!CPU_ISSET(i, &test_mask))
                continue;

            rc = pthread_join(args->tid, &res);
            if (rc) {
                eprint(rc, "unable to join pthread %d for cpu %zu", i, args->cpu);
                continue;
            }

            subr_fini(args->data, test->func);

            //cycles = stats[1].stop - stats[1].start;
            //cyclestot += cycles;

            cptavg = stats[1].latavg;
            if (cptavg > cyc_baseline)
                cptavg -= cyc_baseline;
            cptavgtot += cptavg;

            cptmin = stats[0].latmin;
            if (cptmin > cyc_baseline) {
                cptmin -= cyc_baseline;
                suspicious = " ";
            } else {
                suspicious = "*";
            }
            if (cptmin < cptmintot)
                cptmintot = cptmin;

            calls_total += stats[1].calls;

            if (!calls_width) {
                calls_width = snprintf(NULL, 0, " %6.2lf",
                                       (stats[1].calls * CPU_COUNT(&test_mask)) / 1000000.0);
            }

            if (headers) {
                printf("\n%3s %5s %*s %9s %7s %7s   %7s %7s\n",
                       "", "MHz", calls_width, "tot",
                       "avg", "avg", "avg", "sermin", "sermin");

                printf("%3s %5s %*s %9s %7s %7s   %7s %7s  %-*s  %s\n",
                       (verbosity > 0) ? "CPU" : "-",
                       (USE_CLOCK) ? "CLK" : "TSC",
                       calls_width, "MCALLS",
                       "MCALLS/s",
                       (USE_CLOCK) ? "NSECS" : "CYCLES",
                       "NSECS",
                       (USE_CLOCK) ? "NSECS" : "CYCLES",
                       "NSECS",
                       name_width, "NAME", "DESC");

                headers = false;
            }

            if (verbosity > 0) {
                printf("%3zu %5lu %*.2lf %9.2lf %7.1lf %7.2lf   %7.1lf%s %7.2lf  %-*s  %s\n",
                       args->cpu,
                       tsc_freq / 1000000,
                       calls_width, (stats[1].calls / 1000000.0),
                       tsc_freq / (cptavg * 1000000),
                       cptavg,
                       (cptavg * 1000000000.0) / tsc_freq,
                       cptmin,
                       suspicious,
                       (cptmin * 1000000000.0) / tsc_freq,
                       name_width, test->name, test->desc);
            }
        }

        cptavg = cptavgtot / CPU_COUNT(&test_mask);
        cptmin = cptmintot;

        printf("%3s %5lu %*.2lf %9.2lf %7.1lf %7.2lf   %7.1lf %7.2lf  %-*s  %s\n",
               "-",
               tsc_freq / 1000000,
               calls_width, (calls_total / 1000000.0),
               tsc_freq / (cptavg * 1000000),
               cptavg,
               (cptavg * 1000000000.0) / tsc_freq,
               cptmin,
               (cptmin * 1000000000.0) / tsc_freq,
               name_width, test->name, test->desc);
        fflush(stdout);

        if (!cyc_baseline)
            cyc_baseline = cptmin;
    }

    free(teststr);
    free(tdargsv);

    return 0;
}
