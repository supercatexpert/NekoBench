/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Kyosuke Nekoyashiki
 */
/* NekoBench - cache/RAM bandwidth & latency and core-to-core latency. */
#include "nekobench.h"
#include "report.h"

#include <getopt.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define NB_VERSION "1.0.0"
#define MAX_CPUS   1024
#define MAX_CLASS  16

enum { T_LAT = 1, T_BW = 2, T_MT = 4, T_C2C = 8, T_SWEEP = 16, T_AIDA = 32, T_MEMLAT = 64 };
enum { OPT_AIDA = 1000, OPT_AIDA_BLOCK, OPT_AIDA_HUGE, OPT_SMALL_PAGES, OPT_C2C_METHOD,
       OPT_SIMD };

typedef struct {
    unsigned  tests;
    int       cpu;            /* -1: one representative per core class */
    cpu_set_t cpus;           /* CPUs for multi-thread bandwidth and c2c */
    size_t    mem_size;       /* 0: auto */
    size_t    sweep_max;      /* 0: = mem_size */
    unsigned  trial_ms;
    int       repeat;
    int       c2c_samples, c2c_iters, c2c_method;
    const char *html;         /* HTML report path, or NULL */
    size_t    aida_block;     /* --aida memory latency block, 0 = fully random */
    bool      aida_small_pages; /* --aida buffers on 4 KiB pages, as AIDA64 on Windows */
    bool      small_pages;      /* --small-pages: 4 KiB pages for every buffer */
} opts_t;

static nb_report_t R;         /* everything measured, for the HTML report */

/* The CPU of `set` with the highest max boost clock (lowest number on ties).
 * On parts with "favored" cores (Turbo Boost Max 3.0) that's the core that
 * reaches the rated single-core turbo. */
static int best_cpu(const cpu_set_t *set)
{
    int best = -1;
    unsigned best_mhz = 0;
    for (int c = 0; c < CPU_SETSIZE; c++) {
        if (!CPU_ISSET(c, set))
            continue;
        unsigned mhz = nb_cpu_max_mhz(c);
        if (best < 0 || mhz > best_mhz) {
            best = c;
            best_mhz = mhz;
        }
    }
    return best;
}

/* Print the measured clock against the cpufreq maximum, with a hint when the
 * core is clearly below it. Latency in cycles is unaffected (it's converted
 * with the measured clock); latency in ns and core-bound bandwidth are. */
/* With show_max, always prints the max boost clock; otherwise only the
 * warning when the measured clock is well below it. */
static void print_clock_note(int cpu, double ghz, bool show_max)
{
    unsigned max = nb_cpu_max_mhz(cpu);
    if (!max || ghz <= 0)
        return;
    bool low = ghz < 0.92 * max / 1000.0;
    if (!low && !show_max)
        return;
    printf("   max boost %.2f GHz", max / 1000.0);
    if (low)
        printf(" -- running %.0f%% below it (busy cores lower the turbo ratio;\n"
               "   also power/thermal limits, governor). Cycle counts are unaffected; ns and\n"
               "   L1/L2 GB/s scale with the clock",
               100.0 * (1.0 - ghz * 1000.0 / max));
    printf("\n");
}

/* ----------------------------------------------------------------- helpers */

static const char *level_name(const nb_cache_t *c, char *buf, size_t n)
{
    snprintf(buf, n, "L%d%s", c->level, c->type == 'D' ? "d" : "");
    return buf;
}

/* Working-set sizes that land squarely in each level. L1: half its size.
 * Level n: the geometric midpoint between everything below it (assumed
 * non-inclusive) and its own per-thread capacity -- random-access hit rates
 * drop well before a cache is "full" (replacement policy, slice hashing,
 * other tenants), so staying far from both edges gives a clean plateau. When
 * the level is barely bigger than what's below (e.g. a shared L3 split over
 * many threads) fall back to "below + half". */
/* `below` is the capacity in front of level i: the sum of all smaller levels
 * when they hold different data (exclusive / non-inclusive), or just the
 * next level down when level i is inclusive (it already holds that data).
 * Close levels get the midpoint, so an inclusive cache is never overrun. */
static void level_targets(const double *caps, const nb_cache_t *c, int n, size_t *ws)
{
    double below = 0;
    for (int i = 0; i < n; i++) {
        double t;
        if (i > 0 && c[i].inclusive)
            below = caps[i - 1];
        if (i == 0)
            t = caps[0] / 2;
        else if (caps[i] >= 4 * below)
            t = sqrt(below * caps[i]);
        else if (c[i].inclusive)
            t = (below + caps[i]) / 2;
        else
            t = below + caps[i] / 2;
        size_t s = (size_t)t / 4096 * 4096;
        ws[i] = s < 4096 ? 4096 : s;
        below += caps[i];
    }
}

static void print_bw_cells(const double *bw, unsigned tests, bool ram)
{
    if (!(tests & (T_BW | T_MT)))
        return;
    for (int k = 0; k < NB_BW_NKERNELS; k++) {
        if (k == NB_BW_NTCOPY || (k == NB_BW_NTWRITE && !nb_bw_kernel_available(k)))
            continue;
        if (bw[k] < 0 || (k == NB_BW_NTWRITE && !ram))
            printf(" %9s", "-");
        else
            printf(" %9.1f", bw[k]);
    }
}

static void print_bw_header(void)
{
    for (int k = 0; k < NB_BW_NKERNELS; k++)
        if (k != NB_BW_NTCOPY && (k != NB_BW_NTWRITE || nb_bw_kernel_available(k)))
            printf(" %9s", nb_bw_names[k]);
}

static unsigned bw_mask(bool ram)
{
    unsigned m = (1u << NB_BW_READ) | (1u << NB_BW_WRITE) | (1u << NB_BW_COPY);
    if (ram && nb_bw_kernel_available(NB_BW_NTWRITE))
        m |= 1u << NB_BW_NTWRITE;
    return m;
}

/* ------------------------------------------------------------ single-thread */

static void run_sweep(const opts_t *o, int cpu, double ghz)
{
    size_t max = o->sweep_max ? o->sweep_max : o->mem_size;
    printf("\n   Sweep on CPU %d (random-chase latency and single-thread read bandwidth)\n", cpu);
    printf("   %-12s %10s %8s %7s %11s\n", "Working set", "Lat (ns)", "cycles", "GHz", "Read GB/s");
    for (size_t p = 4 * NB_KiB; p <= max; p *= 2) {
        size_t pts[2] = { p, p + p / 2 };
        for (int i = 0; i < 2; i++) {
            if (pts[i] > max)
                break;
            char sz[32];
            nb_fmt_size(pts[i], sz, sizeof sz);
            double lat = nb_latency_ns(cpu, pts[i], o->trial_ms / 4 + 1, o->repeat);
            /* The clock can drift during a long sweep (governor, power
             * capping); cycles use the clock measured right after. */
            double g = nb_measure_freq_quick_ghz(cpu);
            if (g <= 0)
                g = ghz;
            double bw[NB_BW_NKERNELS];
            int c = cpu;
            size_t s = pts[i];
            nb_bw_run(1, &c, &s, 1u << NB_BW_READ, o->trial_ms / 2 + 1, o->repeat, bw);
            printf("   %-12s %10.2f %8.1f %7.2f %11.1f\n", sz, lat, lat * g, g, bw[NB_BW_READ]);
            nb_rpt_single_t *rs = &R.single[R.nsingle - 1];
            if (rs->npts < NB_RPT_MAX_SWEEP)
                rs->pts[rs->npts++] = (nb_rpt_pt_t){ pts[i], lat, lat * g, bw[NB_BW_READ] };
        }
    }
}

static void run_single(const opts_t *o, const nb_cpu_t *t)
{
    double ghz = nb_measure_freq_ghz(t->cpu);
    printf("\n== Single-thread: CPU %d (%s), measured clock %.2f GHz ==\n",
           t->cpu, t->kind, ghz);
    print_clock_note(t->cpu, ghz, true);
    if (R.nsingle >= NB_RPT_MAX_CLASS)
        R.nsingle = NB_RPT_MAX_CLASS - 1;   /* (never in practice) reuse last slot */
    nb_rpt_single_t *rs = &R.single[R.nsingle++];
    memset(rs, 0, sizeof *rs);
    rs->topo = *t;
    rs->ghz = ghz;

    double caps[NB_MAX_CACHES];
    size_t ws[NB_MAX_CACHES];
    for (int i = 0; i < t->ncache; i++)
        caps[i] = (double)t->cache[i].size;
    level_targets(caps, t->cache, t->ncache, ws);

    printf("   %-6s %-10s %-12s", "Level", "Size", "Working set");
    if (o->tests & T_LAT)
        printf(" %10s %8s", "Lat (ns)", "cycles");
    if (o->tests & T_BW) {
        printf("  |");
        print_bw_header();
        printf("  (GB/s)");
    }
    printf("\n");

    for (int i = 0; i <= t->ncache; i++) {
        bool ram = (i == t->ncache);
        char name[16], size[32], wss[32];
        size_t w = ram ? o->mem_size : ws[i];
        if (ram) {
            snprintf(name, sizeof name, "RAM");
            snprintf(size, sizeof size, "-");
        } else {
            level_name(&t->cache[i], name, sizeof name);
            nb_fmt_size(t->cache[i].size, size, sizeof size);
        }
        nb_fmt_size(w, wss, sizeof wss);
        printf("   %-6s %-10s %-12s", name, size, wss);
        fflush(stdout);
        nb_rpt_row_t *row = &rs->rows[rs->nrows++];
        snprintf(row->name, sizeof row->name, "%s", name);
        snprintf(row->col1, sizeof row->col1, "%s", size);
        snprintf(row->col2, sizeof row->col2, "%s", wss);
        row->lat_ns = row->lat_cyc = NAN;
        for (int k = 0; k < NB_BW_NKERNELS; k++)
            row->bw[k] = -1;
        if (o->tests & T_LAT) {
            double lat = nb_latency_ns(t->cpu, w, o->trial_ms / 2 + 1, o->repeat);
            double g = nb_measure_freq_quick_ghz(t->cpu);   /* clock may have drifted */
            if (g <= 0)
                g = ghz;
            printf(" %10.2f %8.1f", lat, lat * g);
            fflush(stdout);
            row->lat_ns = lat;
            row->lat_cyc = lat * g;
        }
        if (o->tests & T_BW) {
            double bw[NB_BW_NKERNELS];
            int cpu = t->cpu;
            nb_bw_run(1, &cpu, &w, bw_mask(ram), o->trial_ms, o->repeat, bw);
            printf("  |");
            print_bw_cells(bw, T_BW, ram);
            memcpy(row->bw, bw, sizeof bw);
        }
        printf("\n");
    }

    if (o->tests & T_SWEEP)
        run_sweep(o, t->cpu, ghz);
}

/* ------------------------------------------------------------ multi-thread */

/* One row of a multi-thread run: which CPUs take part and each one's
 * working set. */
typedef struct {
    char    name[16];   /* "L1d", "L0d/L1d", ..., "RAM" */
    int     level;      /* cache level of the row (first-level row: the lowest), -1 = RAM */
    bool    ram;
    int     nr;
    int    *cpus;
    size_t *sizes;
    size_t  total, lo, hi;
} plan_row_t;

/* Plan per-level working sets for threads on `cpus`. Each thread's share of
 * a cache is its size divided by the run's threads sharing it. Row keys: the
 * first-level D-cache of every CPU (grouped by position: Intel reuses "L1"
 * for different positions on hybrid parts), then higher levels by number,
 * then RAM. A cache row includes only CPUs that have the level (LP E-cores
 * without L3 sit out the L3 row). AIDA64 groups the same way: on Panther
 * Lake its "L0" row covers P-core L0s plus E-core L1ds, and "L1" only the
 * P-cores' 192 KiB L1. */
static int mt_plan(const cpu_set_t *set, const int *cpus, int n, size_t ram_per_thread,
                   plan_row_t *rows, int max)
{
    nb_cpu_t *topo = calloc((size_t)n, sizeof *topo);
    size_t (*ws)[NB_MAX_CACHES] = calloc((size_t)n, sizeof *ws);
    if (!topo || !ws) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    int maxlvl = 0;
    for (int i = 0; i < n; i++) {
        nb_topo_cpu(cpus[i], &topo[i]);
        double caps[NB_MAX_CACHES];
        for (int j = 0; j < topo[i].ncache; j++) {
            cpu_set_t both;
            CPU_AND(&both, &topo[i].cache[j].shared, set);
            int sharers = CPU_COUNT(&both);
            caps[j] = (double)topo[i].cache[j].size / (sharers > 0 ? sharers : 1);
            if (topo[i].cache[j].level > maxlvl)
                maxlvl = topo[i].cache[j].level;
        }
        level_targets(caps, topo[i].cache, topo[i].ncache, ws[i]);
    }

    int nrows = 0;
    for (int key = -1; key <= maxlvl + 1 && nrows < max; key++) {
        plan_row_t *r = &rows[nrows];
        memset(r, 0, sizeof *r);
        r->ram = (key == maxlvl + 1);
        r->level = r->ram ? -1 : 99;
        r->lo = SIZE_MAX;
        r->cpus = malloc((size_t)n * sizeof *r->cpus);
        r->sizes = malloc((size_t)n * sizeof *r->sizes);
        if (!r->cpus || !r->sizes) {
            fprintf(stderr, "out of memory\n");
            exit(1);
        }
        for (int i = 0; i < n; i++) {
            size_t sz = ram_per_thread;
            if (!r->ram) {
                int pick = -1;
                if (key == -1)
                    pick = topo[i].ncache > 0 ? 0 : -1;
                else
                    for (int j = 1; j < topo[i].ncache; j++)
                        if (topo[i].cache[j].level == key)
                            pick = j;
                if (pick < 0)
                    continue;
                sz = ws[i][pick];
                if (topo[i].cache[pick].level < r->level)
                    r->level = topo[i].cache[pick].level;
                char nm[16];
                level_name(&topo[i].cache[pick], nm, sizeof nm);
                if (!r->name[0])
                    snprintf(r->name, sizeof r->name, "%s", nm);
                else if (!strstr(r->name, nm) && strlen(r->name) + strlen(nm) + 2 < sizeof r->name)
                    strcat(strcat(r->name, "/"), nm);   /* e.g. "L0d/L1d" */
            }
            r->cpus[r->nr] = cpus[i];
            r->sizes[r->nr++] = sz;
            r->total += sz;
            if (sz < r->lo) r->lo = sz;
            if (sz > r->hi) r->hi = sz;
        }
        if (r->ram)
            snprintf(r->name, sizeof r->name, "RAM");
        if (!r->nr) {
            free(r->cpus);
            free(r->sizes);
            continue;   /* no CPU has this level */
        }
        nrows++;
    }
    free(topo);
    free(ws);
    return nrows;
}

static void plan_free(plan_row_t *rows, int n)
{
    for (int i = 0; i < n; i++) {
        free(rows[i].cpus);
        free(rows[i].sizes);
    }
}

static size_t ram_share(const opts_t *o, int n)
{
    size_t sz = o->mem_size / (size_t)n / 4096 * 4096;
    return sz < NB_MiB ? NB_MiB : sz;
}

static void run_mt(const opts_t *o, const int *cpus, int n)
{
    char list[512];
    nb_fmt_cpulist(&o->cpus, list, sizeof list);
    printf("\n== Multi-thread aggregate bandwidth: %d threads on CPUs %s ==\n", n, list);
    printf("   (each thread streams its own buffer, sized to its share of each cache level)\n");
    printf("   %-8s %7s %-12s %-22s  |", "Level", "Threads", "Total WS", "Per-thread WS");
    print_bw_header();
    printf("  (GB/s)\n");
    R.has_mt = true;
    R.mt_threads = n;
    snprintf(R.mt_cpus, sizeof R.mt_cpus, "%s", list);

    plan_row_t rows[NB_MAX_CACHES + 2];
    int nrows = mt_plan(&o->cpus, cpus, n, ram_share(o, n), rows, NB_MAX_CACHES + 1);
    for (int ri = 0; ri < nrows; ri++) {
        plan_row_t *pr = &rows[ri];
        char tot[32], a[32], b[32], per[80];
        nb_fmt_size(pr->total, tot, sizeof tot);
        nb_fmt_size(pr->lo, a, sizeof a);
        nb_fmt_size(pr->hi, b, sizeof b);
        if (pr->lo == pr->hi)
            snprintf(per, sizeof per, "%s", a);
        else
            snprintf(per, sizeof per, "%s .. %s", a, b);
        printf("   %-8s %7d %-12s %-22s  |", pr->name, pr->nr, tot, per);
        fflush(stdout);

        double bw[NB_BW_NKERNELS];
        nb_rpt_row_t *row = &R.mt_rows[R.mt_nrows < NB_MAX_CACHES ? R.mt_nrows++ : R.mt_nrows];
        _Static_assert(sizeof row->name == sizeof pr->name, "name sizes");
        memcpy(row->name, pr->name, sizeof row->name);
        snprintf(row->col1, sizeof row->col1, "%s", tot);
        snprintf(row->col2, sizeof row->col2, "%s", per);
        row->lat_ns = row->lat_cyc = NAN;
        row->threads = pr->nr;
        if (nb_bw_run(pr->nr, pr->cpus, pr->sizes, bw_mask(pr->ram), o->trial_ms, o->repeat, bw) != 0) {
            printf(" failed");
            for (int k = 0; k < NB_BW_NKERNELS; k++)
                bw[k] = -1;
        } else {
            print_bw_cells(bw, T_MT, pr->ram);
        }
        memcpy(row->bw, bw, sizeof bw);
        printf("\n");
    }
    plan_free(rows, nrows);
}

/* ------------------------------------------------------------- AIDA64 mode */

/* An AIDA64 "Cache & Memory Benchmark"-style grid, for side-by-side
 * comparison. What AIDA64 documents (developer posts on its forum):
 * read/write/copy are multi-threaded over all cores; copy is scored as
 * bytes read + bytes written (64 MB / time for a 32 MB -> 32 MB copy);
 * latency walks "block-random" addresses. Choices that are NekoBench's own
 * (not documented by AIDA64) are listed in the printed notes. */
#define AIDA_MEM_PER_THREAD (64 * NB_MiB)   /* each thread: 32 MB -> 32 MB copy */

static void run_aida(const opts_t *o, const int *cpus, int n, int lat_cpu, bool mem_set)
{
    char list[512];
    nb_fmt_cpulist(&o->cpus, list, sizeof list);
    size_t per = mem_set ? ram_share(o, n) : AIDA_MEM_PER_THREAD;
    bool nt = nb_bw_kernel_available(NB_BW_NTWRITE) && nb_bw_kernel_available(NB_BW_NTCOPY);

    plan_row_t rows[NB_MAX_CACHES + 2];
    int nrows = mt_plan(&o->cpus, cpus, n, per, rows, NB_MAX_CACHES + 1);

    nb_cpu_t lt;
    nb_topo_cpu(lat_cpu, &lt);
    double ghz = 0;
    double caps[NB_MAX_CACHES];
    size_t lws[NB_MAX_CACHES];
    for (int i = 0; i < lt.ncache; i++)
        caps[i] = (double)lt.cache[i].size;
    level_targets(caps, lt.cache, lt.ncache, lws);
    /* AIDA64's latency tests reach well into each cache: on a Xeon X7460
     * its L2 latency (22.4 cycles) matches a ~1.9 MiB chase of the 3 MiB L2
     * on 4 KiB pages (sweep with per-point clock), not our 312 KiB target,
     * and the 358H's L2 points the same way. Use at least 60% of L2. Only
     * L2: 60% of a large L3 (e.g. 20 MiB of a 14700K's 33 MiB) falls off the
     * cache, and the first levels already match. */
    for (int i = 1; i < lt.ncache; i++) {
        if (lt.cache[i].level != 2)
            continue;
        size_t frac = lt.cache[i].size / 10 * 6 / 4096 * 4096;
        if (lws[i] < frac)
            lws[i] = frac;
    }
    /* AIDA64 runs on ordinary 4 KiB pages (no large-page privilege). */
    nb_huge_pages = !(o->small_pages || o->aida_small_pages);

    printf("\n== AIDA64-style Cache & Memory Benchmark ==\n");
    printf("   bandwidth: %d threads on CPUs %s; latency: CPU %d (%s)\n\n",
           n, list, lat_cpu, lt.kind);
    printf("   %-10s %14s %14s %14s %11s\n", "", "Read", "Write", "Copy", "Latency");

    R.has_aida = true;
    R.aida_threads = n;
    R.aida_lat_cpu = lat_cpu;
    snprintf(R.aida_cpus, sizeof R.aida_cpus, "%s", list);
    snprintf(R.aida_lat_kind, sizeof R.aida_lat_kind, "%s", lt.kind);
    R.aida_nrows = 0;

    /* Latency first, while the chip is cool: on laptops the all-core
     * bandwidth runs heat it and pull the single-core boost down, and
     * latency in ns scales with that clock. */
    double lat[NB_MAX_CACHES + 2];
    for (int ri = 0; ri < nrows; ri++) {
        plan_row_t *pr = &rows[ri];
        lat[ri] = NAN;
        if (pr->ram) {
            lat[ri] = nb_latency_block_ns(lat_cpu, o->mem_size, o->aida_block, o->trial_ms / 2 + 1, o->repeat);
        } else {
            for (int j = 0; j < lt.ncache; j++)
                if (lt.cache[j].level == pr->level)
                    lat[ri] = nb_latency_ns(lat_cpu, lws[j], o->trial_ms / 2 + 1, o->repeat);
        }
    }
    ghz = nb_measure_freq_ghz(lat_cpu);

    /* AIDA64 order: Memory first, then the caches. */
    for (int pass = 0; pass < 2; pass++) {
        for (int ri = 0; ri < nrows; ri++) {
            plan_row_t *pr = &rows[ri];
            if (pr->ram != (pass == 0))
                continue;
            int kr = NB_BW_READ, kw = NB_BW_WRITE, kc = NB_BW_COPY;
            if (pr->ram && nt) {
                kw = NB_BW_NTWRITE;
                kc = NB_BW_NTCOPY;
            }
            unsigned mask = (1u << kr) | (1u << kw) | (1u << kc);

            char label[24];
            if (pr->ram)
                snprintf(label, sizeof label, "Memory");
            else
                snprintf(label, sizeof label, "L%d Cache", pr->level);
            printf("   %-10s", label);
            fflush(stdout);

            /* AIDA64 documents 4 KiB pages for its memory benchmarks. */
            double bw[NB_BW_NKERNELS];
            int rc = nb_bw_run(pr->nr, pr->cpus, pr->sizes, mask, o->trial_ms, o->repeat, bw);

            double v[3] = { rc ? -1 : bw[kr], rc ? -1 : bw[kw], rc ? -1 : bw[kc] };
            for (int k = 0; k < 3; k++) {
                if (v[k] < 0)
                    printf(" %14s", "failed");
                else if (pr->ram)
                    printf(" %9.0f MB/s", v[k] * 1000);
                else
                    printf(" %9.1f GB/s", v[k]);
            }
            if (isnan(lat[ri]))
                printf(" %11s", "-");
            else
                printf(" %8.1f ns", lat[ri]);
            if (!isnan(lat[ri]) && ghz > 0)
                printf("  (%.1f cyc)", lat[ri] * ghz);
            printf("\n");

            if (R.aida_nrows < NB_MAX_CACHES + 1) {
                nb_rpt_aida_t *a = &R.aida[R.aida_nrows++];
                snprintf(a->name, sizeof a->name, "%s", label);
                a->ram = pr->ram;
                a->read = v[0];
                a->write = v[1];
                a->copy = v[2];
                a->lat_ns = lat[ri];
                a->lat_cyc = lat[ri] * ghz;
            }
        }
    }
    nb_huge_pages = !o->small_pages;
    char pers[32], wss[32], blk[64];
    nb_fmt_size(per, pers, sizeof pers);
    nb_fmt_size(o->mem_size, wss, sizeof wss);
    if (o->aida_block) {
        char b[32];
        nb_fmt_size(o->aida_block, b, sizeof b);
        snprintf(blk, sizeof blk, "block-random (%s blocks)", b);
    } else {
        snprintf(blk, sizeof blk, "fully random");
    }
    R.aida_ghz = ghz;
    snprintf(R.aida_lat_pattern, sizeof R.aida_lat_pattern, "%s", blk);
    unsigned maxmhz = nb_cpu_max_mhz(lat_cpu);
    char maxs[48] = "";
    if (maxmhz)
        snprintf(maxs, sizeof maxs, " (max boost %.2f GHz)", maxmhz / 1000.0);
    printf("\n   Latency CPU clock: %.2f GHz%s, measured right after the latency runs\n"
           "   (which go first, before the all-core bandwidth runs heat the chip). Compare\n"
           "   across tools in cycles: ns scale with the clock.\n", ghz, maxs);
    print_clock_note(lat_cpu, ghz, false);
    char lwl[160] = "";
    for (int i = 0, len = 0; i < lt.ncache && len < (int)sizeof lwl - 32; i++) {
        char name[16], sz[32];
        nb_fmt_size(lws[i], sz, sizeof sz);
        len += snprintf(lwl + len, sizeof lwl - (size_t)len, "%s%s %s", i ? ", " : "",
                        level_name(&lt.cache[i], name, sizeof name), sz);
    }
    bool small = o->small_pages || o->aida_small_pages;
    printf("   Notes: copy = bytes read + written, as AIDA64 scores it. Memory write/copy use %s.\n"
           "   All buffers on %s pages. Memory bandwidth: %s per thread. Memory latency:\n"
           "   %s chase over %s.\n"
           "   Cache latency working sets: %s.\n"
           "   MB/s and GB/s are decimal (10^6, 10^9).\n",
           nt ? "non-temporal stores" : "regular stores (no NT support in this build)",
           small ? "4 KiB" : "2 MiB (THP)", pers, blk, wss, lwl);
    for (int i = 0; i < nrows; i++)
        if (!rows[i].ram && strchr(rows[i].name, '/')) {
            printf("   The %s row is every core's first-level D-cache (P-core L0 + E-core L1d),\n"
                   "   grouped by position as AIDA64 does.\n", rows[i].name);
            break;
        }
    R.aida_small_pages = small;
    snprintf(R.aida_mem_per, sizeof R.aida_mem_per, "%s", pers);
    R.aida_nt = nt;
    plan_free(rows, nrows);
}

/* Memory latency versus chase pattern: block-random with growing blocks,
 * then fully random. Shows how much prefetchers (and DRAM page locality)
 * shorten "latency" when accesses stay inside a region, and gives the data
 * needed to match another tool's memory-latency figure. */
static void run_memlat(const opts_t *o, int cpu)
{
    char wss[32];
    nb_fmt_size(o->mem_size, wss, sizeof wss);
    printf("\n== Memory latency vs access pattern: CPU %d, %s working set ==\n", cpu, wss);
    printf("   %-24s %10s\n", "Pattern", "Lat (ns)");
    for (size_t b = 4 * NB_KiB; b <= o->mem_size; b *= 4) {
        char bs[32], name[48];
        nb_fmt_size(b, bs, sizeof bs);
        snprintf(name, sizeof name, "block-random %s", bs);
        printf("   %-24s", name);
        fflush(stdout);
        printf(" %10.1f\n", nb_latency_block_ns(cpu, o->mem_size, b, o->trial_ms / 2 + 1, o->repeat));
    }
    printf("   %-24s", "fully random");
    fflush(stdout);
    printf(" %10.1f\n", nb_latency_ns(cpu, o->mem_size, o->trial_ms / 2 + 1, o->repeat));
    double ghz = nb_measure_freq_ghz(cpu);
    printf("   clock %.2f GHz\n", ghz);
}

/* --------------------------------------------------------- core-to-core */

static bool share_first(const nb_cpu_t *a, int cpu_b)
{
    return a->ncache > 0 && CPU_ISSET(cpu_b, &a->cache[0].shared);
}

static bool share_level(const nb_cpu_t *a, int cpu_b, int level)
{
    for (int j = 0; j < a->ncache; j++)
        if (a->cache[j].level == level && CPU_ISSET(cpu_b, &a->cache[j].shared))
            return true;
    return false;
}

static void run_c2c(const opts_t *o, const int *cpus, int n)
{
    char list[512];
    nb_fmt_cpulist(&o->cpus, list, sizeof list);
    printf("\n== Core-to-core latency (one-way ns, best of %d samples x %d round trips, %s): CPUs %s ==\n",
           o->c2c_samples, o->c2c_iters, nb_c2c_method_names[o->c2c_method], list);
    if (n < 2) {
        printf("   need at least 2 CPUs\n");
        return;
    }

    double *m = malloc((size_t)n * (size_t)n * sizeof *m);
    signed char *cat_m = calloc((size_t)n * (size_t)n, 1);
    nb_cpu_t *topo = calloc((size_t)n, sizeof *topo);
    if (!m || !topo || !cat_m) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    for (int i = 0; i < n; i++)
        nb_topo_cpu(cpus[i], &topo[i]);

    bool tty = isatty(STDERR_FILENO);
    int pairs = n * (n - 1) / 2, done = 0;
    /* categories: 0 = same core (share L1), 1 = share L2, 2 = same package, 3 = other package */
    double csum[NB_C2C_NCATS] = { 0 }, cmin[NB_C2C_NCATS], cmax[NB_C2C_NCATS] = { 0 };
    int ccnt[NB_C2C_NCATS] = { 0 };
    for (int c = 0; c < NB_C2C_NCATS; c++)
        cmin[c] = INFINITY;

    for (int i = 0; i < n; i++) {
        m[i * n + i] = NAN;
        for (int j = i + 1; j < n; j++) {
            /* Best sample: each sample already averages c2c_iters round trips;
             * taking the minimum filters out samples hit by preemption. */
            double med = NAN, mn = NAN;
            if (nb_c2c_pair(cpus[i], cpus[j], o->c2c_method, o->c2c_samples, o->c2c_iters, &med, &mn) == 0)
                med = mn;
            else
                med = NAN;
            m[i * n + j] = m[j * n + i] = med;
            if (!isnan(med)) {
                int cat = share_first(&topo[i], cpus[j]) ? 0
                        : share_level(&topo[i], cpus[j], 2) ? 1
                        : topo[i].package != topo[j].package ? 3 : 2;
                cat_m[i * n + j] = cat_m[j * n + i] = (signed char)cat;
                csum[cat] += med;
                ccnt[cat]++;
                if (med < cmin[cat]) cmin[cat] = med;
                if (med > cmax[cat]) cmax[cat] = med;
            }
            if (tty && (++done % 8 == 0 || done == pairs))
                fprintf(stderr, "\r   measuring pair %d/%d...", done, pairs);
        }
    }
    if (tty)
        fprintf(stderr, "\r%40s\r", "");

    printf("\n   CPU ");
    for (int j = 0; j < n; j++)
        printf("%5d", cpus[j]);
    printf("\n");
    for (int i = 0; i < n; i++) {
        printf("  %4d ", cpus[i]);
        for (int j = 0; j < n; j++) {
            double v = m[i * n + j];
            if (isnan(v))
                printf("%5s", i == j ? "-" : "?");
            else
                printf("%5.0f", v);
        }
        printf("\n");
    }

    printf("\n   %-26s %6s %8s %8s %8s\n", "Pair type", "pairs", "min", "avg", "max");
    for (int c = 0; c < NB_C2C_NCATS; c++)
        if (ccnt[c])
            printf("   %-26s %6d %8.1f %8.1f %8.1f\n", nb_c2c_cat_names[c], ccnt[c], cmin[c],
                   csum[c] / ccnt[c], cmax[c]);

    /* Hand the matrix over to the report (freed at exit). */
    R.has_c2c = true;
    snprintf(R.c2c_cpulist, sizeof R.c2c_cpulist, "%s", list);
    R.c2c_n = n;
    R.c2c_samples = o->c2c_samples;
    R.c2c_iters = o->c2c_iters;
    R.c2c_method = nb_c2c_method_names[o->c2c_method];
    R.c2c_cpus = malloc((size_t)n * sizeof *R.c2c_cpus);
    if (R.c2c_cpus)
        memcpy(R.c2c_cpus, cpus, (size_t)n * sizeof *cpus);
    else
        R.has_c2c = false;
    R.c2c_m = m;
    R.c2c_cat = cat_m;
    for (int c = 0; c < NB_C2C_NCATS; c++) {
        R.cat_cnt[c] = ccnt[c];
        R.cat_min[c] = cmin[c];
        R.cat_avg[c] = ccnt[c] ? csum[c] / ccnt[c] : 0;
        R.cat_max[c] = cmax[c];
    }
    free(topo);
}

/* --------------------------------------------------------------- main */

static void usage(const char *argv0)
{
    printf(
        "NekoBench %s - memory hierarchy and core-to-core latency benchmark\n\n"
        "Usage: %s [options]\n\n"
        "  -t, --tests LIST     comma-separated: latency, bandwidth, mt, c2c, sweep, all,\n"
        "                       memlat (memory latency vs access pattern; not in all)\n"
        "                       (default: latency,bandwidth,mt,c2c)\n"
        "  -c, --cpu N          run single-thread tests on CPU N only\n"
        "                       (default: one CPU of each distinct core type)\n"
        "  -C, --cpus LIST      CPUs for multi-thread and c2c tests, e.g. 0-7,16\n"
        "                       (default: all CPUs in the affinity mask)\n"
        "  -m, --mem-size SIZE  RAM working set, e.g. 2G (default: max(1G, 32 x LLC))\n"
        "  -S, --sweep-max SIZE largest working set in the sweep (default: mem-size)\n"
        "  -d, --duration MS    duration of one bandwidth trial (default: 200)\n"
        "  -r, --repeat N       trials per measurement, best is kept (default: 3)\n"
        "  -q, --quick          shorter trials and fewer samples\n"
        "  -o, --html FILE      also write a self-contained HTML report to FILE\n"
        "      --aida           AIDA64 Cache & Memory Benchmark-style grid (all-thread\n"
        "                       read/write/copy, single-thread latency); runs alone\n"
        "                       unless --tests is also given\n"
        "      --aida-lat-block SIZE|random  --aida memory latency pattern: block-random\n"
        "                       with SIZE blocks (default 64M), or fully random;\n"
        "                       calibrate with --tests memlat\n"
        "      --aida-huge-pages  --aida buffers on 2 MiB pages instead of AIDA64's\n"
        "                       4 KiB pages\n"
        "      --small-pages    4 KiB pages for every buffer instead of 2 MiB THP (adds\n"
        "                       TLB misses, as on Windows without large pages)\n"
        "      --c2c-method M   core-to-core ping-pong: cas (default; both sides spin on\n"
        "                       compare-and-swap) or load (spin on loads, answer with a\n"
        "                       store; overstates latency on older/FSB CPUs)\n"
        "      --simd LEVEL     bandwidth kernels: sse2, avx or avx512 on x86 (default:\n"
        "                       the widest this CPU supports)\n"
        "  -V, --version        print version\n"
        "  -h, --help           this help\n\n"
        "Bandwidth is in GB/s (10^9 bytes/s) of program-visible traffic; copy counts\n"
        "bytes read + bytes written. Latency is the average random pointer-chase\n"
        "load-to-use time. Core-to-core latency is half a cache-line round trip.\n",
        NB_VERSION, argv0);
}

static bool parse_tests(const char *s, unsigned *out)
{
    char buf[256];
    snprintf(buf, sizeof buf, "%s", s);
    *out = 0;
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        if (!strcmp(tok, "latency") || !strcmp(tok, "lat"))      *out |= T_LAT;
        else if (!strcmp(tok, "bandwidth") || !strcmp(tok, "bw")) *out |= T_BW;
        else if (!strcmp(tok, "mt"))                             *out |= T_MT;
        else if (!strcmp(tok, "c2c"))                            *out |= T_C2C;
        else if (!strcmp(tok, "sweep"))                          *out |= T_SWEEP;
        else if (!strcmp(tok, "memlat"))                         *out |= T_MEMLAT;
        else if (!strcmp(tok, "all"))                            *out |= T_LAT | T_BW | T_MT | T_C2C | T_SWEEP;
        else return false;
    }
    return *out != 0;
}

static void signature(const nb_cpu_t *t, char *buf, size_t n)
{
    size_t len = (size_t)snprintf(buf, n, "%s", t->kind);
    for (int i = 0; i < t->ncache && len < n; i++)
        len += (size_t)snprintf(buf + len, n - len, "|%d:%zu", t->cache[i].level, t->cache[i].size);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    nb_init();

    opts_t o = {
        .tests = T_LAT | T_BW | T_MT | T_C2C,
        .cpu = -1,
        .cpus = nb_allowed,
        .trial_ms = 200,
        .repeat = 3,
        .c2c_samples = 25,
        .c2c_iters = 500,
        .aida_block = 64 * NB_MiB,   /* matched AIDA64 on a Core Ultra X7 358H (93.7 vs 93.1 ns) */
        .aida_small_pages = true,
    };

    static const struct option lopts[] = {
        { "tests", required_argument, NULL, 't' },
        { "cpu", required_argument, NULL, 'c' },
        { "cpus", required_argument, NULL, 'C' },
        { "mem-size", required_argument, NULL, 'm' },
        { "sweep-max", required_argument, NULL, 'S' },
        { "duration", required_argument, NULL, 'd' },
        { "repeat", required_argument, NULL, 'r' },
        { "quick", no_argument, NULL, 'q' },
        { "html", required_argument, NULL, 'o' },
        { "aida", no_argument, NULL, OPT_AIDA },
        { "aida-lat-block", required_argument, NULL, OPT_AIDA_BLOCK },
        { "aida-huge-pages", no_argument, NULL, OPT_AIDA_HUGE },
        { "small-pages", no_argument, NULL, OPT_SMALL_PAGES },
        { "c2c-method", required_argument, NULL, OPT_C2C_METHOD },
        { "simd", required_argument, NULL, OPT_SIMD },
        { "version", no_argument, NULL, 'V' },
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };
    int ch;
    bool quick = false, dur_set = false, rep_set = false, tests_set = false, aida = false, mem_set = false;
    while ((ch = getopt_long(argc, argv, "t:c:C:m:S:d:r:qo:Vh", lopts, NULL)) != -1) {
        switch (ch) {
        case 't':
            tests_set = true;
            if (!parse_tests(optarg, &o.tests)) {
                fprintf(stderr, "invalid --tests '%s'\n", optarg);
                return 2;
            }
            break;
        case 'c': {
            char *end;
            long v = strtol(optarg, &end, 10);
            if (*end || v < 0 || v >= CPU_SETSIZE || !CPU_ISSET((int)v, &nb_allowed)) {
                fprintf(stderr, "invalid or disallowed --cpu '%s'\n", optarg);
                return 2;
            }
            o.cpu = (int)v;
            break;
        }
        case 'C': {
            cpu_set_t s;
            if (nb_parse_cpulist(optarg, &s) <= 0) {
                fprintf(stderr, "invalid --cpus '%s'\n", optarg);
                return 2;
            }
            CPU_AND(&o.cpus, &s, &nb_allowed);
            if (CPU_COUNT(&o.cpus) != CPU_COUNT(&s))
                fprintf(stderr, "warning: some CPUs in --cpus are offline or not allowed; ignored\n");
            if (CPU_COUNT(&o.cpus) == 0) {
                fprintf(stderr, "no usable CPUs in --cpus\n");
                return 2;
            }
            break;
        }
        case 'm':
        case 'S': {
            size_t v;
            if (!nb_parse_size(optarg, &v) || v < 64 * NB_KiB) {
                fprintf(stderr, "invalid size '%s' (minimum 64K)\n", optarg);
                return 2;
            }
            if (ch == 'm') { o.mem_size = v; mem_set = true; }
            else           o.sweep_max = v;
            break;
        }
        case 'd':
            o.trial_ms = (unsigned)atoi(optarg);
            if (o.trial_ms < 10 || o.trial_ms > 60000) {
                fprintf(stderr, "--duration must be 10..60000 ms\n");
                return 2;
            }
            dur_set = true;
            break;
        case 'r':
            o.repeat = atoi(optarg);
            if (o.repeat < 1 || o.repeat > 1000) {
                fprintf(stderr, "--repeat must be 1..1000\n");
                return 2;
            }
            rep_set = true;
            break;
        case 'q': quick = true; break;
        case 'o': o.html = optarg; break;
        case OPT_AIDA: aida = true; break;
        case OPT_AIDA_BLOCK:
            if (!strcmp(optarg, "random") || !strcmp(optarg, "0")) {
                o.aida_block = 0;
            } else if (!nb_parse_size(optarg, &o.aida_block) || o.aida_block < 4 * NB_KiB) {
                fprintf(stderr, "invalid --aida-lat-block '%s' (minimum 4K, or 'random')\n", optarg);
                return 2;
            }
            break;
        case OPT_AIDA_HUGE: o.aida_small_pages = false; break;
        case OPT_SMALL_PAGES: o.small_pages = true; break;
        case OPT_SIMD:
            if (nb_bw_select(optarg) != 0)
                return 2;
            break;
        case OPT_C2C_METHOD: {
            int m = -1;
            for (int k = 0; k < NB_C2C_NMETHODS; k++)
                if (!strcmp(optarg, nb_c2c_method_names[k]))
                    m = k;
            if (m < 0) {
                fprintf(stderr, "invalid --c2c-method '%s' (cas or load)\n", optarg);
                return 2;
            }
            o.c2c_method = m;
            break;
        }
        case 'V': printf("nekobench %s\n", NB_VERSION); return 0;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 2;
        }
    }
    if (aida)
        o.tests = tests_set ? (o.tests | T_AIDA) : T_AIDA;
    if (o.html) {
        FILE *hf = fopen(o.html, "w");
        if (!hf) {
            perror(o.html);
            return 2;
        }
        fclose(hf);
    }
    if (quick) {
        if (!dur_set) o.trial_ms = 100;
        if (!rep_set) o.repeat = 2;
        o.c2c_samples = 11;
        o.c2c_iters = 300;
    }

    /* ---- topology & core classes */
    static int allowed[MAX_CPUS], runcpus[MAX_CPUS];
    int nallowed = nb_cpus_in(&nb_allowed, allowed, MAX_CPUS);
    int nrun = nb_cpus_in(&o.cpus, runcpus, MAX_CPUS);

    static nb_cpu_t cls[MAX_CLASS];
    static cpu_set_t cls_cpus[MAX_CLASS];
    char sigs[MAX_CLASS][256];
    int ncls = 0;
    size_t llc = 0;
    bool l4 = false;
    for (int i = 0; i < nallowed; i++) {
        nb_cpu_t t;
        char sig[256];
        nb_topo_cpu(allowed[i], &t);
        signature(&t, sig, sizeof sig);
        for (int j = 0; j < t.ncache; j++) {
            if (t.cache[j].size > llc && j == t.ncache - 1) llc = t.cache[j].size;
            if (t.cache[j].level >= 4) l4 = true;
        }
        int k;
        for (k = 0; k < ncls; k++)
            if (!strcmp(sigs[k], sig))
                break;
        if (k == ncls) {
            if (ncls == MAX_CLASS)
                continue;
            cls[ncls] = t;
            CPU_ZERO(&cls_cpus[ncls]);
            snprintf(sigs[ncls], sizeof sigs[ncls], "%s", sig);
            ncls++;
        }
        CPU_SET(allowed[i], &cls_cpus[k]);
    }

    if (!o.mem_size) {
        o.mem_size = 32 * llc > NB_GiB ? 32 * llc : NB_GiB;
        o.mem_size = (o.mem_size + 2 * NB_MiB - 1) / (2 * NB_MiB) * (2 * NB_MiB);
    }

    char model[256];
    nb_cpu_model(model, sizeof model);
    char memsz[32];
    nb_fmt_size(o.mem_size, memsz, sizeof memsz);
    printf("NekoBench %s\n", NB_VERSION);
    if (nb_topo_replayed())
        printf("NOTE:         topology replayed from NEKOBENCH_SYSFS_ROOT=%s;\n"
               "              measurements still run on this machine\n", getenv("NEKOBENCH_SYSFS_ROOT"));
    if (nb_topo_apic_rebuilt())
        printf("NOTE:         kernel APIC IDs differ from CPUID initial APIC IDs (renumbered by\n"
               "              firmware); packages and cache sharing rebuilt from CPUID\n");
    printf("CPU:          %s\n", model);
    printf("Logical CPUs: %d usable (of %ld online)\n", nallowed, sysconf(_SC_NPROCESSORS_ONLN));
    nb_huge_pages = !o.small_pages;
    printf("THP:          %s%s\n", nb_thp_mode(),
           o.small_pages ? "  (not used: --small-pages, latencies include TLB misses)"
           : strcmp(nb_thp_mode(), "never") == 0
               ? "  (WARNING: no huge pages, latencies include TLB misses)" : "");
    printf("RAM test WS:  %s\n", memsz);
    const char *simd = nb_bw_simd_name();
    nb_cpufreq_policy(R.policy, sizeof R.policy);
    printf("CPU freq:     %s\n", R.policy);
    if (nb_llc_clock_risk(&o.cpus))
        printf("NOTE:         the shared L3 spans several cpufreq policies that aren't all\n"
               "              \"performance\"; on some SoCs (e.g. RK3588) its clock follows one\n"
               "              cluster, so a core tested alone sees slow L3/RAM while the other\n"
               "              cluster idles. Set every policy to performance for stable numbers.\n");
    printf("Compiler:     %s, SIMD stores: %s, NT stores: %s\n", __VERSION__, simd,
           nb_bw_kernel_available(NB_BW_NTWRITE) ? "yes" : "no");

    /* report header */
    snprintf(R.version, sizeof R.version, "%s", NB_VERSION);
    snprintf(R.model, sizeof R.model, "%s", model);
    snprintf(R.thp, sizeof R.thp, "%s", nb_thp_mode());
    snprintf(R.compiler, sizeof R.compiler, "%s", __VERSION__);
    snprintf(R.simd, sizeof R.simd, "%s", simd);
    R.nallowed = nallowed;
    R.nonline = sysconf(_SC_NPROCESSORS_ONLN);
    R.mem_size = o.mem_size;
    R.l4 = l4;
    R.has_lat = o.tests & T_LAT;
    R.has_bw = o.tests & T_BW;
    {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(R.date, sizeof R.date, "%Y-%m-%d %H:%M %Z", &tm);
        size_t len = 0;
        for (int i = 0; i < argc && len < sizeof R.cmdline - 1; i++)
            len += (size_t)snprintf(R.cmdline + len, sizeof R.cmdline - len, "%s%s", i ? " " : "", argv[i]);
    }
    R.ncls = ncls < NB_RPT_MAX_CLASS ? ncls : NB_RPT_MAX_CLASS;
    for (int k = 0; k < R.ncls; k++) {
        R.cls[k].topo = cls[k];
        nb_fmt_cpulist(&cls_cpus[k], R.cls[k].cpus, sizeof R.cls[k].cpus);
    }

    printf("\nCache topology (%d core type%s):\n", ncls, ncls == 1 ? "" : "s");
    for (int k = 0; k < ncls; k++) {
        char list[512];
        nb_fmt_cpulist(&cls_cpus[k], list, sizeof list);
        printf("  %-9s CPUs %s\n", cls[k].kind, list);
        for (int j = 0; j < cls[k].ncache; j++) {
            char name[16], sz[32];
            nb_fmt_size(cls[k].cache[j].size, sz, sizeof sz);
            char fixed[96] = "";
            if (cls[k].cache[j].shared_fixed)
                snprintf(fixed, sizeof fixed, "  [OS reports %d; rebuilt from CPUID]",
                         cls[k].cache[j].os_nshared);
            printf("          %-4s %10s, %zu B lines, shared by %d CPU%s%s%s\n",
                   level_name(&cls[k].cache[j], name, sizeof name), sz, cls[k].cache[j].line,
                   cls[k].cache[j].nshared, cls[k].cache[j].nshared == 1 ? "" : "s",
                   cls[k].cache[j].inferred ? "  [not reported by the OS; from CPU model table]" : "",
                   fixed);
        }
    }
    printf("  L4: %s\n", l4 ? "present" : "not present");

    /* ---- run */
    if (o.tests & (T_LAT | T_BW | T_SWEEP)) {
        if (o.cpu >= 0) {
            nb_cpu_t t;
            nb_topo_cpu(o.cpu, &t);
            run_single(&o, &t);
        } else {
            for (int k = 0; k < ncls; k++) {
                /* Prefer a representative inside --cpus if one exists. */
                nb_cpu_t t = cls[k];
                cpu_set_t both;
                CPU_AND(&both, &cls_cpus[k], &o.cpus);
                if (CPU_COUNT(&both) == 0)
                    continue;
                nb_topo_cpu(best_cpu(&both), &t);
                run_single(&o, &t);
            }
        }
    }
    if (o.tests & T_MT)
        run_mt(&o, runcpus, nrun);
    /* Latency on the fastest-boosting CPU of any core type: CPU 0 is a
     * LITTLE core on most Arm big.LITTLE SoCs, so "first type" is wrong. */
    int lat_cpu = o.cpu;
    if (lat_cpu < 0)
        lat_cpu = best_cpu(&o.cpus);
    if (o.tests & T_AIDA)
        run_aida(&o, runcpus, nrun, lat_cpu, mem_set);
    if (o.tests & T_MEMLAT)
        run_memlat(&o, lat_cpu);
    if (o.tests & T_C2C)
        run_c2c(&o, runcpus, nrun);

    int rc = 0;
    if (o.html) {
        if (nb_html_write(o.html, &R) == 0) {
            printf("\nHTML report written to %s\n", o.html);
        } else {
            perror(o.html);
            rc = 1;
        }
    }
    free(R.c2c_m);
    free(R.c2c_cat);
    free(R.c2c_cpus);
    return rc + (int)(nb_sink & 0);
}
