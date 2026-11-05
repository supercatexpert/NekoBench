#include "nekobench.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

/* Core-to-core latency via a cache-line ping-pong.
 *
 * The pinger on CPU a writes an odd sequence number; the ponger on CPU b
 * spins until it sees it and answers with the next even number. Every round
 * trip moves the line a->b and b->a, so one-way latency = RTT / 2. No PAUSE
 * in the spin loops: on recent Intel cores PAUSE costs ~100+ cycles and would
 * dominate the measurement. */

typedef struct {
    _Alignas(128) _Atomic uint64_t flag;   /* own 128 B block: no false sharing, */
    _Alignas(128) _Atomic int ready;       /* no adjacent-line prefetch pairing */
    int cpu_b;
    uint64_t total;
} c2c_ctx;

static void *ponger(void *arg)
{
    c2c_ctx *c = arg;
    if (nb_pin_self(c->cpu_b) != 0) {
        atomic_store(&c->ready, -1);
        return NULL;
    }
    atomic_store(&c->ready, 1);
    for (uint64_t k = 0; k < c->total; k++) {
        uint64_t want = 2 * k + 1;
        while (atomic_load_explicit(&c->flag, memory_order_acquire) != want)
            ;
        atomic_store_explicit(&c->flag, want + 1, memory_order_release);
    }
    return NULL;
}

static int cmp_dbl(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int nb_c2c_pair(int a, int b, int samples, int iters, double *median_ns, double *min_ns)
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
    for (int i = 0; i < warm; i++, seq++) {
        atomic_store_explicit(&c->flag, 2 * seq + 1, memory_order_release);
        while (atomic_load_explicit(&c->flag, memory_order_acquire) != 2 * seq + 2)
            ;
    }
    for (int s = 0; s < samples; s++) {
        uint64_t t0 = nb_now_ns();
        for (int i = 0; i < iters; i++, seq++) {
            atomic_store_explicit(&c->flag, 2 * seq + 1, memory_order_release);
            while (atomic_load_explicit(&c->flag, memory_order_acquire) != 2 * seq + 2)
                ;
        }
        res[s] = (double)(nb_now_ns() - t0) / (double)iters / 2.0;
    }
    pthread_join(th, NULL);

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
