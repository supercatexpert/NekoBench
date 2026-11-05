/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Kyosuke Nekoyashiki
 */
#include "nekobench.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#if defined(__AVX512F__) || defined(__AVX__) || defined(__SSE2__)
#define NB_HAVE_NT 1
#endif
#elif defined(__aarch64__)
#define NB_HAVE_NT 1      /* STNP: non-temporal store pair */
#endif

/* Generic vector matching the native SIMD width, so GCC/Clang keep it in
 * registers (AVX-512: 64 B, AVX: 32 B, SSE/NEON/other: 16 B). */
#if defined(__AVX512F__)
#define NB_VEC_BYTES 64
#elif defined(__AVX__)
#define NB_VEC_BYTES 32
#else
#define NB_VEC_BYTES 16
#endif
typedef uint64_t nb_vec __attribute__((vector_size(NB_VEC_BYTES)));

#define BLOCK 512                       /* bytes per loop iteration */
#define VPB   (BLOCK / NB_VEC_BYTES)    /* vectors per iteration */

/* Sizes must be multiples of 1024 (copy uses two halves). The empty asm with
 * a memory clobber stops the compiler turning store loops into memset/memcpy;
 * the constant-trip inner loops are fully unrolled via pragmas. */

#if defined(__x86_64__) && (defined(__AVX512F__) || defined(__AVX__) || defined(__SSE2__))
/* On x86 the read kernel is pure loads into registers. Combining loaded data
 * with ALU ops (even one vpaddq per load) caps Golden/Raptor Cove at ~2 loads
 * per cycle instead of 3, under-reporting L1 bandwidth by ~40%. */
#if defined(__AVX512F__)
#define LD(o, r) "vmovdqa64 " #o "(%[p]), %%zmm" #r "\n\t"
#define RD_BLOCK                                                          \
    LD(0, 0) LD(64, 1) LD(128, 2) LD(192, 3) LD(256, 4) LD(320, 5)        \
    LD(384, 6) LD(448, 7)
#elif defined(__AVX__)
#define LD(o, r) "vmovdqa " #o "(%[p]), %%ymm" #r "\n\t"
#define RD_BLOCK                                                          \
    LD(0, 0) LD(32, 1) LD(64, 2) LD(96, 3) LD(128, 4) LD(160, 5)          \
    LD(192, 6) LD(224, 7) LD(256, 8) LD(288, 9) LD(320, 10) LD(352, 11)   \
    LD(384, 12) LD(416, 13) LD(448, 14) LD(480, 15)
#else
#define LD(o, r) "movdqa " #o "(%[p]), %%xmm" #r "\n\t"
#define RD_BLOCK                                                          \
    LD(0, 0) LD(16, 1) LD(32, 2) LD(48, 3) LD(64, 4) LD(80, 5)            \
    LD(96, 6) LD(112, 7) LD(128, 8) LD(144, 9) LD(160, 10) LD(176, 11)    \
    LD(192, 12) LD(208, 13) LD(224, 14) LD(240, 15)                       \
    LD(256, 0) LD(272, 1) LD(288, 2) LD(304, 3) LD(320, 4) LD(336, 5)     \
    LD(352, 6) LD(368, 7) LD(384, 8) LD(400, 9) LD(416, 10) LD(432, 11)   \
    LD(448, 12) LD(464, 13) LD(480, 14) LD(496, 15)
#endif
__attribute__((noinline)) static uint64_t k_read(void *buf, size_t bytes, uint64_t seed)
{
    const uint8_t *p = buf, *e = p + bytes;
    (void)seed;
    __asm__ volatile("1:\n\t" RD_BLOCK
                     "add $512, %[p]\n\t"
                     "cmp %[e], %[p]\n\t"
                     "jb 1b\n\t"
                     : [p] "+r"(p)
                     : [e] "r"(e)
                     : "cc", "memory", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5",
                       "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12",
                       "xmm13", "xmm14", "xmm15");
    return (uint64_t)(uintptr_t)p;
}
#elif defined(__aarch64__)
/* aarch64: pure 128-bit load pairs, no ALU work per load (see the x86 note). */
#define LDP(o, a, b) "ldp q" #a ", q" #b ", [%[p], #" #o "]\n\t"
__attribute__((noinline)) static uint64_t k_read(void *buf, size_t bytes, uint64_t seed)
{
    const uint8_t *p = buf, *e = p + bytes;
    (void)seed;
    __asm__ volatile("1:\n\t"
                     LDP(0, 16, 17) LDP(32, 18, 19) LDP(64, 20, 21) LDP(96, 22, 23)
                     LDP(128, 24, 25) LDP(160, 26, 27) LDP(192, 28, 29) LDP(224, 30, 31)
                     LDP(256, 16, 17) LDP(288, 18, 19) LDP(320, 20, 21) LDP(352, 22, 23)
                     LDP(384, 24, 25) LDP(416, 26, 27) LDP(448, 28, 29) LDP(480, 30, 31)
                     "add %[p], %[p], #512\n\t"
                     "cmp %[p], %[e]\n\t"
                     "b.lo 1b\n\t"
                     : [p] "+r"(p)
                     : [e] "r"(e)
                     /* v16-v31 are caller-saved: no prologue spills */
                     : "cc", "memory", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23",
                       "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31");
    return (uint64_t)(uintptr_t)p;
}
#else
__attribute__((noinline)) static uint64_t k_read(void *buf, size_t bytes, uint64_t seed)
{
    const nb_vec *p = buf, *e = (const nb_vec *)((const uint8_t *)buf + bytes);
    nb_vec a0 = { 0 }, a1 = { 0 }, a2 = { 0 }, a3 = { 0 };
    (void)seed;
    for (; p < e; p += VPB) {
#pragma GCC unroll 8
        for (int j = 0; j < VPB; j += 4) {
            a0 += p[j]; a1 += p[j + 1]; a2 += p[j + 2]; a3 += p[j + 3];
        }
    }
    nb_vec s = a0 + a1 + a2 + a3;
    uint64_t r = 0;
    for (size_t i = 0; i < sizeof s / sizeof s[0]; i++)
        r += s[i];
    return r;
}
#endif

__attribute__((noinline)) static uint64_t k_write(void *buf, size_t bytes, uint64_t seed)
{
    nb_vec *p = buf, *e = (nb_vec *)((uint8_t *)buf + bytes);
    nb_vec v = (nb_vec){ 0 } + seed;
    for (; p < e; p += VPB) {
#pragma GCC unroll 32
        for (int j = 0; j < VPB; j++)
            p[j] = v;
        __asm__ volatile("" ::: "memory");
    }
    return seed;
}

/* Copy from the first half of the region to the second half. dst is skewed
 * by 2 KiB so that loads and in-flight stores don't 4K-alias. */
#define COPY_SKEW 2048
__attribute__((noinline)) static uint64_t k_copy(void *buf, size_t bytes, uint64_t seed)
{
    size_t half = bytes / 2;
    const nb_vec *s = buf, *e = (const nb_vec *)((const uint8_t *)buf + half);
    nb_vec *d = (nb_vec *)((uint8_t *)buf + half + COPY_SKEW);
    for (; s < e; s += VPB, d += VPB) {
#pragma GCC unroll 32
        for (int j = 0; j < VPB; j++)
            d[j] = s[j];
        __asm__ volatile("" ::: "memory");
    }
    return seed;
}

#if defined(NB_HAVE_NT) && !defined(__aarch64__)
__attribute__((noinline)) static uint64_t k_ntwrite(void *buf, size_t bytes, uint64_t seed)
{
    uint8_t *p = buf, *e = p + bytes;
#if defined(__AVX512F__)
    __m512i v = _mm512_set1_epi64((long long)seed);
    for (; p < e; p += 512)
        for (int i = 0; i < 512; i += 64)
            _mm512_stream_si512((void *)(p + i), v);
#elif defined(__AVX__)
    __m256i v = _mm256_set1_epi64x((long long)seed);
    for (; p < e; p += 512)
        for (int i = 0; i < 512; i += 32)
            _mm256_stream_si256((__m256i *)(p + i), v);
#else
    __m128i v = _mm_set1_epi64x((long long)seed);
    for (; p < e; p += 512)
        for (int i = 0; i < 512; i += 16)
            _mm_stream_si128((__m128i *)(p + i), v);
#endif
    _mm_sfence();
    return seed;
}

/* Copy with regular loads and streaming (cache-bypassing) stores. */
__attribute__((noinline)) static uint64_t k_ntcopy(void *buf, size_t bytes, uint64_t seed)
{
    size_t half = bytes / 2;
    const uint8_t *s = buf, *e = s + half;
    uint8_t *d = (uint8_t *)buf + half + COPY_SKEW;
#if defined(__AVX512F__)
    for (; s < e; s += 512, d += 512)
        for (int i = 0; i < 512; i += 64)
            _mm512_stream_si512((void *)(d + i), _mm512_load_si512((const void *)(s + i)));
#elif defined(__AVX__)
    for (; s < e; s += 512, d += 512)
        for (int i = 0; i < 512; i += 32)
            _mm256_stream_si256((__m256i *)(d + i), _mm256_load_si256((const __m256i *)(s + i)));
#else
    for (; s < e; s += 512, d += 512)
        for (int i = 0; i < 512; i += 16)
            _mm_stream_si128((__m128i *)(d + i), _mm_load_si128((const __m128i *)(s + i)));
#endif
    _mm_sfence();
    return seed;
}
#endif

#if defined(NB_HAVE_NT) && defined(__aarch64__)
/* STNP hints that the lines won't be reused, so the core can write them
 * without reading them first (like x86 MOVNT). */
#define STNP(o) "stnp q0, q1, [%[d], #" #o "]\n\t"
__attribute__((noinline)) static uint64_t k_ntwrite(void *buf, size_t bytes, uint64_t seed)
{
    uint8_t *d = buf, *e = d + bytes;
    __asm__ volatile("dup v0.2d, %[v]\n\t"
                     "mov v1.16b, v0.16b\n\t"
                     "1:\n\t"
                     STNP(0) STNP(32) STNP(64) STNP(96) STNP(128) STNP(160) STNP(192) STNP(224)
                     STNP(256) STNP(288) STNP(320) STNP(352) STNP(384) STNP(416) STNP(448) STNP(480)
                     "add %[d], %[d], #512\n\t"
                     "cmp %[d], %[e]\n\t"
                     "b.lo 1b\n\t"
                     "dmb ishst\n\t"
                     : [d] "+r"(d)
                     : [e] "r"(e), [v] "r"(seed)
                     : "cc", "memory", "v0", "v1");
    return seed;
}

#define CPNT(o) "ldp q0, q1, [%[s], #" #o "]\n\t" "stnp q0, q1, [%[d], #" #o "]\n\t"
__attribute__((noinline)) static uint64_t k_ntcopy(void *buf, size_t bytes, uint64_t seed)
{
    size_t half = bytes / 2;
    const uint8_t *s = buf, *e = s + half;
    uint8_t *d = (uint8_t *)buf + half + COPY_SKEW;
    __asm__ volatile("1:\n\t"
                     CPNT(0) CPNT(32) CPNT(64) CPNT(96) CPNT(128) CPNT(160) CPNT(192) CPNT(224)
                     CPNT(256) CPNT(288) CPNT(320) CPNT(352) CPNT(384) CPNT(416) CPNT(448) CPNT(480)
                     "add %[s], %[s], #512\n\t"
                     "add %[d], %[d], #512\n\t"
                     "cmp %[s], %[e]\n\t"
                     "b.lo 1b\n\t"
                     "dmb ishst\n\t"
                     : [s] "+r"(s), [d] "+r"(d)
                     : [e] "r"(e)
                     : "cc", "memory", "v0", "v1");
    return seed;
}
#endif

typedef uint64_t (*kernel_fn)(void *, size_t, uint64_t);

static const kernel_fn kernels[NB_BW_NKERNELS] = {
    [NB_BW_READ] = k_read,
    [NB_BW_WRITE] = k_write,
    [NB_BW_COPY] = k_copy,
#ifdef NB_HAVE_NT
    [NB_BW_NTWRITE] = k_ntwrite,
    [NB_BW_NTCOPY] = k_ntcopy,
#endif
};

const char *const nb_bw_names[NB_BW_NKERNELS] = { "Read", "Write", "Copy", "NT-Write", "NT-Copy" };

bool nb_bw_kernel_available(int k) { return k >= 0 && k < NB_BW_NKERNELS && kernels[k]; }

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
        kernel_fn fn = kernels[c->kernel];
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
        if (!(kernel_mask & (1u << k)) || !kernels[k])
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
