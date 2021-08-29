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

#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <unistd.h>
#include <string.h>
#include <sys/param.h>

#include "lfstack.h"

/* You can make struct lfstack bigger, but c11 atomics on amd64 will
 * use locks rather than cmpxchg16b to perform atomic operations on it.
 */
_Static_assert(sizeof(struct lfstack_impl) <= 16, "perf penalty (cannot use cmpxchg16b)");


struct lfstack *
lfstack_create(u_int nodemax)
{
    struct lfstack *lfstack;
    size_t align, sz;

    sz = sizeof(*lfstack) + sizeof(struct lfstack_node) * nodemax;
    align = __alignof(*lfstack);

    lfstack = aligned_alloc(align, roundup(sz, align));

    return lfstack_init(lfstack, sz);
}

struct lfstack *
lfstack_init(void *buf, size_t bufsz)
{
    struct lfstack *lfstack = buf;
    u_long nodemax;

    if (!buf || bufsz < sizeof(*lfstack) + sizeof(struct lfstack_node))
        return NULL;

    nodemax = (bufsz - sizeof(*lfstack)) / sizeof(struct lfstack_node);

    memset(lfstack, 0, sizeof(*lfstack));
    atomic_init(&lfstack->nodec, 0);
    lfstack->nodemax = nodemax;

    return lfstack;
}

void
lfstack_destroy(struct lfstack *lfstack, void (*dtor)(void *arg))
{
    if (dtor) {
        struct lfstack_node *node;
        struct lfstack_impl head;

        head = atomic_load(&lfstack->stack);
        node = head.head;

        while (node) {
            dtor(node->data);
            node = node->next;
        }
    }

    free(lfstack);
}

bool
lfstack_push(struct lfstack *lfstack, void *data)
{
    struct lfstack_impl next, orig;
    struct lfstack_node *node;

    /* First, allocate a stack node from the node cache if it's not empty,
     * otherwise try to allocate one from the vector of embedded nodes.
     */
    orig = atomic_load(&lfstack->nodecache);

    do {
        if (!orig.head) {
            u_long n;

            n = atomic_fetch_add(&lfstack->nodec, 1);
            if (n >= lfstack->nodemax)
                return false;

            node = lfstack->nodev + n;
            goto push;
        }

        next.head = orig.head->next;
        next.gen = orig.gen + 1;
    } while (!atomic_compare_exchange_weak(&lfstack->nodecache, &orig, next));

    node = orig.head;

    /* Next, add the user-data pointer to the node then push
     * the node onto the stack.
     */
  push:
    node->data = data;

    orig = atomic_load(&lfstack->stack);

    do {
        node->next = orig.head;
        next.head = node;
        next.gen = orig.gen + 1;
    } while (!atomic_compare_exchange_weak(&lfstack->stack, &orig, next));

    return true;
}


void *
lfstack_pop(struct lfstack *lfstack)
{
    struct lfstack_impl next, orig;
    struct lfstack_node *node;
    void *data;

    /* First, try to remove a node from the top of the stack.
     */
    orig = atomic_load(&lfstack->stack);

    do {
        if (!orig.head) {
            return NULL;
        }

        next.head = orig.head->next;
        next.gen = orig.gen + 1;
    } while (!atomic_compare_exchange_weak(&lfstack->stack, &orig, next));

    /* Next, extract the user-data pointer from the node
     * then push it onto the node cache.
     */
    node = orig.head;
    data = node->data;

    orig = atomic_load(&lfstack->nodecache);

    do {
        node->next = orig.head;
        next.head = node;
        next.gen = orig.gen + 1;
    } while (!atomic_compare_exchange_weak(&lfstack->nodecache, &orig, next));

    return data;
}
