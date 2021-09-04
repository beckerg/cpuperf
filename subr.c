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

#include <stdint.h>
#include <stdatomic.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <semaphore.h>
#include <pthread.h>

#include <immintrin.h>
#include <x86intrin.h>

#if __linux__
#include <sched.h>
#endif

#include "xoroshiro.h"
#include "lfstack.h"
#include "subr.h"

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
subr_baseline(struct testdata *data __unused)
{
    return 0;
}

uintptr_t
subr_inc_tls(struct testdata *data __unused)
{
    static __thread uint64_t tls;

    return ++tls;
}

uintptr_t
subr_inc_atomic(struct testdata *data)
{
    return atomic_fetch_add_explicit(&data->inc.cnt, 1, memory_order_relaxed);
}

#if HAVE_RDTSC
uintptr_t
subr_rdtsc(struct testdata *data __unused)
{
    return _rdtsc();
}
#endif

#if HAVE_RDTSCP
uintptr_t
subr_rdtscp(struct testdata *data __unused)
{
    return __rdtscp(&data->tsc.aux);
}
#endif

#if HAVE_RDRAND64
uintptr_t
subr_rdrand64(struct testdata *data __unused)
{
    unsigned long long val;

    while (!_rdrand64_step(&val))
        continue;

    return val;
}
#endif

#ifdef __RDPID__
uintptr_t
subr_rdpid(struct testdata *data __unused)
{
    return _rdpid_u32() & 0xfff;
}
#endif

#if __amd64__
uintptr_t
subr_cpuid(struct testdata *data __unused)
{
    __asm__ volatile ("cpuid" ::: "eax","ebx","ecx","edx","memory");

    return 0;
}

uintptr_t
subr_lsl(struct testdata *data __unused)
{
    uint cpu;

    __asm__ volatile ("lsl %1,%0" : "=r" (cpu) : "r" (123));

    return cpu & 0xfff;
}

uintptr_t
subr_lfence(struct testdata *data __unused)
{
    _mm_lfence();

    return 0;
}

uintptr_t
subr_sfence(struct testdata *data __unused)
{
    _mm_sfence();

    return 0;
}

uintptr_t
subr_mfence(struct testdata *data __unused)
{
    _mm_mfence();

    return 0;
}

uintptr_t
subr_pause(struct testdata *data __unused)
{
    _mm_pause();

    return 0;
}

#endif

#if __linux__
uintptr_t
subr_sched_getcpu(struct testdata *data __unused)
{
    return sched_getcpu();
}
#endif

uintptr_t
subr_xoroshiro(struct testdata *data)
{
    return xoroshiro128plus(data->prng.state);
}

uintptr_t
subr_mod127(struct testdata *data)
{
    return xoroshiro128plus(data->prng.state) % 127;
}

uintptr_t
subr_mod128(struct testdata *data)
{
    return xoroshiro128plus(data->prng.state) % 128;
}

uintptr_t
subr_clock(struct testdata *data)
{
    clock_gettime(CLOCK_MONOTONIC, &data->clock.ts);

    return data->clock.ts.tv_sec;
}

static inline void
subr_spin_lock(atomic_int *ptr)
{
    int old = 0;

    while (!atomic_cmpxchg_acq(ptr, &old, 1)) {
        while (atomic_load_explicit(ptr, memory_order_relaxed))
            cpu_pause();
        old = 0;
    }
}

static inline void
subr_spin_unlock(atomic_int *ptr)
{
    atomic_store_explicit(ptr, 0, memory_order_release);
}

uintptr_t
subr_spin(struct testdata *data)
{
    subr_spin_lock(&data->spin.lock);
    data->spin.cnt++;
    subr_spin_unlock(&data->spin.lock);

    return 0;
}

uintptr_t
subr_ptspin(struct testdata *data)
{
    pthread_spin_lock(&data->ptspin.lock);
    data->ptspin.cnt++;
    pthread_spin_unlock(&data->ptspin.lock);

    return 0;
}


static inline void
subr_ticket_lock(struct testdata *data)
{
    uint64_t head, tail;

    head = atomic_inc(&data->ticket.head);

    while ((tail = atomic_load_acq(&data->ticket.tail)) < head) {

#if HAVE_RDTSC
        uint64_t stop = _rdtsc() + (head - tail) * 256;

        cpu_pause();

        while (_rdtsc() < stop)
            continue;
#else
        cpu_pause();
#endif
    }
}

static inline void
subr_ticket_unlock(struct testdata *data)
{
    atomic_inc_rel(&data->ticket.tail);
}

uintptr_t
subr_ticket(struct testdata *data)
{
    subr_ticket_lock(data);
    data->ticket.cnt++;
    subr_ticket_unlock(data);

    return 0;
}

uintptr_t
subr_mutex(struct testdata *data)
{
    pthread_mutex_lock(&data->mutex.mtx);
    data->mutex.cnt++;
    pthread_mutex_unlock(&data->mutex.mtx);

    return 0;
}

uintptr_t
subr_sema(struct testdata *data)
{
    while (sem_wait(&data->sema.sema))
        continue;

    //data->sema.cnt++;
    sem_post(&data->sema.sema);

    return 0;
}

/* A lock-free stack implements a fixed size stack that can push/pop
 * pointers to user data structures.  Unlike the spinlock based stack
 * (below) the lock-free stack does not require any storage within the
 * user data structure.
 */
static int
subr_lfstack_init(struct testdata *data)
{
    struct lfstack *lfstack;
    int nelem = data->cpumax;
    void *mem;
    int i;

    lfstack = lfstack_create(nelem);
    if (!lfstack)
        return ENOMEM;

    for (i = 0; i < nelem; ++i) {
        mem = aligned_alloc(128, 128);
        if (mem)
            lfstack_push(lfstack, mem);
    }

    data->lfstack.lfstack = lfstack;

    return 0;
}

uintptr_t
subr_lfstack(struct testdata *data)
{
    void *item;

    item = lfstack_pop(data->lfstack.lfstack);
    if (item) {
        *(long *)item += 1;
        lfstack_push(data->lfstack.lfstack, item);
    }

    return 0;
}

/* A spinlock based stack uses a pointer within the user's data structure
 * to link freed items onto the stack and a simple spinlock to protect
 * the head of the list.  Unlike lfstack, this stack requires storage
 * within the user's data structure for list linkage, but has no real
 * item limit.
 */
struct slnode {
    void  *next;
    long   cnt;
};

static int
subr_slstack_init(struct testdata *data)
{
    int nelem = data->cpumax;
    struct slnode *node;
    int i;

    data->slstack.lock = 0;
    data->slstack.head = NULL;

    for (i = 0; i < nelem; ++i) {
        node = aligned_alloc(128, 128);
        if (node) {
            node->cnt = 0;
            node->next = data->slstack.head;
            data->slstack.head = node;
        }
    }

    return 0;
}

static void
subr_slstack_push(struct testdata *data, struct slnode *node)
{
    subr_spin_lock(&data->slstack.lock);
    node->next = data->slstack.head;
    data->slstack.head = node;
    subr_spin_unlock(&data->slstack.lock);
}

static struct slnode *
subr_slstack_pop(struct testdata *data)
{
    struct slnode *node;

    subr_spin_lock(&data->slstack.lock);
    node = data->slstack.head;
    if (node)
        data->slstack.head = node->next;
    subr_spin_unlock(&data->slstack.lock);

    return node;
}

uintptr_t
subr_slstack(struct testdata *data)
{
    struct slnode *node;

    node = subr_slstack_pop(data);
    node->cnt++;
    subr_slstack_push(data, node);

    return 0;
}

int
subr_init(struct testdata *data, subr_func *func)
{
    if (atomic_inc(&data->refcnt) > 1)
        return 0;

    if (func == subr_xoroshiro) {
        xoroshiro128plus_init(data->prng.state, 0);
    }
    else if (func == subr_ptspin) {
        return pthread_spin_init(&data->ptspin.lock, 0);
    }
    else if (func == subr_mutex) {
        return pthread_mutex_init(&data->mutex.mtx, NULL);
    }
    else if (func == subr_sema) {
        return sem_init(&data->sema.sema, 0, data->cpumax);
    }
    else if (func == subr_lfstack) {
        return subr_lfstack_init(data);
    }
    else if (func == subr_slstack) {
        return subr_slstack_init(data);
    }

    return 0;
}

void
subr_fini(struct testdata *data, subr_func *func)
{
    if (atomic_dec(&data->refcnt) > 1)
        return;

    if (func == subr_ptspin) {
        pthread_spin_destroy(&data->ptspin.lock);
    }
    else if (func == subr_mutex) {
        pthread_mutex_destroy(&data->mutex.mtx);
    }
    else if (func == subr_sema) {
        sem_destroy(&data->sema.sema);
    }
    else if (func == subr_lfstack) {
        lfstack_destroy(data->lfstack.lfstack, free);
    }
    else if (func == subr_slstack) {
        struct slnode *node;

        while (( node = subr_slstack_pop(data) )) {
            free(node);
        }
    }
}
