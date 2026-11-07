/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Kyosuke Nekoyashiki
 */
#include "nekobench.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bw_kernels.h"

/* ---------------------------------------------------------- dispatch */

/* The rest of the program is built for the baseline ISA; the kernels come
 * in several builds and the widest one this CPU (and OS) supports is used.
 * __builtin_cpu_supports also checks XCR0, i.e. that the kernel saves the
 * AVX / AVX-512 register state. */
static const nb_bw_variant *const variants[] = {
#ifdef NB_BW_HAVE_AVX512
    &nb_bwk_avx512,
#endif
#ifdef NB_BW_HAVE_AVX
    &nb_bwk_avx,
#endif
#ifdef NB_BW_HAVE_SSE2
    &nb_bwk_sse2,
#endif
    &nb_bwk_base,
};
#define NVARIANTS (sizeof variants / sizeof variants[0])

static const nb_bw_variant *active;

static bool variant_ok(const nb_bw_variant *v)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_cpu_init();
    switch (v->req) {
    case NB_REQ_SSE2:   return __builtin_cpu_supports("sse2");
    case NB_REQ_AVX:    return __builtin_cpu_supports("avx");
    case NB_REQ_AVX512: return __builtin_cpu_supports("avx512f");
    }
#endif
    return v->req == NB_REQ_NONE;
}

int nb_bw_select(const char *key)
{
    for (size_t i = 0; i < NVARIANTS; i++)
        if (variant_ok(variants[i]) && (!key || !strcmp(key, variants[i]->key))) {
            active = variants[i];
            return 0;
        }
    fprintf(stderr, "invalid --simd '%s'; this CPU and build support:", key ? key : "");
    const char *last = "";
    for (size_t i = 0; i < NVARIANTS; i++)
        if (variant_ok(variants[i]) && strcmp(last, variants[i]->key)) {
            fprintf(stderr, " %s", variants[i]->key);
            last = variants[i]->key;
        }
    fprintf(stderr, "\n");
    return -1;
}

static const nb_bw_variant *cur(void)
{
    if (!active)
        nb_bw_select(NULL);
    return active;
}

const char *nb_bw_simd_name(void) { return cur()->name; }

const char *const nb_bw_names[NB_BW_NKERNELS] = { "Read", "Write", "Copy", "NT-Write", "NT-Copy" };

bool nb_bw_kernel_available(int k) { return k >= 0 && k < NB_BW_NKERNELS && cur()->k[k]; }

/* ---------------------------------------------------------- thread group */

typedef struct {
    pthread_barrier_t bar;
    atomic_int stop;
    int kernel;
    int quit;
} bw_ctx;

typedef struct {
    bw_ctx *ctx;
    pthread_t th;
    int cpu;
    size_t size, alloc;
    uint8_t *buf;
    uint64_t bytes, ns, sink;
    int err;
} bw_worker;

static void *bw_thread(void *arg)
{
    bw_worker *w = arg;
    bw_ctx *c = w->ctx;

    if (nb_pin_self(w->cpu) != 0) {
        w->err = 1;
    } else {
        /* Allocate after pinning so first-touch places memory locally. */
        w->alloc = w->size + COPY_SKEW + 4096;
        w->buf = nb_alloc(w->alloc);
        if (!w->buf)
            w->err = 2;
        else
            memset(w->buf, 0x5a, w->alloc);
    }
    pthread_barrier_wait(&c->bar);          /* ready */

    for (;;) {
        pthread_barrier_wait(&c->bar);      /* start */
        if (c->quit)
            break;
        nb_kernel_fn fn = cur()->k[c->kernel];
        uint64_t passes = 0, acc = 0;
        uint64_t t0 = nb_now_ns();
        do {
            acc += fn(w->buf, w->size, passes);
            passes++;
        } while (!atomic_load_explicit(&c->stop, memory_order_relaxed));
        w->ns = nb_now_ns() - t0;
        w->bytes = passes * w->size;
        w->sink += acc;
        pthread_barrier_wait(&c->bar);      /* end */
    }

    nb_free(w->buf, w->alloc);
    return NULL;
}

int nb_bw_run(int n, const int *cpus, const size_t *sizes, unsigned kernel_mask,
              unsigned trial_ms, int repeat, double out[NB_BW_NKERNELS])
{
    for (int k = 0; k < NB_BW_NKERNELS; k++)
        out[k] = -1;

    bw_ctx ctx = { .quit = 0 };
    atomic_init(&ctx.stop, 0);
    bw_worker *w = calloc((size_t)n, sizeof *w);
    if (!w)
        return -1;
    pthread_barrier_init(&ctx.bar, NULL, (unsigned)n + 1);

    int started = 0;
    for (int i = 0; i < n; i++) {
        w[i].ctx = &ctx;
        w[i].cpu = cpus[i];
        w[i].size = sizes[i] / 1024 * 1024;
        if (w[i].size < 1024)
            w[i].size = 1024;
        if (pthread_create(&w[i].th, NULL, bw_thread, &w[i]) != 0)
            break;
        started++;
    }
    if (started != n) {
        /* Can't satisfy the barrier count; this only happens on resource
         * exhaustion, so bail out hard rather than deadlock. */
        fprintf(stderr, "nekobench: failed to create bandwidth threads\n");
        exit(1);
    }

    pthread_barrier_wait(&ctx.bar);         /* ready */
    int err = 0;
    for (int i = 0; i < n; i++)
        if (w[i].err) {
            fprintf(stderr, "nekobench: worker on CPU %d failed (%s)\n", w[i].cpu,
                    w[i].err == 1 ? "cannot pin" : "out of memory");
            err = -1;
        }

    for (int k = 0; k < NB_BW_NKERNELS && !err; k++) {
        if (!(kernel_mask & (1u << k)) || !cur()->k[k])
            continue;
        double best = 0;
        for (int t = 0; t <= repeat; t++) {    /* trial 0 is warm-up */
            atomic_store(&ctx.stop, 0);
            ctx.kernel = k;
            pthread_barrier_wait(&ctx.bar); /* start */
            nb_sleep_ms(t == 0 ? (trial_ms + 1) / 2 : trial_ms);
            atomic_store(&ctx.stop, 1);
            pthread_barrier_wait(&ctx.bar); /* end */
            double gbs = 0;                  /* bytes/ns == GB/s */
            for (int i = 0; i < n; i++)
                gbs += (double)w[i].bytes / (double)w[i].ns;
            if (t > 0 && gbs > best)
                best = gbs;
        }
        out[k] = best;
    }

    ctx.quit = 1;
    pthread_barrier_wait(&ctx.bar);         /* release workers */
    for (int i = 0; i < n; i++) {
        pthread_join(w[i].th, NULL);
        nb_sink += w[i].sink;
    }
    pthread_barrier_destroy(&ctx.bar);
    free(w);
    return err;
}
