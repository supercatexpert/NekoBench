/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kyosuke Nekoyashiki
 */
/* In-memory results, filled by main.c and rendered by html.c. */
#ifndef NEKOBENCH_REPORT_H
#define NEKOBENCH_REPORT_H

#include "nekobench.h"

#define NB_RPT_MAX_CLASS 16
#define NB_RPT_MAX_SWEEP 96

typedef struct {
    char   name[16];     /* "L1d", "L2", ..., "RAM" */
    char   col1[32];     /* single: cache size; multi: total working set */
    char   col2[80];     /* single: working set; multi: per-thread working set */
    double lat_ns;       /* NAN if not measured */
    double lat_cyc;
    double bw[NB_BW_NKERNELS];   /* GB/s, < 0 if not measured / n.a. */
    int    threads;              /* multi-thread rows: threads that ran it */
} nb_rpt_row_t;

typedef struct {
    size_t ws;
    double lat_ns, lat_cyc, read_gbs;
} nb_rpt_pt_t;

typedef struct {
    nb_cpu_t     topo;
    double       ghz;
    int          nrows;
    nb_rpt_row_t rows[NB_MAX_CACHES + 1];
    int          npts;
    nb_rpt_pt_t  pts[NB_RPT_MAX_SWEEP];
} nb_rpt_single_t;

typedef struct {
    nb_cpu_t topo;       /* representative CPU */
    char     cpus[512];  /* all CPUs of this type */
} nb_rpt_class_t;

typedef struct {
    char   name[24];     /* "Memory", "L1 Cache", ... */
    bool   ram;
    double read, write, copy;    /* GB/s, < 0 = failed */
    double lat_ns;               /* NAN if n/a */
    double lat_cyc;
} nb_rpt_aida_t;

typedef struct {
    /* system */
    char   version[16], model[256], thp[32], compiler[160], simd[16];
    char   date[64], cmdline[1024];
    int    nallowed;
    long   nonline;
    size_t mem_size;
    bool   l4;
    bool   has_lat, has_bw;          /* columns of the single-thread tables */

    int            ncls;
    nb_rpt_class_t cls[NB_RPT_MAX_CLASS];

    int             nsingle;
    nb_rpt_single_t single[NB_RPT_MAX_CLASS];

    /* multi-thread bandwidth */
    bool         has_mt;
    char         mt_cpus[512];
    int          mt_threads;
    int          mt_nrows;
    nb_rpt_row_t mt_rows[NB_MAX_CACHES + 1];

    /* core-to-core */
    bool         has_c2c;
    char         c2c_cpulist[512];
    int          c2c_n, c2c_samples, c2c_iters;
    int         *c2c_cpus;        /* c2c_n entries */
    double      *c2c_m;           /* c2c_n x c2c_n, ns; NAN on diagonal/failure */
    signed char *c2c_cat;         /* pair category: 0 SMT, 1 same L2, 2 other */
    int          cat_cnt[3];
    double       cat_min[3], cat_avg[3], cat_max[3];

    /* AIDA64-style summary */
    bool          has_aida, aida_nt, aida_small_pages;
    char          aida_cpus[512], aida_lat_kind[16], aida_mem_per[32], aida_lat_pattern[64];
    int           aida_threads, aida_lat_cpu, aida_nrows;
    double        aida_ghz;
    nb_rpt_aida_t aida[NB_MAX_CACHES + 1];

    char          policy[96];     /* cpufreq governor / EPP */
} nb_report_t;

extern const char *const nb_c2c_cat_names[3];

/* Write a self-contained HTML report. Returns 0 on success. */
int nb_html_write(const char *path, const nb_report_t *r);

#endif
