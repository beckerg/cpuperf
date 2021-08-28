/*
 * Copyright (c) 2021 Greg Becker.  All rights reserved.
 */

#ifndef SUBR_H
#define SUBR_H

#include <stdint.h>
#include <stdatomic.h>
#include <unistd.h>
#include <time.h>
#include <semaphore.h>
#include <pthread.h>

#define HAVE_RDTSC          (__has_builtin(__builtin_ia32_rdtsc))
#define HAVE_RDTSCP         (__has_builtin(__builtin_ia32_rdtscp))
#define HAVE_PAUSE          (__has_builtin(__builtin_ia32_pause))

static inline void
cpu_pause(void)
{
#if HAVE_PAUSE
    _mm_pause();
#else
    usleep(1);
#endif
}

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

struct mutex {
    pthread_mutex_t mtx;
    uint64_t        cnt;
};

struct ticket {
    atomic_ullong head;
    atomic_ullong tail;
    uint64_t      cnt;
};

struct spin {
    atomic_int lock;
    uint64_t   cnt;
};

struct ptspin {
    pthread_spinlock_t lock;
    uint64_t           cnt;
};

struct sema {
    sem_t    sema;
    uint64_t cnt;
};

struct stack_lf {
    struct lfstack *lfstack;
};

struct stack_sl {
    atomic_int  lock;
    void       *head;
};

struct testdata {
    union {
        struct inc      inc;
        struct clock    clock;
        struct tsc      tsc;
        struct prng     prng;
        struct mutex    mutex;
        struct ticket   ticket;
        struct spin     spin;
        struct ptspin   ptspin;
        struct sema     sema;
        struct stack_lf lfstack;
        struct stack_sl slstack;
    };

    uint cpumax;
};

extern uintptr_t subr_baseline(struct testdata *data);

extern uintptr_t subr_inc_tls(struct testdata *data);
extern uintptr_t subr_inc_atomic(struct testdata *data);

#if HAVE_RDTSC
extern uintptr_t subr_rdtsc(struct testdata *data);
#endif

#if HAVE_RDTSCP
extern uintptr_t subr_rdtscp(struct testdata *data);
#endif

#ifdef __RDPID__
extern uintptr_t subr_rdpid(struct testdata *data);
#endif

#if __amd64__
extern uintptr_t subr_cpuid(struct testdata *data);
extern uintptr_t subr_lsl(struct testdata *data);
#endif

#if __linux
extern uintptr_t subr_sched_getcpu(struct testdata *data);
#endif

extern int subr_xoroshiro_init(struct testdata *data);
extern uintptr_t subr_xoroshiro(struct testdata *data);
extern uintptr_t subr_mod127(struct testdata *data);
extern uintptr_t subr_mod128(struct testdata *data);

extern uintptr_t subr_clock(struct testdata *data);

extern uintptr_t subr_ticket(struct testdata *data);
extern uintptr_t subr_spin(struct testdata *data);

extern int subr_ptspin_init(struct testdata *data);
extern uintptr_t subr_ptspin(struct testdata *data);

extern int subr_mutex_init(struct testdata *data);
extern uintptr_t subr_mutex(struct testdata *data);

extern int subr_sem_init(struct testdata *data);
extern uintptr_t subr_sem(struct testdata *data);

extern int subr_lfstack_init(struct testdata *data);
extern uintptr_t subr_lfstack(struct testdata *data);

extern int subr_slstack_init(struct testdata *data);
extern uintptr_t subr_slstack(struct testdata *data);

#endif
