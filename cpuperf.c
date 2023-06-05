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
int verbosity;
uint64_t tsc_freq;
time_t duration;
char *progname;
char *teststr;
char *cpustr;

pthread_barrier_t barrier;
volatile bool testrunning0;
volatile bool testrunning1;

struct clp_posparam posparamv[] = {
    CLP_POSPARAM("cpuid...", string, cpustr, NULL, NULL, "one or more CPU IDs"),
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

    CLP_XOPTION('l',    bool,  testlist, "dfHt", "list tests",
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

struct test testv[] = {
    { subr_baseline,        1, 1, "baseline",            "call empty function" },
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
    { subr_rdtsc,           0, 0, "cpu-rdtsc",           "read time stamp counter" },
#endif
#if HAVE_RDTSCP
    { subr_rdtscp,          0, 0, "cpu-rdtscp",          "serialized rdtsc+rdpid" },
#endif
#ifdef __RDPID__
    { subr_rdpid,           0, 0, "cpu-rdpid",           "read processor ID" },
#endif
#if __linux__
    { subr_sched_getcpu,    0, 0, "sys-sched-getcpu",    "read processor ID" },
#endif
#if __amd64__
    { subr_lsl,             0, 0, "cpu-lsl",             "read processor ID" },

    { subr_cpuid,           0, 0, "cpu-cpuid",           "serialize instruction execution" },
    { subr_lfence,          0, 0, "cpu-lfence",          "serialize mem loads" },
    { subr_sfence,          0, 0, "cpu-sfence",          "serialize mem stores" },
    { subr_mfence,          0, 0, "cpu-mfence",          "serialize mem loads+stores" },
    { subr_pause,           0, 0, "cpu-pause",           "spin-wait-loop enhancer" },
#endif
#ifdef CLOCK_REALTIME
    { subr_clock_real,      0, 0, "sys-clock-gettime",   "real time (default)" },
#endif
#ifdef CLOCK_REALTIME_COARSE
    { subr_clock_realfast,  0, 0, "sys-clock-gettime",   "real time (course)" },
#endif
#ifdef CLOCK_MONOTONIC
    { subr_clock_mono,      0, 0, "sys-clock-gettime",   "monotonic time (default)" },
#endif
#ifdef CLOCK_MONOTONIC_COARSE
    { subr_clock_monofast,  0, 0, "sys-clock-gettime",   "monotonic time (course)" },
#endif
    { subr_rwlock_rdlock,   1, 0, "lock-rwlock-rdlock",  "rdlock+inc+unlock (no writers)" },
    { subr_rwlock_wrlock,   1, 0, "lock-rwlock-wrlock",  "wrlock+inc+unlock (no readers)" },
    { subr_ticket,          1, 0, "lock-ticket",         "lock+inc+unlock" },
    { subr_spin_cmpxchg,    1, 0, "lock-spin-cmpxchg",   "lock+inc+unlock" },
    { subr_spin_pthread,    1, 0, "lock-spin-pthread",   "lock+inc+unlock" },
    { subr_mutex_pthread,   1, 0, "lock-mutex-pthread",  "lock+inc+unlock" },
    { subr_mutex_sema,      1, 0, "lock-mutex-sema",     "wait+inc+post (value=1)" },
    { subr_sema,            1, 0, "lock-semaphore",      "wait+inc+post (value=ncpus)" },
    { subr_stack_lockfree,  1, 0, "stack-lockfree",      "pop+inc+push" },
    { subr_stack_mutex,     1, 0, "stack-mutex",         "lock+pop+inc+push+unlock" },
    { subr_stack_sema,      1, 0, "stack-sema",          "lock+pop+inc+push+unlock" },
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
cpuset_print(cpuset_t *mask, const char *fmt, ...)
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

static size_t
group_find(cpuset_t *groupv, size_t groupc, size_t cpu)
{
    size_t i;

    for (i = 0; i < groupc; ++i) {
        if (CPU_ISSET(cpu, &groupv[i]))
            break;
    }

    return i;
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

    return (now.tv_sec * 1000000000) + now.tv_nsec;
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


static void
affinity_set(cpuset_t *maskp, size_t cpu)
{
    cpuset_t mask;
    int rc;

    if (maskp) {
        CPU_COPY(maskp, &mask);
    } else {
        CPU_ZERO(&mask);
        CPU_SET(cpu, &mask);
    }

    rc = pthread_setaffinity_np(pthread_self(), sizeof(&mask), &mask);
    if (rc) {
        eprint(rc, "unable to affine thread to CPUs...");
        abort();
    }
}

static void
affinity_check(size_t cpu)
{
    cpuset_t mask;
    int rc;

    CPU_ZERO(&mask);

    rc = pthread_getaffinity_np(pthread_self(), sizeof(&mask), &mask);

    if (rc || !CPU_ISSET(cpu, &mask) || CPU_COUNT(&mask) != 1) {
        cpuset_print(&mask, "rc %d, cpu %zu:", rc, cpu);
        abort();
    }
}

static void *
test_main(void *arg)
{
    struct subr_args *args = arg;
    struct subr_stats *stats;
    double latmin, latavg;
    subr_func *func;
    int rc;

    affinity_check(args->cpu);

    /* Rendezvous with the leader...
     */
    rc = pthread_barrier_wait(&barrier);
    if (rc > 0) {
        eprint(rc, "CPU %zu unable to rendezvous with leader", args->cpu);
        pthread_exit(NULL);
    }

    func = args->func;
    stats = args->stats;

    /* Spin here to synchronize with all test threads and maybe kick in turbo boost...
     */
    while (itv_cycles() < stats->start)
        cpu_pause();

    stats->start = itv_start();

    latmin = DBL_MAX;
    latavg = 0;

    /* First, we repeatedly measure the time for a single call to the test
     * function to try and determine it's minimum serialized latency.
     */
    while (testrunning0) {
        uint64_t start, stop;

        start = itv_start();
        func(args);
        stop = itv_stop();

        latavg += stop - start;
        if (stop - start < latmin)
            latmin = stop - start;

        stats->calls++;
    }

    stats->stop = itv_stop();

    stats->latavg = latavg / stats->calls;
    stats->latmin = latmin;

    /* Rendezvous with the leader...
     */
    rc = pthread_barrier_wait(&barrier);
    if (rc > 0) {
        eprint(rc, "CPU %zu unable to rendezvous with leader", args->cpu);
        pthread_exit(NULL);
    }

    /* Switch to second stats buffer.
     */
    ++stats;

    /* Spin here to synchronize with all test threads and maybe kick in turbo boost...
     */
    while (itv_cycles() < stats->start)
        cpu_pause();

    stats->start = itv_start();

    /* Next, we repeatedly call the test function with minimal loop
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

    stats->stop = itv_stop();

    stats->latavg = (double)(stats->stop - stats->start) / stats->calls;
    stats->latmin = stats->latavg;

    /* Rendezvous with the leader...
     */
    rc = pthread_barrier_wait(&barrier);
    if (rc > 0) {
        eprint(rc, "CPU %zu unable to rendezvous with leader", args->cpu);
        pthread_exit(NULL);
    }

    pthread_exit(NULL);
}

int
main(int argc, char **argv)
{
    cpuset_t groupv[argc], avail_mask, leader_mask, test_mask, mask;
    struct subr_data **datav;
    double cyc_baseline;
    struct test *test;
    size_t maxcpuidx;
    int calls_width;
    int name_width;
    size_t datavsz;
    size_t groupc;
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

    if (verbosity > 0) {
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

    for (maxcpuidx = CPU_SETSIZE - 1; maxcpuidx >= 0; --maxcpuidx) {
        if (CPU_ISSET(maxcpuidx, &test_mask))
            break;
    }

    datavsz = sizeof(*datav) * (maxcpuidx + 1);

    datav = aligned_alloc(4096, roundup(datavsz, 4096));
    if (!datav) {
        eprint(errno, "unable to alloc thread data");
        exit(EX_OSERR);
    }

    if (duration < 1)
        duration = 1;
    cyc_baseline = 0;
    calls_width = 0;

    rc = pthread_barrier_init(&barrier, NULL, CPU_COUNT(&test_mask) + 1);
    if (rc) {
        eprint(errno, "unable to initialize barrier");
        exit(EX_OSERR);
    }

    if (setpriority(PRIO_PROCESS, 0, -20) && verbosity > 0)
        eprint(0, "run as root to reduce jitter");

    for (test = testv; test->name; ++test) {
        struct subr_args *args_head, **args_nextpp, *args;
        double latavg_total, latmin_total, latavg, latmin;
        uint64_t cyc_start0, cyc_start1;
        uint64_t calls_total;
        double cycles_total;
        double cycles_avg;
        char *suspicious;
        size_t nthreads;

        if (!test->matched)
            continue;

        memset(datav, 0, datavsz);
        testrunning0 = true;
        testrunning1 = true;
        nthreads = 0;

        cyc_start0 = itv_cycles() + tsc_freq;
        cyc_start1 = itv_cycles() + tsc_freq * 5;

        args_head = NULL;
        args_nextpp = &args_head;

        /* Start one thread for each CPU specified on the command line.
         */
        for (size_t i = 0; i < CPU_SETSIZE; ++i) {
            struct subr_data *data;
            size_t group, align;

            if (!CPU_ISSET(i, &test_mask))
                continue;

            /* Switch to this CPU so that allocation of args and data
             * come from the local NUMA node.
             */
            affinity_set(NULL, i);

            align = __alignof(*args);
            if (align < 128)
                align = 128;

            args = aligned_alloc(align, roundup(sizeof(*args), align));
            if (!args)
                abort();

            memset(args, 0, sizeof(*args));
            args->cpu = i;
            args->func = test->func;
            args->stats[0].start = cyc_start0;
            args->stats[1].start = cyc_start1;

            /* Append to the end of the list of args objects.
             */
            *args_nextpp = args;
            args_nextpp = &args->next;

            if (shared) {
                group = group_find(groupv, groupc, i);
            } else {
                group = nthreads;
            }

            data = datav[group];
            if (!data) {
                align = __alignof(*data);
                if (align < 128)
                    align = 128;

                data = aligned_alloc(align, roundup(sizeof(*data), align));
                if (!data)
                    abort();

                memset(data, 0, sizeof(*data));
                data->cpumax = shared ? CPU_COUNT(&groupv[group]) : 1;

                datav[group] = data;
            }

            args->data = data;

            subr_init(args);

            rc = pthread_create(&args->tid, NULL, test_main, args);
            if (rc) {
                eprint(rc, "unable to create pthread for cpu %zu", args->cpu);
                exit(EX_OSERR);
            }

            nthreads++;
        }

        affinity_set(&leader_mask, 0);

        /* Rendezvous with all test threads...
         */
        rc = pthread_barrier_wait(&barrier);
        if (rc > 0) {
            eprint(rc, "leader unable to rendezvous with test threads");
            pthread_exit(NULL);
        }

        /* Spin here to synchronize with all test threads...
         */
        while (itv_cycles() < cyc_start0)
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
        while (itv_cycles() < cyc_start1)
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
                eprint(rc, "unable to join pthread for CPU %zu", args->cpu);
                continue;
            }

            subr_fini(args);

            if (args->data->refcnt == 0)
                free(args->data);

            cycles = stats[1].stop - stats[1].start;
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
                printf("\n%4s %5s %*s %9s %9s %8s %8s   %7s %7s\n",
                       "", "MHz",
                       calls_width, "total",
                       "avg",
                       "avg/cpu", "avg/cpu", "avg/cpu",
                       "sermin", "sermin");

                printf("%4s %5s %*s %9s %9s %8s %8s   %7s %7s  %-*s  %s\n",
                       "CPU",
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

            if (verbosity > 1) {
                printf("%4zu %5lu %*.2lf %9.2lf %9.2lf %8.1lf %8.2lf   %7.1lf%s%7.2lf  %-*s  %s\n",
                       args->cpu,
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

        printf("%4s %5lu %*.2lf %9.2lf %9.2lf %8.1lf %8.2lf   %7.1lf %7.2lf  %-*s  %s\n",
               "-",
               tsc_freq / 1000000,
               calls_width, (calls_total / 1000000.0),
               (calls_total / (cycles_avg / tsc_freq)) / 1000000,
               (calls_total / (cycles_avg / tsc_freq)) / (CPU_COUNT(&test_mask) * 1000000),
               latavg,
               (latavg * 1000000000.0) / tsc_freq,
               latmin,
               (latmin * 1000000000.0) / tsc_freq,
               name_width, test->name, test->desc);
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
