/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Kyosuke Nekoyashiki
 */
#include "nekobench.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

/* Core-to-core latency via a cache-line ping-pong.
 *
 * The flag counts up: the pinger on CPU a moves it 2k -> 2k+1, the ponger on
 * CPU b answers 2k+1 -> 2k+2. Every round trip moves the line a->b and b->a,
 * so one-way latency = RTT / 2.
 *
 * NB_C2C_CAS (default): both sides spin on compare-and-swap. A failed CAS
 * still takes the line exclusive, so each handoff is one ownership transfer
 * and the waiter never sits on a Shared copy. For the pinger, waiting for
 * the pong *is* the CAS of the next ping.
 *
 * NB_C2C_LOAD: the waiter spins on plain loads and answers with a plain
 * store. On recent cores this is a few ns cheaper, but the waiter keeps a
 * Shared copy, so every store needs an extra invalidation, and on older
 * cores (Core 2 / Penryn, FSB systems) the loads in flight when the line is
 * stolen cause memory-ordering machine clears and bus contention; results
 * there come out several times too high.
 *
 * No PAUSE in either loop: on recent Intel cores PAUSE costs ~100+ cycles
 * and would dominate the measurement. */

typedef struct {
    _Alignas(128) _Atomic uint64_t flag;   /* own 128 B block: no false sharing, */
    _Alignas(128) _Atomic int ready;       /* no adjacent-line prefetch pairing */
    int cpu_b;
    int method;
    uint64_t total;
} c2c_ctx;

const char *const nb_c2c_method_names[NB_C2C_NMETHODS] = { "cas", "load" };

/* Spin until flag == from, then make it from+1. */
static inline void cas_handoff(_Atomic uint64_t *flag, uint64_t from)
{
    for (;;) {
        uint64_t e = from;
        if (atomic_compare_exchange_weak_explicit(flag, &e, from + 1, memory_order_acq_rel,
                                                  memory_order_relaxed))
            return;
    }
}

static inline void load_handoff(_Atomic uint64_t *flag, uint64_t from)
{
    while (atomic_load_explicit(flag, memory_order_acquire) != from)
        ;
    atomic_store_explicit(flag, from + 1, memory_order_release);
}

static void *ponger(void *arg)
{
    c2c_ctx *c = arg;
    if (nb_pin_self(c->cpu_b) != 0) {
        atomic_store(&c->ready, -1);
        return NULL;
    }
    atomic_store(&c->ready, 1);
    if (c->method == NB_C2C_CAS)
        for (uint64_t k = 0; k < c->total; k++)
            cas_handoff(&c->flag, 2 * k + 1);
    else
        for (uint64_t k = 0; k < c->total; k++)
            load_handoff(&c->flag, 2 * k + 1);
    return NULL;
}

/* Run `count` round trips starting at sequence number *seq. */
static void pings(c2c_ctx *c, uint64_t *seq, int count)
{
    uint64_t s = *seq;
    if (c->method == NB_C2C_CAS) {
        for (int i = 0; i < count; i++, s++)
            cas_handoff(&c->flag, 2 * s);
        /* The last pong is collected by the next ping's CAS, or below. */
    } else {
        for (int i = 0; i < count; i++, s++) {
            atomic_store_explicit(&c->flag, 2 * s + 1, memory_order_release);
            while (atomic_load_explicit(&c->flag, memory_order_acquire) != 2 * s + 2)
                ;
        }
    }
    *seq = s;
}

static int cmp_dbl(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int nb_c2c_pair(int a, int b, int method, int samples, int iters, double *median_ns,
                double *min_ns)
{
    const int warm = 2000;
    c2c_ctx *c = aligned_alloc(128, sizeof *c);
    double *res = malloc((size_t)samples * sizeof *res);
    if (!c || !res) {
        free(c);
        free(res);
        return -1;
    }
    atomic_init(&c->flag, 0);
    atomic_init(&c->ready, 0);
    c->cpu_b = b;
    c->method = method;
    c->total = (uint64_t)warm + (uint64_t)samples * (uint64_t)iters;

    int rc = -1;
    if (nb_pin_self(a) != 0)
        goto out_free;

    pthread_t th;
    if (pthread_create(&th, NULL, ponger, c) != 0)
        goto out_unpin;

    int r;
    while ((r = atomic_load(&c->ready)) == 0)
        ;
    if (r < 0) {
        pthread_join(th, NULL);
        goto out_unpin;
    }

    uint64_t seq = 0;
    pings(c, &seq, warm);
    for (int s = 0; s < samples; s++) {
        uint64_t t0 = nb_now_ns();
        pings(c, &seq, iters);
        res[s] = (double)(nb_now_ns() - t0) / (double)iters / 2.0;
    }
    pthread_join(th, NULL);   /* ponger exits after the final pong */

    qsort(res, (size_t)samples, sizeof *res, cmp_dbl);
    *min_ns = res[0];
    *median_ns = (samples % 2) ? res[samples / 2]
                               : 0.5 * (res[samples / 2 - 1] + res[samples / 2]);
    rc = 0;

out_unpin:
    nb_unpin_self();
out_free:
    free(res);
    free(c);
    return rc;
}
