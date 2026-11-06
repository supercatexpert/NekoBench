/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Kyosuke Nekoyashiki
 */
#include "nekobench.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------- frequency probe */

/* A chain of dependent register-register adds retires at exactly one add per
 * cycle on every mainstream core, so adds/second == core clock. The empty asm
 * forces each intermediate value to exist and keeps `one` opaque (some cores
 * can fold add-immediate chains in the renamer, but not reg-reg adds). */
__attribute__((noinline)) static uint64_t add_chain(uint64_t n, uint64_t one)
{
    uint64_t x = 0;
    __asm__ volatile("" : "+r"(one));
    for (uint64_t i = 0; i < n; i++) {
#define A x += one; __asm__ volatile("" : "+r"(x) : "r"(one));
        A A A A A A A A A A A A A A A A
#undef A
    }
    return x;
}

double nb_measure_freq_ghz(int cpu)
{
    if (nb_pin_self(cpu) != 0)
        return 0;

    /* ~100 ms busy warm-up so the core reaches its sustained boost clock. */
    uint64_t t_end = nb_now_ns() + 100000000ull;
    while (nb_now_ns() < t_end)
        nb_sink += add_chain(100000, 1);

    double best = 0;
    for (int r = 0; r < 7; r++) {
        const uint64_t n = 1000000;           /* 16M adds ~= 3-5 ms */
        uint64_t t0 = nb_now_ns();
        nb_sink += add_chain(n, 1);
        uint64_t dt = nb_now_ns() - t0;
        double ghz = (double)(n * 16) / (double)dt;
        if (ghz > best)
            best = ghz;
    }
    nb_unpin_self();
    return best;
}

/* Short probe for right after a measurement, while the core is still busy
 * at the clock the measurement ran at: no warm-up, best of 3 x ~2-4 ms. */
double nb_measure_freq_quick_ghz(int cpu)
{
    if (nb_pin_self(cpu) != 0)
        return 0;
    double best = 0;
    for (int r = 0; r < 3; r++) {
        const uint64_t n = 500000;
        uint64_t t0 = nb_now_ns();
        nb_sink += add_chain(n, 1);
        uint64_t dt = nb_now_ns() - t0;
        double ghz = (double)(n * 16) / (double)dt;
        if (ghz > best)
            best = ghz;
    }
    nb_unpin_self();
    return best;
}

/* ---------------------------------------------------------- pointer chase */

__attribute__((noinline)) static void **chase(void **p, size_t n)
{
    for (size_t i = 0; i < n; i += 16) {
#define S p = (void **)*p;
        S S S S S S S S S S S S S S S S
#undef S
    }
    return p;
}

/* Link every NB_LINE-sized line of buf into one cycle that prefetchers
 * cannot predict. block == 0: one random order over the whole buffer.
 * block > 0: "block-random" -- the lines of each block are visited in random
 * order, blocks one after another (AIDA64-style; limits TLB/DRAM-page misses). */
static bool build_chain(uint8_t *buf, size_t size, size_t block)
{
    size_t n = size / NB_LINE;
    if (n < 2)
        return false;
#if SIZE_MAX > UINT32_MAX
    if (n > UINT32_MAX)     /* indices are 32-bit: max 256 GiB */
        return false;
#endif
    size_t per = block ? block / NB_LINE : n;
    if (per < 2)
        per = 2;
    uint32_t *order = malloc(n * sizeof *order);
    if (!order)
        return false;
    for (size_t i = 0; i < n; i++)
        order[i] = (uint32_t)i;
    uint64_t rng = 0x9E3779B97F4A7C15ull ^ (uint64_t)size ^ ((uint64_t)block << 17);
    for (size_t b = 0; b < n; b += per) {
        size_t m = (n - b < per) ? n - b : per;
        for (size_t i = m - 1; i > 0; i--) {      /* Fisher-Yates within the block */
            size_t j = (size_t)(nb_rand(&rng) % (i + 1));
            uint32_t t = order[b + i];
            order[b + i] = order[b + j];
            order[b + j] = t;
        }
    }
    for (size_t k = 0; k < n; k++)                 /* one cycle through all lines */
        *(void **)(buf + (size_t)order[k] * NB_LINE) = buf + (size_t)order[(k + 1) % n] * NB_LINE;
    free(order);
    return true;
}

static size_t round16(double v)
{
    size_t n = (size_t)v;
    n = (n + 15) & ~(size_t)15;
    return n ? n : 16;
}

double nb_latency_ns(int cpu, size_t size, unsigned trial_ms, int repeat)
{
    return nb_latency_block_ns(cpu, size, 0, trial_ms, repeat);
}

double nb_latency_block_ns(int cpu, size_t size, size_t block, unsigned trial_ms, int repeat)
{
    if (nb_pin_self(cpu) != 0)
        return NAN;

    double best = NAN;
    uint8_t *buf = nb_alloc(size);
    if (!buf || !build_chain(buf, size, block))
        goto out;

    void **p = (void **)buf;
    size_t lines = size / NB_LINE;

    /* Warm-up: walk the cycle once (bounded for huge buffers). */
    p = chase(p, round16((double)(lines < (1u << 20) ? lines : (1u << 20))));

    /* Calibrate the number of loads to ~trial_ms per trial. */
    size_t iters = 1 << 14;
    uint64_t dt;
    for (;;) {
        uint64_t t0 = nb_now_ns();
        p = chase(p, iters);
        dt = nb_now_ns() - t0;
        if (dt >= 10000000ull || iters >= SIZE_MAX / 4)   /* 32-bit safe cap */
            break;
        iters *= 2;
    }
    iters = round16((double)iters * (trial_ms * 1e6) / (double)dt);

    best = INFINITY;
    for (int r = 0; r < repeat; r++) {
        uint64_t t0 = nb_now_ns();
        p = chase(p, iters);
        double ns = (double)(nb_now_ns() - t0) / (double)iters;
        if (ns < best)
            best = ns;
    }
    nb_sink += (uintptr_t)p;

out:
    nb_free(buf, size);
    nb_unpin_self();
    return best;
}
