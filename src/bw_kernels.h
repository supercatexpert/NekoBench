/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Kyosuke Nekoyashiki
 */
#ifndef NB_BW_KERNELS_H
#define NB_BW_KERNELS_H

#include "nekobench.h"

/* One set of bandwidth kernels. bw_kernels.c is compiled once per SIMD
 * level (baseline, plus SSE2/AVX/AVX-512 builds on x86), each defining the
 * variant named by NB_KVAR; bandwidth.c picks one at run time. */
/* Copy kernels skew dst by this much (see bw_kernels.c); buffers need the slack. */
#define COPY_SKEW 2048

typedef uint64_t (*nb_kernel_fn)(void *, size_t, uint64_t);

enum { NB_REQ_NONE, NB_REQ_SSE2, NB_REQ_AVX, NB_REQ_AVX512 };

typedef struct {
    const char *key;            /* --simd name: sse2, avx, avx512, neon, generic */
    const char *name;           /* display name */
    int req;                    /* CPU feature needed beyond the baseline */
    nb_kernel_fn k[NB_BW_NKERNELS];
} nb_bw_variant;

extern const nb_bw_variant nb_bwk_base, nb_bwk_sse2, nb_bwk_avx, nb_bwk_avx512;

#endif
