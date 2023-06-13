/*
 * Copyright (c) 2021,2023 Greg Becker.  All rights reserved.
 */

#ifndef SUBR_H
#define SUBR_H

#include <stdint.h>
#include <stdatomic.h>
#include <unistd.h>
#include <time.h>
#include <semaphore.h>
#include <pthread.h>

#if __has_include(<immintrin.h>)
#include <immintrin.h>
#include <x86intrin.h>
#endif

#define HAVE_RDTSC          (__has_builtin(__builtin_ia32_rdtsc))
#define HAVE_RDTSCP         (__has_builtin(__builtin_ia32_rdtscp))
#define HAVE_RDRAND64       (__has_builtin(__builtin_ia32_rdrand64_step))
#define HAVE_PAUSE          (__has_builtin(__builtin_ia32_pause))

struct subr_inc {
    atomic_ullong cnt;
};

struct subr_clock {
    struct timespec ts;
};

struct subr_tsc {
    uint aux;
};

struct subr_prng {
    uint64_t state[2];
};

struct subr_mutex_pthread {
    pthread_mutex_t lock;
    uint64_t        cnt;
};

struct subr_mutex_rwlock {
    pthread_rwlock_t lock;
    uint64_t         cnt;
};

struct subr_mutex_sema {
    sem_t    lock;
    uint64_t cnt;
};

struct subr_ticket {
    atomic_ullong head;
    atomic_ullong tail __aligned(64);
    uint64_t      cnt;
};

struct subr_spin_cmpxchg {
    atomic_int lock;
    uint64_t   cnt;
};

struct subr_spin_pthread {
    pthread_spinlock_t lock;
    uint64_t           cnt;
};

struct subr_sema {
    sem_t sema;
};

struct subr_stack_lockfree {
    struct lfstack *lfstack;
};

struct subr_stack_mutex {
    pthread_mutex_t  lock;
    void            *head;
};

struct subr_stack_sema {
    sem_t            lock;
    void            *head;
};

struct subr_data {
    union {
        struct subr_inc              inc;
        struct subr_clock            clock;
        struct subr_tsc              tsc;
        struct subr_prng             prng;
        struct subr_ticket           ticket;
        struct subr_spin_cmpxchg     spin_cmpxchg;
        struct subr_spin_pthread     spin_pthread;
        struct subr_mutex_pthread    mutex_pthread;
        struct subr_mutex_rwlock     mutex_rwlock;
        struct subr_mutex_sema       mutex_sema;
        struct subr_sema             sema;
        struct subr_stack_lockfree   stack_lockfree;
        struct subr_stack_mutex      stack_mutex;
        struct subr_stack_sema       stack_sema;
    };

    atomic_int refcnt;
    uint       cpumax;
};

struct subr_args;
typedef uintptr_t subr_func(struct subr_args *);

struct subr_stats {
    uint64_t    calls;
    uint64_t    delta;
    double      latmin;
    double      latavg;
};

struct subr_args {
    struct subr_data  *data;
    struct subr_stats  stats[2];
    subr_func         *func;
    uint64_t           seed;
    pthread_t          tid;
    size_t             cpu;
    struct subr_args  *next;
};

static inline void
cpu_pause(void)
{
#if HAVE_PAUSE
    __builtin_ia32_pause();
#else
    usleep(1);
#endif
}


extern subr_func subr_baseline;

extern subr_func subr_inc_tls;
extern subr_func subr_inc_atomic;
extern subr_func subr_inc_atomic_cst;

#if HAVE_RDTSC
extern subr_func subr_rdtsc;
#endif

#if HAVE_RDTSCP
extern subr_func subr_rdtscp;
#endif

#if HAVE_RDRAND64
extern subr_func subr_rdrand64;
#endif

#ifdef __RDPID__
extern subr_func subr_rdpid;
#endif

#if __amd64__
extern subr_func subr_cpuid;
extern subr_func subr_lsl;
extern subr_func subr_lfence;
extern subr_func subr_sfence;
extern subr_func subr_mfence;
extern subr_func subr_pause;
#endif

#if __linux
extern subr_func subr_sched_getcpu;
#endif

extern subr_func subr_xoroshiro;
extern subr_func subr_mod127;
extern subr_func subr_mod128;

extern subr_func subr_clock_real;
extern subr_func subr_clock_realfast;

extern subr_func subr_clock_mono;
extern subr_func subr_clock_monofast;

extern subr_func subr_rwlock_rdlock;
extern subr_func subr_rwlock_wrlock;
extern subr_func subr_ticket;
extern subr_func subr_spin_cmpxchg;
extern subr_func subr_spin_pthread;
extern subr_func subr_mutex_pthread;
extern subr_func subr_mutex_sema;

extern subr_func subr_sema;

extern subr_func subr_stack_lockfree;
extern subr_func subr_stack_mutex;
extern subr_func subr_stack_sema;

extern int subr_init(struct subr_args *args);
extern void subr_fini(struct subr_args *args);

#endif
