/*
 * Copyright (c) 2021 Greg Becker.  All rights reserved.
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
#include <sysexits.h>
#include <sys/time.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <pthread.h>

#include <immintrin.h>
#include <x86intrin.h>

#if __FreeBSD__
#include <pthread_np.h>
#include <sys/cpuset.h>
#include <sys/sysctl.h>
#endif

#if __linux__
#include <sched.h>
typedef cpu_set_t cpuset_t;
#endif

#include "clp.h"
#include "subr.h"

#ifndef __aligned
#define __aligned(_size)    __attribute__((__aligned__(_size)))
#endif

bool headers, shared, unserialized;
uint64_t tsc_freq;
time_t duration;
char *progname;
int verbosity;
int semval;
int left;

volatile bool testrunning;

struct clp_posparam posparamv[] = {
    CLP_POSPARAM("cpuid...", int, left, NULL, NULL, "one or more CPU IDs"),
    CLP_POSPARAM_END
};

struct clp_option optionv[] = {
    CLP_OPTION('d', time_t,   duration,     NULL, "specify max duration (seconds)"),
    CLP_OPTION('H', bool,     headers,      NULL, "suppress headers"),
    CLP_OPTION('f', uint64_t, tsc_freq,     NULL, "specify processor cycles/second"),
    CLP_OPTION('i', int,      semval,       NULL, "set initial semaphore value"),
    CLP_OPTION('s', bool,     shared,       NULL, "let threads share data if applicable"),
    CLP_OPTION('u', bool,     unserialized, NULL, "do not serialize test function calls"),
    CLP_OPTION_VERBOSITY(verbosity),
    CLP_OPTION_HELP,
    CLP_OPTION_END
};

struct test {
    uintptr_t (*func)(struct testdata *);
    int       (*init)(struct testdata *);
    bool        shared;
    const char *name;
    const char *desc;
};

struct tdargs {
    struct testdata *data;
    pthread_t        tid;
    uint             cpu;
    struct test     *test;
    uint64_t         c_start;
    uint64_t         c_stop;
    double           latmin;
    double           latavg;

    struct testdata testdata __aligned(128);
};

struct test testv[] = {
    { subr_baseline,                   NULL, 0, "baseline",      "baseline" },
    { subr_inc_tls,                    NULL, 0, "inc_tls",       "inc tls var" },
    { subr_inc_atomic,                 NULL, 0, "inc_atomic",    "inc atomic (relaxed)" },
    { subr_xoroshiro,   subr_xoroshiro_init, 0, "xoroshiro",     "128-bit prng" },
    { subr_mod128,      subr_xoroshiro_init, 0, "mod128",        "xoroshiro % 128" },
    { subr_mod127,      subr_xoroshiro_init, 0, "mod127",        "xoroshiro % 127" },
#if __amd64__
    { subr_rdtsc,                      NULL, 0, "rdtsc",         "rdtsc" },
    { subr_rdtscp,                     NULL, 0, "rdtscp",        "rdtscp (rdtsc+rdpid)" },
    { subr_cpuid,                      NULL, 0, "cpuid",         "cpuid (serialization)" },
    { subr_lsl,                        NULL, 0, "lsl",           "getcpu" },
#ifdef __RDPID__
    { subr_rdpid,                      NULL, 0, "rdpid",         "getcpu" },
#endif
#endif
#if __linux__
    { subr_sched_getcpu,               NULL, 0, "sched_getcpu",  "getcpu" },
#endif
    { subr_clock,                      NULL, 0, "clock_gettime", "monotonic" },
    { subr_ticket,                     NULL, 1, "ticket",        "lock+inc+unlock" },
    { subr_spin,                       NULL, 1, "spin-cmpxchg",  "lock+inc+unlock" },
    { subr_ptspin,         subr_ptspin_init, 1, "spin-pthread",  "lock+inc+unlock" },
    { subr_mutex,           subr_mutex_init, 1, "mutex-pthread", "lock+inc+unlock" },
    { subr_sem,               subr_sem_init, 1, "semaphore",     "wait+inc+post" },
    { subr_slstack,       subr_slstack_init, 1, "slstack",       "pop+inc+push (spinlock)" },
    { subr_lfstack,       subr_lfstack_init, 1, "lfstack",       "pop+inc+push (lockfree)" },
    { NULL }
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

static void *
test_main(void *arg)
{
    uintptr_t (*func)(struct testdata *);
    struct tdargs *args = arg;
    double latmin, latavg;
    struct testdata *data;
    struct test *test;
    cpuset_t nmask;
    long i;
    uint aux;
    int rc;

    CPU_ZERO(&nmask);
    CPU_SET(args->cpu, &nmask);

    rc = pthread_setaffinity_np(pthread_self(), sizeof(nmask), &nmask);
    if (rc) {
        eprint(EINVAL, "unable to set cpu affinity to CPU %u", args->cpu);
        pthread_exit(NULL);
    }

    test = args->test;
    func = test->func;
    data = args->data;

    latmin = DBL_MAX;
    latavg = 0;

    /* Spin here to synchronize with all threads and maybe kick in turbo boost...
     */
    while (__rdtsc() < args->c_start)
        _mm_pause();

    if ((shared && test->shared) || unserialized) {
        args->c_start = __rdtscp(&aux);

        for (i = 0; testrunning; ++i) {
            func(data);
            func(data);
            func(data);
            func(data);
        }

        args->c_stop = __rdtscp(&aux);

        args->latavg = (args->c_stop - args->c_start) / (i * 4.0);
        args->latmin = args->latavg;
    }
    else {
        unsigned long long start, stop;

        args->c_start = __rdtscp(&aux);

        for (i = 0; testrunning; ++i) {
            __asm__ volatile ("cpuid" ::: "eax","ebx","ecx","edx","memory");
            start = __rdtsc();

            func(data);

            stop = __rdtscp(&aux);

            latavg += stop - start;
            if (stop - start < latmin)
                latmin = stop - start;
        }

        args->c_stop = __rdtscp(&aux);

        args->latavg = latavg / i;
        args->latmin = latmin;
    }

    assert((aux & 0xfff) == args->cpu);

    pthread_exit(NULL);
}

static bool
given(int c)
{
    return !!clp_given(c, optionv, NULL);
}

int
main(int argc, char **argv)
{
    struct tdargs *tdargsv;
    struct test *test;
    double c_baseline;
    size_t tdargsvsz;
    cpuset_t omask;
    int namewidth;
    int rc;

    progname = strrchr(argv[0], '/');
    progname = progname ? progname + 1 : argv[0];

    duration = 10;
    headers = true;

    rc = clp_parsev(argc, argv, optionv, posparamv);
    if (rc)
        return rc;

    if (given('h') || given('V'))
        return 0;

    /* Try to run the leader thread on any CPU not given on the command line.
     */
    rc = pthread_getaffinity_np(pthread_self(), sizeof(omask), &omask);
    if (rc) {
        eprint(0, "unable to get cpu affinity");
    } else {
        for (int i = 0; i < posparamv->argc; ++i) {
            int cpu = atoi(posparamv->argv[i]);

            CPU_CLR(cpu, &omask);
        }

        rc = pthread_setaffinity_np(pthread_self(), sizeof(omask), &omask);
        if (rc) {
            eprint(0, "unable to set leader cpu affinity");
        }
    }

#if __amd64__
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

    if (!tsc_freq) {
        const char cmd[] = "lscpu | sed -En 's/^BogoMIPS[^0-9]*([0-9.]*)$/\\1/p'";
        char buf[32];
        FILE *fp;

        fp = popen(cmd, "r");
        if (fp) {
            if (fgets(buf, sizeof(buf), fp)) {
                tsc_freq = (strtod(buf, NULL) * 1000000) / 2;
            }
            pclose(fp);
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

    namewidth = 8;

    for (test = testv; test->name; ++test) {
        int w = strlen(test->name);

        if (w > namewidth)
            namewidth = w;
    }

    if (headers) {
        printf("\n%3s %5s %8s %10s %7s %10s %7s  %7s\n",
               "", "", "", "avg", "avg", "max", "min", "min");

        printf("%3s %5s %8s %10s %7s %10s %7s  %7s  %-*s  %s\n",
               "CPU", "TSC", "ELAPSED",
               "ITERS/SEC", "CYCLES",
               "ITERS/SEC", "CYCLES",
               "NSECS",
               namewidth, "TEST", "DESC");
    }

    c_baseline = 0;

    if (setpriority(PRIO_PROCESS, 0, -20) && verbosity > 0)
        eprint(0, "run as root to reduce jitter");

    for (test = testv; test->name; ++test) {
        double cptavgtot, cptmintot;
        double cptavg, cptmin;
        char *suspicious;
        double cyclestot;
        uint64_t c_start;

        memset(tdargsv, 0, tdargsvsz);
        c_start = __rdtsc() + tsc_freq;
        testrunning = true;

        for (int i = 0; i < posparamv->argc; ++i) {
            struct tdargs *args = tdargsv + i;

            args->cpu = atoi(posparamv->argv[i]);
            args->c_start = c_start;
            args->c_stop = c_start + duration * tsc_freq;
            args->test = test;
            args->data = &args->testdata;
            args->data->cpumax = posparamv->argc;

            if (test->init)
                test->init(args->data);

            if (shared && test->shared)
                args->data = &tdargsv->testdata;

            rc = pthread_create(&args->tid, NULL, test_main, args);
            if (rc) {
                eprint(rc, "unable to create pthread %d for cpu %u", i, args->cpu);
                exit(EX_OSERR);
            }
        }

        cptavgtot = cptavg = 0;
        cptmintot = DBL_MAX;
        cptmin = DBL_MAX;
        cyclestot = 0;

        sleep(duration + 1);
        testrunning = false;

        for (int i = 0; i < posparamv->argc; ++i) {
            struct tdargs *args = tdargsv + i;
            double cycles;
            void *res;

            rc = pthread_join(args->tid, &res);
            if (rc) {
                eprint(rc, "unable to join pthread %d for cpu %u", i, args->cpu);
                continue;
            }

            cycles = args->c_stop - args->c_start;
            cyclestot += cycles;

            cptavg = args->latavg;
            if (cptavg > c_baseline)
                cptavg -= c_baseline;
            cptavgtot += cptavg;

            cptmin = args->latmin;
            if (cptmin > c_baseline) {
                cptmin -= c_baseline;
                suspicious = " ";
            } else {
                suspicious = "*";
            }
            if (cptmin < cptmintot)
                cptmintot = cptmin;

            if (verbosity > 0) {
                printf("%3u %5lu %8.3lf %10.2lf %7.1lf %10.2lf %7.1lf%s %7.2lf  %-*s  %s\n",
                       args->cpu, tsc_freq / 1000000,
                       cycles / tsc_freq,
                       tsc_freq / (cptavg * 1000000),
                       cptavg,
                       tsc_freq / (cptmin * 1000000),
                       cptmin,
                       suspicious,
                       (cptmin * 1000000000.0) / tsc_freq,
                       namewidth, test->name, test->desc);
            }
        }

        cptavg = cptavgtot / posparamv->argc;
        cptmin = cptmintot;
        if (cptmin > cptavg) {
            cptmin = cptavg;
            suspicious = "*";
        } else {
            suspicious = " ";
        }

        printf("%3s %5lu %8.3lf %10.2lf %7.1lf %10.2lf %7.1lf%s %7.2lf  %-*s  %s\n",
               "-", tsc_freq / 1000000,
               (cyclestot / tsc_freq) / posparamv->argc,
               tsc_freq / (cptavg * 1000000),
               cptavg,
               tsc_freq / (cptmin * 1000000),
               cptmin,
               suspicious,
               (cptmin * 1000000000.0) / tsc_freq,
               namewidth, test->name, test->desc);
        fflush(stdout);

        if (!c_baseline)
            c_baseline = cptmin;
    }

    free(tdargsv);

    return 0;
}
