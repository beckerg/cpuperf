/*
 * Copyright (C) 2021 Greg Becker.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
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

#ifndef LFSTACK_H
#define LFSTACK_H

#include <stdint.h>
#include <stdbool.h>
#include <stdalign.h>
#include <stdatomic.h>

/* lfstack is optimized for adjacent cacheline prefetch as is commonly
 * found on server class amd64 motherboards.  This wastes quite a bit
 * of space but typically improves performance.  Set LFSTACK_ALIGN to
 * 16 for the most compact representation.
 */
#ifndef LFSTACK_ALIGN
#define LFSTACK_ALIGN    (128)
#endif

struct lfstack_node {
    struct lfstack_node  *next; // ptr to next node in list
    void                 *data; // ptr to user data
};

struct lfstack_impl {
    struct lfstack_node  *head;
    u_long                gen;
};

struct lfstack {
    alignas(LFSTACK_ALIGN) _Atomic
    struct lfstack_impl nodecache; // lock-free stack of free nodes

    alignas(LFSTACK_ALIGN) _Atomic
    struct lfstack_impl stack;     // lock-free stack of busy nodes
    atomic_ulong        nodec;     // number of nodes allocated from nodev[]
    u_long              nodemax;   // max number of nodes in nodev[]

    alignas(LFSTACK_ALIGN)
    struct lfstack_node nodev[];   // vector of embedded nodes
};

extern struct lfstack *lfstack_create(u_int nodemax);
extern struct lfstack *lfstack_init(void *buf, size_t bufsz);
extern void lfstack_destroy(struct lfstack *lfstack, void (*dtor)(void *));
extern bool lfstack_push(struct lfstack *lfstack, void *data);
extern void *lfstack_pop(struct lfstack *lfstack);

#endif
