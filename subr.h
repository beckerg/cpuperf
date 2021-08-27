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

union testdata {
    struct {
        atomic_ullong cnt;
    } inc;

    struct {
        struct timespec ts;
    } clock;

    struct {
        uint aux;
    } rdtsc;

    struct {
        uint64_t state[2];
    } xoroshiro;

    struct {
        pthread_mutex_t mtx;
        uint64_t        cnt;
    } mutex;

    struct {
        atomic_ullong head;
        atomic_ullong tail;
        uint64_t      cnt;
    } ticket;

    struct {
        atomic_int lock;
        uint64_t   cnt;
    } spin;

    struct {
        pthread_spinlock_t lock;
        uint64_t           cnt;
    } ptspin;

    struct {
        sem_t    sem;
        uint64_t cnt;
        int      initval;
    } sem;

    struct {
        struct lfstack *lfstack;
        int             initval;
    } lfstack;

    struct {
        atomic_int  lock;
        void       *head;
        int         initval;
    } slstack;
};

extern uintptr_t subr_baseline(union testdata *data);

extern uintptr_t subr_inc_tls(union testdata *data);
extern uintptr_t subr_inc_atomic(union testdata *data);

#if __amd64__
extern uintptr_t subr_rdtsc(union testdata *data);
extern uintptr_t subr_rdtscp(union testdata *data);
extern uintptr_t subr_cpuid(union testdata *data);
extern uintptr_t subr_lsl(union testdata *data);
#endif

#ifdef __RDPID__
extern uintptr_t subr_rdpid(union testdata *data);
#endif

#if __linux
extern uintptr_t subr_sched_getcpu(union testdata *data);
#endif

extern int subr_xoroshiro_init(union testdata *data);
extern uintptr_t subr_xoroshiro(union testdata *data);
extern uintptr_t subr_mod127(union testdata *data);
extern uintptr_t subr_mod128(union testdata *data);

extern uintptr_t subr_clock(union testdata *data);

extern uintptr_t subr_ticket(union testdata *data);
extern uintptr_t subr_spin(union testdata *data);

extern int subr_ptspin_init(union testdata *data);
extern uintptr_t subr_ptspin(union testdata *data);

extern int subr_mutex_init(union testdata *data);
extern uintptr_t subr_mutex(union testdata *data);

extern int subr_sem_init(union testdata *data);
extern uintptr_t subr_sem(union testdata *data);

extern int subr_lfstack_init(union testdata *data);
extern uintptr_t subr_lfstack(union testdata *data);

extern int subr_slstack_init(union testdata *data);
extern uintptr_t subr_slstack(union testdata *data);

#endif
