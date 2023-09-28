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

#if __amd64__
#if __has_include(<immintrin.h>)
#include <immintrin.h>
#include <x86intrin.h>
#endif
#endif

#define HAVE_RDTSC          (__has_builtin(__builtin_ia32_rdtsc))
#define HAVE_RDTSCP         (__has_builtin(__builtin_ia32_rdtscp))
#define HAVE_RDRAND64       (__has_builtin(__builtin_ia32_rdrand64_step))
#define HAVE_PAUSE          (__has_builtin(__builtin_ia32_pause))

#ifndef __aligned
#define __aligned(_size)    __attribute__((__aligned__(_size)))
#endif

#ifndef rounddown
#define rounddown(x, y)     (((x)/(y))*(y))
#endif


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

struct subr_pthread_mutex {
    pthread_mutex_t lock;
    uint64_t        cnt;
};

struct subr_binsema_mutex {
    sem_t    lock;
    uint64_t cnt;
};

struct subr_pthread_rwlock {
    pthread_rwlock_t lock;
    uint64_t         cnt;
};

struct subr_ticket_spin {
    atomic_ullong head;
    uint64_t      cnt __aligned(64);
    atomic_ullong tail;
};

struct subr_cmpxchg_spin {
    atomic_int lock;
    uint64_t   cnt;
};

struct subr_pthread_spin {
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

struct subr_stack_wrlock {
    pthread_rwlock_t  lock;
    void             *head;
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
        struct subr_ticket_spin      ticket_spin;
        struct subr_cmpxchg_spin     cmpxchg_spin;
        struct subr_pthread_spin     pthread_spin;
        struct subr_pthread_mutex    pthread_mutex;
        struct subr_pthread_rwlock   pthread_rwlock;
        struct subr_binsema_mutex    binsema_mutex;
        struct subr_sema             sema;
        struct subr_stack_lockfree   stack_lockfree;
        struct subr_stack_mutex      stack_mutex;
        struct subr_stack_wrlock     stack_wrlock;
        struct subr_stack_sema       stack_sema;
    };

    atomic_int  refcnt;
    uint        cpumax;
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
    uint64_t           options;
    uint64_t           seed;
    pthread_t          tid;
    size_t             tdnum;
    size_t             tdgrp;
    struct subr_args  *next;
};

static inline void
cpu_pause(void)
{
#if HAVE_PAUSE
    __builtin_ia32_pause();
#else
    pthread_yield();
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

extern subr_func subr_ticket_spin;
extern subr_func subr_cmpxchg_spin;
extern subr_func subr_pthread_spin;
extern subr_func subr_pthread_mutex;
extern subr_func subr_binsema_mutex;
extern subr_func subr_pthread_rwlock_wrlock;
extern subr_func subr_pthread_rwlock_rdlock;

extern subr_func subr_pthread_spin_trylock;
extern subr_func subr_pthread_mutex_trylock;

extern subr_func subr_sema;

extern subr_func subr_stack_lockfree;
extern subr_func subr_stack_mutex;
extern subr_func subr_stack_wrlock;
extern subr_func subr_stack_sema;

extern int subr_init(struct subr_args *args);
extern void subr_fini(struct subr_args *args);

#endif
