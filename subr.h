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

struct inc {
    atomic_ullong cnt;
};

struct clock {
    struct timespec ts;
};

struct tsc {
    uint aux;
};

struct prng {
    uint64_t state[2];
};

struct mutex_pthread {
    pthread_mutex_t lock;
    uint64_t        cnt;
};

struct mutex_sema {
    sem_t    lock;
    uint64_t cnt;
};

struct ticket {
    atomic_ullong head;
    atomic_ullong tail __aligned(64);
    uint64_t      cnt;
};

struct spin_cmpxchg {
    atomic_int lock;
    uint64_t   cnt;
};

struct spin_pthread {
    pthread_spinlock_t lock;
    uint64_t           cnt;
};

struct sema {
    sem_t sema;
};

struct stack_lockfree {
    struct lfstack *lfstack;
};

struct stack_mutex {
    pthread_mutex_t  lock;
    void            *head;
};

struct stack_sema {
    sem_t            lock;
    void            *head;
};

struct testdata {
    union {
        struct inc              inc;
        struct clock            clock;
        struct tsc              tsc;
        struct prng             prng;
        struct ticket           ticket;
        struct spin_cmpxchg     spin_cmpxchg;
        struct spin_pthread     spin_pthread;
        struct mutex_pthread    mutex_pthread;
        struct mutex_sema       mutex_sema;
        struct sema             sema;
        struct stack_lockfree   lfstack;
        struct stack_mutex      stack_mutex;
        struct stack_sema       stack_sema;
    };

    atomic_int refcnt;
    uint       cpumax;
};

typedef uintptr_t subr_func(struct testdata *);

static inline void
cpu_pause(void)
{
#if HAVE_PAUSE
    _mm_pause();
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

extern subr_func subr_clock;

extern subr_func subr_ticket;
extern subr_func subr_spin_cmpxchg;
extern subr_func subr_spin_pthread;
extern subr_func subr_mutex_pthread;
extern subr_func subr_mutex_sema;

extern subr_func subr_sema;

extern subr_func subr_stack_lockfree;
extern subr_func subr_stack_mutex;
extern subr_func subr_stack_sema;

extern int  subr_init(struct testdata *data, subr_func *func);
extern void subr_fini(struct testdata *data, subr_func *func);

#endif
