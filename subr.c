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
#include <stdatomic.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <semaphore.h>
#include <pthread.h>

#if __has_include(<immintrin.h>)
#include <immintrin.h>
#include <x86intrin.h>
#endif

#if __linux__
#include <sched.h>
#endif

#include "xoroshiro.h"
#include "lfstack.h"
#include "subr.h"

#ifndef NELEM
#define NELEM(_vec)         (sizeof(_vec) / sizeof((_vec)[0]))
#endif

#ifndef __unused
#define __unused            __attribute__((__unused__))
#endif

#define atomic_load_acq(_ptr) \
    atomic_load_explicit((_ptr), memory_order_acquire)

#define atomic_inc(_ptr) \
    atomic_fetch_add_explicit((_ptr), 1, memory_order_relaxed)

#define atomic_dec(_ptr) \
    atomic_fetch_add_explicit((_ptr), -1, memory_order_relaxed)

#define atomic_inc_acq(_ptr) \
    atomic_fetch_add_explicit((_ptr), 1, memory_order_acquire)

#define atomic_inc_rel(_ptr) \
    atomic_fetch_add_explicit((_ptr), 1, memory_order_release)

#define atomic_cmpxchg_acq(_ptr, _oldp, _new) \
    atomic_compare_exchange_weak_explicit((_ptr), (_oldp), (_new),      \
                                          memory_order_acquire, memory_order_relaxed)

#define atomic_cmpxchg_rel(_ptr, _oldp, _new)                        \
    atomic_compare_exchange_strong_explicit((_ptr), (_oldp), (_new), \
                                            memory_order_release, memory_order_relaxed)

uintptr_t
subr_baseline(struct subr_args *args __unused)
{
    return 0;
}

uintptr_t
subr_inc_tls(struct subr_args *args __unused)
{
    static __thread uint64_t tls;

    return ++tls;
}

uintptr_t
subr_inc_atomic(struct subr_args *args)
{
    struct subr_inc *inc = &args->data->inc;

    return atomic_fetch_add_explicit(&inc->cnt, 1, memory_order_relaxed);
}

uintptr_t
subr_inc_atomic_cst(struct subr_args *args)
{
    struct subr_inc *inc = &args->data->inc;

    return atomic_fetch_add_explicit(&inc->cnt, 1, memory_order_seq_cst);
}

#if HAVE_RDTSC
uintptr_t
subr_rdtsc(struct subr_args *args __unused)
{
    return __builtin_ia32_rdtsc();
}
#endif

#if HAVE_RDTSCP
uintptr_t
subr_rdtscp(struct subr_args *args __unused)
{
    struct subr_tsc *tsc = &args->data->tsc;

    return __builtin_ia32_rdtscp(&tsc->aux);
}
#endif

#if HAVE_RDRAND64
uintptr_t
subr_rdrand64(struct subr_args *args __unused)
{
    unsigned long long val;

    while (!__builtin_ia32_rdrand64_step(&val))
        continue;

    return val;
}
#endif

#ifdef __RDPID__
uintptr_t
subr_rdpid(struct subr_args *args __unused)
{
    return _rdpid_u32() & 0xfff;
}
#endif

#if __amd64__
uintptr_t
subr_cpuid(struct subr_args *args __unused)
{
    __asm__ volatile ("cpuid" ::: "eax","ebx","ecx","edx","memory");

    return 0;
}

uintptr_t
subr_lsl(struct subr_args *args __unused)
{
    uint cpu;

    __asm__ volatile ("lsl %1,%0" : "=r" (cpu) : "r" (123));

    return cpu & 0xfff;
}

uintptr_t
subr_lfence(struct subr_args *args __unused)
{
    _mm_lfence();

    return 0;
}

uintptr_t
subr_sfence(struct subr_args *args __unused)
{
    _mm_sfence();

    return 0;
}

uintptr_t
subr_mfence(struct subr_args *args __unused)
{
    _mm_mfence();

    return 0;
}

uintptr_t
subr_pause(struct subr_args *args __unused)
{
    _mm_pause();

    return 0;
}

#endif

#if __linux__
uintptr_t
subr_sched_getcpu(struct subr_args *args __unused)
{
    return sched_getcpu();
}
#endif

uintptr_t
subr_xoroshiro(struct subr_args *args)
{
    struct subr_prng *prng = &args->data->prng;

    return xoroshiro128plus(prng->state);
}

uintptr_t
subr_mod127(struct subr_args *args)
{
    struct subr_prng *prng = &args->data->prng;

    return xoroshiro128plus(prng->state) % 127;
}

uintptr_t
subr_mod128(struct subr_args *args)
{
    struct subr_prng *prng = &args->data->prng;

    return xoroshiro128plus(prng->state) % 128;
}

#ifdef CLOCK_REALTIME
uintptr_t
subr_clock_real(struct subr_args *args)
{
    struct subr_clock *clock = &args->data->clock;

    clock_gettime(CLOCK_REALTIME, &clock->ts);

    return (clock->ts.tv_sec * 1000000000) + clock->ts.tv_nsec;
}
#endif

#ifdef CLOCK_REALTIME_COARSE
uintptr_t
subr_clock_realfast(struct subr_args *args)
{
    struct subr_clock *clock = &args->data->clock;

    clock_gettime(CLOCK_REALTIME_COARSE, &clock->ts);

    return (clock->ts.tv_sec * 1000000000) + clock->ts.tv_nsec;
}
#endif

#ifdef CLOCK_MONOTONIC
uintptr_t
subr_clock_mono(struct subr_args *args)
{
    struct subr_clock *clock = &args->data->clock;

    clock_gettime(CLOCK_MONOTONIC, &clock->ts);

    return (clock->ts.tv_sec * 1000000000) + clock->ts.tv_nsec;
}
#endif

#ifdef CLOCK_MONOTONIC_COARSE
uintptr_t
subr_clock_monofast(struct subr_args *args)
{
    struct subr_clock *clock = &args->data->clock;

    clock_gettime(CLOCK_MONOTONIC_COARSE, &clock->ts);

    return (clock->ts.tv_sec * 1000000000) + clock->ts.tv_nsec;
}
#endif

static inline void
subr_cmpxchg_lock(atomic_int *ptr)
{
    int old = 0;

    while (!atomic_cmpxchg_acq(ptr, &old, 1)) {
        while (atomic_load_explicit(ptr, memory_order_relaxed))
            cpu_pause();
        old = 0;
    }
}

static inline void
subr_cmpxchg_unlock(atomic_int *ptr)
{
    atomic_store_explicit(ptr, 0, memory_order_release);
}

uintptr_t
subr_spin_cmpxchg(struct subr_args *args)
{
    struct subr_spin_cmpxchg *spin = &args->data->spin_cmpxchg;

    subr_cmpxchg_lock(&spin->lock);
    spin->cnt++;
    subr_cmpxchg_unlock(&spin->lock);

    return 0;
}

uintptr_t
subr_spin_pthread(struct subr_args *args)
{
    struct subr_spin_pthread *spin = &args->data->spin_pthread;

    pthread_spin_lock(&spin->lock);
    spin->cnt++;
    pthread_spin_unlock(&spin->lock);

    return 0;
}

uintptr_t
subr_mutex_pthread(struct subr_args *args)
{
    struct subr_mutex_pthread *mtx = &args->data->mutex_pthread;

    pthread_mutex_lock(&mtx->lock);
    mtx->cnt++;
    pthread_mutex_unlock(&mtx->lock);

    return 0;
}

uintptr_t
subr_mutex_sema(struct subr_args *args)
{
    struct subr_mutex_sema *mtx = &args->data->mutex_sema;

    while (sem_wait(&mtx->lock))
        continue;

    mtx->cnt++;
    sem_post(&mtx->lock);

    return 0;
}

uintptr_t
subr_rdlock_pthread(struct subr_args *args)
{
    struct subr_rwlock_pthread *mtx = &args->data->rwlock_pthread;

    pthread_rwlock_rdlock(&mtx->lock);
    mtx->cnt++;
    pthread_rwlock_unlock(&mtx->lock);

    return 0;
}

uintptr_t
subr_wrlock_pthread(struct subr_args *args)
{
    struct subr_rwlock_pthread *mtx = &args->data->rwlock_pthread;

    pthread_rwlock_wrlock(&mtx->lock);
    mtx->cnt++;
    pthread_rwlock_unlock(&mtx->lock);

    return 0;
}

static inline void
subr_ticket_lock(struct subr_ticket *ticket)
{
    uint64_t head;

    head = atomic_inc_acq(&ticket->head);

    while (atomic_load_acq(&ticket->tail) != head) {
        while (atomic_load_explicit(&ticket->tail, memory_order_relaxed) != head) {
            cpu_pause();
        }
    }
}

static inline void
subr_ticket_unlock(struct subr_ticket *ticket)
{
    atomic_inc_rel(&ticket->tail);
}

uintptr_t
subr_ticket(struct subr_args *args)
{
    struct subr_ticket *ticket = &args->data->ticket;

    subr_ticket_lock(ticket);
    ticket->cnt++;
    subr_ticket_unlock(ticket);

    return 0;
}

uintptr_t
subr_sema(struct subr_args *args)
{
    struct subr_sema *sema = &args->data->sema;

    while (sem_wait(&sema->sema))
        continue;

    sem_post(&sema->sema);

    return 0;
}

/* A lock-free stack implements a fixed size stack that can push/pop
 * pointers to user data structures.  Unlike the spinlock based stack
 * (below) the lock-free stack does not require any storage within the
 * user data structure.
 */
static int
subr_stack_lockfree_init(struct subr_args *args)
{
    struct subr_stack_lockfree *stack = &args->data->stack_lockfree;
    struct lfstack *lfstack;
    size_t nelem = args->data->cpumax;
    void *mem;

    lfstack = lfstack_create(nelem);
    if (!lfstack)
        return ENOMEM;

    for (size_t i = 0; i < nelem; ++i) {
        mem = aligned_alloc(128, 128);
        if (mem)
            lfstack_push(lfstack, mem);
    }

    stack->lfstack = lfstack;

    return 0;
}

uintptr_t
subr_stack_lockfree(struct subr_args *args)
{
    struct subr_stack_lockfree *stack = &args->data->stack_lockfree;
    void *item;

    item = lfstack_pop(stack->lfstack);
    if (item) {
        *(long *)item += 1;
        lfstack_push(stack->lfstack, item);
    }

    return 0;
}

/* A spinlock based stack uses a pointer within the user's data structure
 * to link freed items onto the stack and a simple spinlock to protect
 * the head of the list.  Unlike lfstack, this stack requires storage
 * within the user's data structure for list linkage, but is limited
 * only by the amount of available memory.
 */
struct stack_node {
    void  *next;
    long   cnt;
};

static int
subr_stack_mutex_init(struct subr_args *args)
{
    struct subr_stack_mutex *stack = &args->data->stack_mutex;
    size_t nelem = args->data->cpumax;
    struct stack_node *node;
    int rc;

    rc = pthread_mutex_init(&stack->lock, NULL);
    if (rc)
        return rc;

    stack->head = NULL;

    for (size_t i = 0; i < nelem; ++i) {
        node = aligned_alloc(128, 128);
        if (node) {
            node->cnt = 0;
            node->next = stack->head;
            stack->head = node;
        }
    }

    return 0;
}

static void
subr_stack_mutex_push(struct subr_stack_mutex *stack, struct stack_node *node)
{
    pthread_mutex_lock(&stack->lock);
    node->next = stack->head;
    stack->head = node;
    pthread_mutex_unlock(&stack->lock);
}

static struct stack_node *
subr_stack_mutex_pop(struct subr_stack_mutex *stack)
{
    struct stack_node *node;

    pthread_mutex_lock(&stack->lock);
    node = stack->head;
    if (node)
        stack->head = node->next;
    pthread_mutex_unlock(&stack->lock);

    return node;
}

uintptr_t
subr_stack_mutex(struct subr_args *args)
{
    struct subr_stack_mutex *stack = &args->data->stack_mutex;
    struct stack_node *node;

    node = subr_stack_mutex_pop(stack);
    node->cnt++;
    subr_stack_mutex_push(stack, node);

    return 0;
}

static int
subr_stack_wrlock_init(struct subr_args *args)
{
    struct subr_stack_wrlock *stack = &args->data->stack_wrlock;
    size_t nelem = args->data->cpumax;
    struct stack_node *node;
    int rc;

    rc = pthread_rwlock_init(&stack->lock, NULL);
    if (rc)
        return rc;

#if __FreeBSD__
    for (size_t i = 0; i < NELEM(stack->padv); ++i) {
        pthread_rwlock_init(&stack->padv[i], NULL);
    }
#endif

    stack->head = NULL;

    for (size_t i = 0; i < nelem; ++i) {
        node = aligned_alloc(128, 128);
        if (node) {
            node->cnt = 0;
            node->next = stack->head;
            stack->head = node;
        }
    }

    return 0;
}

static void
subr_stack_wrlock_push(struct subr_stack_wrlock *stack, struct stack_node *node)
{
    pthread_rwlock_wrlock(&stack->lock);
    node->next = stack->head;
    stack->head = node;
    pthread_rwlock_unlock(&stack->lock);
}

static struct stack_node *
subr_stack_wrlock_pop(struct subr_stack_wrlock *stack)
{
    struct stack_node *node;

    pthread_rwlock_wrlock(&stack->lock);
    node = stack->head;
    if (node)
        stack->head = node->next;
    pthread_rwlock_unlock(&stack->lock);

    return node;
}

uintptr_t
subr_stack_wrlock(struct subr_args *args)
{
    struct subr_stack_wrlock *stack = &args->data->stack_wrlock;
    struct stack_node *node;

    node = subr_stack_wrlock_pop(stack);
    node->cnt++;
    subr_stack_wrlock_push(stack, node);

    return 0;
}

static int
subr_stack_sema_init(struct subr_args *args)
{
    struct subr_stack_sema *stack = &args->data->stack_sema;
    size_t nelem = args->data->cpumax;
    struct stack_node *node;
    int rc;

    rc = sem_init(&stack->lock, 0, 1);
    if (rc)
        return rc;

    stack->head = NULL;

    for (size_t i = 0; i < nelem; ++i) {
        node = aligned_alloc(128, 128);
        if (node) {
            node->cnt = 0;
            node->next = stack->head;
            stack->head = node;
        }
    }

    return 0;
}

static void
subr_stack_sema_push(struct subr_stack_sema *stack, struct stack_node *node)
{
    while (sem_wait(&stack->lock))
        continue;
    node->next = stack->head;
    stack->head = node;
    sem_post(&stack->lock);
}

static struct stack_node *
subr_stack_sema_pop(struct subr_stack_sema *stack)
{
    struct stack_node *node;

    while (sem_wait(&stack->lock))
        continue;
    node = stack->head;
    if (node)
        stack->head = node->next;
    sem_post(&stack->lock);

    return node;
}

uintptr_t
subr_stack_sema(struct subr_args *args)
{
    struct subr_stack_sema *stack = &args->data->stack_sema;
    struct stack_node *node;

    node = subr_stack_sema_pop(stack);
    node->cnt++;
    subr_stack_sema_push(stack, node);

    return 0;
}

int
subr_init(struct subr_args *args)
{
    struct subr_data *data = args->data;
    subr_func *func = args->func;
    int rc = 0;

    if (atomic_inc(&data->refcnt) > 0)
        return 0;

    if (func == subr_xoroshiro) {
        xoroshiro128plus_init(data->prng.state, args->seed);
    }
    else if (func == subr_ticket) {
        atomic_store(&data->ticket.head, 0);
        atomic_store(&data->ticket.tail, 0);
    }
    else if (func == subr_rdlock_pthread ||
             func == subr_wrlock_pthread) {

        rc = pthread_rwlock_init(&data->rwlock_pthread.lock, NULL);

#if __FreeBSD__
        // Try to prevent false sharing...
        for (size_t i = 0; i < NELEM(data->rwlock_pthread.padv); ++i) {
            pthread_rwlock_init(&data->rwlock_pthread.padv[i], NULL);
        }
#endif
    }
    else if (func == subr_spin_cmpxchg) {
        atomic_store(&data->spin_cmpxchg.lock, 0);
    }
    else if (func == subr_spin_pthread) {
        rc = pthread_spin_init(&data->spin_pthread.lock, 0);

#if __FreeBSD__
        // Try to prevent false sharing...
        for (size_t i = 0; i < NELEM(data->rwlock_pthread.padv); ++i) {
            rc = pthread_spin_init(&data->spin_pthread.padv[i], 0);
        }
#endif
    }
    else if (func == subr_mutex_pthread) {
        rc = pthread_mutex_init(&data->mutex_pthread.lock, NULL);
    }
    else if (func == subr_mutex_sema) {
        rc = sem_init(&data->mutex_sema.lock, 0, 1);
    }
    else if (func == subr_sema) {
        rc = sem_init(&data->sema.sema, 0, data->cpumax);
    }
    else if (func == subr_stack_lockfree) {
        rc = subr_stack_lockfree_init(args);
    }
    else if (func == subr_stack_mutex) {
        rc = subr_stack_mutex_init(args);
    }
    else if (func == subr_stack_wrlock) {
        rc = subr_stack_wrlock_init(args);
    }
    else if (func == subr_stack_sema) {
        rc = subr_stack_sema_init(args);
    }

    return rc;
}

void
subr_fini(struct subr_args *args)
{
    struct subr_data *data = args->data;
    subr_func *func = args->func;

    if (atomic_dec(&data->refcnt) > 1)
        return;

    if (func == subr_rdlock_pthread ||
        func == subr_wrlock_pthread) {

#if __FreeBSD__
        for (size_t i = 0; i < NELEM(data->rwlock_pthread.padv); ++i) {
            pthread_rwlock_destroy(&data->rwlock_pthread.padv[i]);
        }
#endif

        pthread_rwlock_destroy(&data->rwlock_pthread.lock);
    }
    else if (func == subr_spin_pthread) {
#if __FreeBSD__
        for (size_t i = 0; i < NELEM(data->spin_pthread.padv); ++i) {
            pthread_spin_destroy(&data->spin_pthread.padv[i]);
        }
#endif

        pthread_spin_destroy(&data->spin_pthread.lock);
    }
    else if (func == subr_mutex_pthread) {
        pthread_mutex_destroy(&data->mutex_pthread.lock);
    }
    else if (func == subr_mutex_sema) {
        sem_destroy(&data->mutex_sema.lock);
    }
    else if (func == subr_sema) {
        sem_destroy(&data->sema.sema);
    }
    else if (func == subr_stack_lockfree) {
        lfstack_destroy(data->stack_lockfree.lfstack, free);
    }
    else if (func == subr_stack_mutex) {
        struct subr_stack_mutex *stack = &data->stack_mutex;
        struct stack_node *node;

        while (( node = subr_stack_mutex_pop(stack) )) {
            free(node);
        }

        pthread_mutex_destroy(&stack->lock);
    }
    else if (func == subr_stack_wrlock) {
        struct subr_stack_wrlock *stack = &data->stack_wrlock;
        struct stack_node *node;

        while (( node = subr_stack_wrlock_pop(stack) )) {
            free(node);
        }

#if __FreeBSD__
        for (size_t i = 0; i < NELEM(stack->padv); ++i) {
            pthread_rwlock_destroy(&stack->padv[i]);
        }
#endif

        pthread_rwlock_destroy(&stack->lock);
    }
    else if (func == subr_stack_sema) {
        struct subr_stack_sema *stack = &data->stack_sema;
        struct stack_node *node;

        while (( node = subr_stack_sema_pop(stack) )) {
            free(node);
        }

        sem_destroy(&stack->lock);
    }
}
