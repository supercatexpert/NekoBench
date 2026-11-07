/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Kyosuke Nekoyashiki
 */
/* Bandwidth kernels. Compiled once per SIMD level with -DNB_KVAR=<symbol>
 * and NB_KREQ=<NB_REQ_*>; the code below picks its width from the
 * compiler's predefined macros (__AVX512F__, __AVX__, __SSE2__, ...). */
#include "bw_kernels.h"

#include <stdint.h>

#ifndef NB_KVAR
#define NB_KVAR nb_bwk_base
#define NB_KREQ NB_REQ_NONE
#endif

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
/* This variant may be called from baseline (SSE) code: clear the upper
 * halves so legacy-SSE code afterwards pays no AVX transition penalty.
 * (Compiler-generated AVX code gets vzeroupper automatically.) */
#if defined(__AVX__)
#define VZEROUPPER "vzeroupper\n\t"
#else
#define VZEROUPPER
#endif
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
                     VZEROUPPER
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

#if defined(__AVX512F__)
#define NB_KKEY "avx512"
#define NB_KNAME "AVX-512"
#elif defined(__AVX__)
#define NB_KKEY "avx"
#define NB_KNAME "AVX"
#elif defined(__SSE2__)
#define NB_KKEY "sse2"
#define NB_KNAME "SSE2"
#elif defined(__ARM_NEON)
#define NB_KKEY "neon"
#define NB_KNAME "NEON"
#else
#define NB_KKEY "generic"
#define NB_KNAME "generic"
#endif

const nb_bw_variant NB_KVAR = {
    .key = NB_KKEY,
    .name = NB_KNAME,
    .req = NB_KREQ,
    .k = {
        [NB_BW_READ] = k_read,
        [NB_BW_WRITE] = k_write,
        [NB_BW_COPY] = k_copy,
#ifdef NB_HAVE_NT
        [NB_BW_NTWRITE] = k_ntwrite,
        [NB_BW_NTCOPY] = k_ntcopy,
#endif
    },
};
