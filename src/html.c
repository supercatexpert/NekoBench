/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Kyosuke Nekoyashiki
 */
/* Self-contained HTML report: inline CSS/JS, no external resources. Tables
 * carry every number (the page is fully readable without JavaScript); the
 * script adds the sweep charts, tooltips and the theme toggle. */
#include "report.h"
#include "report_assets.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const nb_c2c_cat_names[3] = {
    "Same core (SMT siblings)", "Same L2 cluster", "Different core/cluster"
};

/* ------------------------------------------------------------- escaping */

static void esc(FILE *f, const char *s)
{
    for (; *s; s++) {
        switch (*s) {
        case '<':  fputs("&lt;", f); break;
        case '>':  fputs("&gt;", f); break;
        case '&':  fputs("&amp;", f); break;
        case '"':  fputs("&quot;", f); break;
        case '\'': fputs("&#39;", f); break;
        default:   fputc(*s, f);
        }
    }
}

/* JSON string, safe inside <script>: '<' is emitted as <. */
static void jstr(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            fprintf(f, "\\%c", c);
        else if (c < 0x20 || c == '<' || c == '>' || c == '&')
            fprintf(f, "\\u%04x", c);
        else
            fputc(c, f);
    }
    fputc('"', f);
}

static void jnum(FILE *f, double v)
{
    if (isnan(v) || isinf(v) || v < 0)
        fputs("null", f);
    else
        fprintf(f, "%.6g", v);
}

static const char *fsize(size_t s, char *buf, size_t n)
{
    nb_fmt_size(s, buf, n);
    return buf;
}

static const char *cache_label(const nb_cache_t *c, char *buf, size_t n)
{
    snprintf(buf, n, "L%d%s", c->level, c->type == 'D' ? "d" : "");
    return buf;
}

static int slot_of(int i) { return i % 8 + 1; }

/* ------------------------------------------------------------- sections */

static void w_system(FILE *f, const nb_report_t *r)
{
    char mem[32];
    fputs("<section class='card'><h2>System</h2><dl class='info'>", f);
#define ITEM(k, v) do { fputs("<div><dt>" k "</dt><dd>", f); esc(f, v); fputs("</dd></div>", f); } while (0)
    ITEM("CPU", r->model);
    char cpus[64];
    snprintf(cpus, sizeof cpus, "%d usable of %ld online", r->nallowed, r->nonline);
    ITEM("Logical CPUs", cpus);
    ITEM("Transparent huge pages", r->thp);
    ITEM("RAM working set", fsize(r->mem_size, mem, sizeof mem));
    ITEM("CPU frequency policy", r->policy);
    ITEM("Compiler", r->compiler);
    char simd[64];
    snprintf(simd, sizeof simd, "%s, NT stores %s", r->simd,
             nb_bw_kernel_available(NB_BW_NTWRITE) ? "yes" : "no");
    ITEM("SIMD kernels", simd);
    ITEM("Date", r->date);
    ITEM("Command", r->cmdline);
#undef ITEM
    fputs("</dl></section>\n", f);
}

static void w_topology(FILE *f, const nb_report_t *r)
{
    int minlvl = 1, maxlvl = 3;
    bool inferred = false;
    for (int k = 0; k < r->ncls; k++)
        for (int j = 0; j < r->cls[k].topo.ncache; j++) {
            const nb_cache_t *c = &r->cls[k].topo.cache[j];
            if (c->level > maxlvl) maxlvl = c->level;
            if (c->level < minlvl) minlvl = c->level;
            inferred |= c->inferred;
        }

    fputs("<section class='card'><h2>Cache topology</h2>", f);
    fprintf(f, "<p class='sub'>%d core type%s. L4: %s.</p>", r->ncls, r->ncls == 1 ? "" : "s",
            r->l4 ? "present" : "not present");
    fputs("<div class='scroll'><table class='data'><thead><tr><th>Core type</th><th>CPUs</th>", f);
    for (int l = minlvl; l <= maxlvl; l++)
        fprintf(f, "<th>L%d%s</th>", l, l <= 1 ? "d" : "");
    fputs("</tr></thead><tbody>", f);
    for (int k = 0; k < r->ncls; k++) {
        const nb_cpu_t *t = &r->cls[k].topo;
        fputs("<tr><td class='lvl'>", f);
        esc(f, t->kind);
        fputs("</td><td>", f);
        esc(f, r->cls[k].cpus);
        fputs("</td>", f);
        for (int l = minlvl; l <= maxlvl; l++) {
            const nb_cache_t *c = NULL;
            for (int j = 0; j < t->ncache; j++)
                if (t->cache[j].level == l)
                    c = &t->cache[j];
            if (!c) {
                fputs("<td><span class='dim'>none</span></td>", f);
                continue;
            }
            char sz[32];
            fprintf(f, "<td>%s%s <span class='dim'>&middot; %zu B lines &middot; shared by %d CPU%s</span></td>",
                    fsize(c->size, sz, sizeof sz), c->inferred ? "*" : "", c->line, c->nshared,
                    c->nshared == 1 ? "" : "s");
        }
        fputs("</tr>", f);
    }
    fputs("</tbody></table></div>", f);
    if (inferred)
        fputs("<p class='note'>* Not enumerated by the OS (CPUID leaf 4). Added from NekoBench's CPU model table: "
              "on these P-cores Intel calls the 48 KiB first-level D-cache L0, backed by a 192 KiB L1 D-cache.</p>", f);
    fputs("</section>\n", f);
}

static bool show_kernel(int k)
{
    return k != NB_BW_NTCOPY && (k != NB_BW_NTWRITE || nb_bw_kernel_available(k));
}

/* Bandwidth/latency table. Each bandwidth column carries a thin data bar
 * scaled to that column's maximum. */
static void w_rows(FILE *f, const nb_rpt_row_t *rows, int n, bool lat, bool bw, bool threads,
                   const char *h1, const char *h2)
{
    double colmax[NB_BW_NKERNELS] = { 0 };
    for (int i = 0; i < n; i++)
        for (int k = 0; k < NB_BW_NKERNELS; k++)
            if (rows[i].bw[k] > colmax[k])
                colmax[k] = rows[i].bw[k];

    fputs("<div class='scroll'><table class='data'><thead><tr><th>Level</th>", f);
    if (threads)
        fputs("<th class='num'>Threads</th>", f);
    fprintf(f, "<th>%s</th><th>%s</th>", h1, h2);
    if (lat)
        fputs("<th class='num'>Latency (ns)</th><th class='num'>Cycles</th>", f);
    if (bw)
        for (int k = 0; k < NB_BW_NKERNELS; k++)
            if (show_kernel(k))
                fprintf(f, "<th class='num'>%s (GB/s)</th>", nb_bw_names[k]);
    fputs("</tr></thead><tbody>", f);

    for (int i = 0; i < n; i++) {
        const nb_rpt_row_t *row = &rows[i];
        fputs("<tr><td class='lvl'>", f);
        esc(f, row->name);
        if (threads)
            fprintf(f, "</td><td class='num'>%d", row->threads);
        fputs("</td><td>", f);
        esc(f, row->col1);
        fputs("</td><td>", f);
        esc(f, row->col2);
        fputs("</td>", f);
        if (lat) {
            if (isnan(row->lat_ns))
                fputs("<td class='num'>&ndash;</td><td class='num'>&ndash;</td>", f);
            else
                fprintf(f, "<td class='num'>%.2f</td><td class='num'>%.1f</td>", row->lat_ns, row->lat_cyc);
        }
        if (bw) {
            for (int k = 0; k < NB_BW_NKERNELS; k++) {
                if (!show_kernel(k))
                    continue;
                double v = row->bw[k];
                if (v < 0) {
                    fputs("<td class='num'><span class='dim'>&ndash;</span></td>", f);
                    continue;
                }
                double pct = colmax[k] > 0 ? v / colmax[k] * 100 : 0;
                if (pct < 2)
                    pct = 2;
                fprintf(f, "<td class='num'><span class='cell'>%.1f<span class='bar' style='width:%.1f%%'></span></span></td>",
                        v, pct);
            }
        }
        fputs("</tr>", f);
    }
    fputs("</tbody></table></div>", f);
}

static void w_aida(FILE *f, const nb_report_t *r)
{
    if (!r->has_aida || !r->aida_nrows)
        return;
    fputs("<section class='card'><h2>AIDA64-style Cache &amp; Memory Benchmark</h2><p class='sub'>", f);
    fprintf(f, "Read/write/copy: %d threads on CPUs ", r->aida_threads);
    esc(f, r->aida_cpus);
    fprintf(f, ". Latency: CPU %d (", r->aida_lat_cpu);
    esc(f, r->aida_lat_kind);
    fprintf(f, ", %.2f GHz measured during the latency runs).</p>", r->aida_ghz);

    double colmax[3] = { 0 };
    for (int i = 0; i < r->aida_nrows; i++) {
        const nb_rpt_aida_t *a = &r->aida[i];
        double v[3] = { a->read, a->write, a->copy };
        for (int k = 0; k < 3; k++)
            if (v[k] > colmax[k])
                colmax[k] = v[k];
    }
    fputs("<div class='scroll'><table class='data'><thead><tr><th></th><th class='num'>Read</th>"
          "<th class='num'>Write</th><th class='num'>Copy</th><th class='num'>Latency</th><th class='num'>Cycles</th></tr></thead><tbody>", f);
    for (int i = 0; i < r->aida_nrows; i++) {
        const nb_rpt_aida_t *a = &r->aida[i];
        double v[3] = { a->read, a->write, a->copy };
        fputs("<tr><td class='lvl'>", f);
        esc(f, a->name);
        fputs("</td>", f);
        for (int k = 0; k < 3; k++) {
            if (v[k] < 0) {
                fputs("<td class='num'><span class='dim'>failed</span></td>", f);
                continue;
            }
            double pct = colmax[k] > 0 ? v[k] / colmax[k] * 100 : 0;
            if (pct < 2)
                pct = 2;
            if (a->ram)
                fprintf(f, "<td class='num'><span class='cell'>%.0f MB/s<span class='bar' style='width:%.1f%%'></span></span></td>",
                        v[k] * 1000, pct);
            else
                fprintf(f, "<td class='num'><span class='cell'>%.1f GB/s<span class='bar' style='width:%.1f%%'></span></span></td>",
                        v[k], pct);
        }
        if (isnan(a->lat_ns))
            fputs("<td class='num'>&ndash;</td><td class='num'>&ndash;</td></tr>", f);
        else
            fprintf(f, "<td class='num'>%.1f ns</td><td class='num'>%.1f</td></tr>", a->lat_ns, a->lat_cyc);
    }
    fputs("</tbody></table></div>", f);
    fprintf(f, "<p class='note'>Copy counts bytes read plus bytes written, as AIDA64 scores it. "
               "Memory write and copy use %s. Memory bandwidth uses %s per thread on %s pages; memory latency is a "
               "%s chase, run before the bandwidth tests. Compare latency across tools in cycles, since ns scale with the clock. MB/s and GB/s are decimal. The first cache row is every core's first-level D-cache "
               "(P-core L0 plus E-core L1d on Lion/Cougar Cove hybrids). Bars are scaled to each column's maximum.</p></section>\n",
            r->aida_nt ? "non-temporal stores" : "regular stores", r->aida_mem_per,
            r->aida_small_pages ? "4 KiB" : "2 MiB", r->aida_lat_pattern);
}

static void w_single(FILE *f, const nb_report_t *r)
{
    if (!r->nsingle || !(r->has_lat || r->has_bw))
        return;
    fputs("<section class='card'><h2>Single-thread latency and bandwidth</h2>"
          "<p class='sub'>One thread pinned to one CPU of each core type. Latency is the "
          "average random pointer-chase load-to-use time; bars are scaled to each column's maximum.</p>", f);
    for (int s = 0; s < r->nsingle; s++) {
        const nb_rpt_single_t *x = &r->single[s];
        fprintf(f, "<h3>CPU %d &middot; ", x->topo.cpu);
        esc(f, x->topo.kind);
        fprintf(f, " &middot; measured clock %.2f GHz</h3>", x->ghz);
        w_rows(f, x->rows, x->nrows, r->has_lat, r->has_bw, false, "Size", "Working set");
    }
    fputs("</section>\n", f);
}

static int sweep_count(const nb_report_t *r)
{
    int n = 0;
    for (int s = 0; s < r->nsingle; s++)
        if (r->single[s].npts > 1)
            n++;
    return n;
}

static void w_sweep(FILE *f, const nb_report_t *r)
{
    if (!sweep_count(r))
        return;
    fputs("<section class='card'><h2>Working-set sweep</h2>"
          "<p class='sub'>Latency and single-thread read bandwidth as the working set grows. "
          "Dashed lines mark cache sizes. Hover or focus a chart and use the arrow keys for exact values.</p>", f);
    fputs("<div class='legend'>", f);
    for (int s = 0; s < r->nsingle; s++)
        if (r->single[s].npts > 1) {
            fprintf(f, "<span><span class='key bg%d'></span>CPU %d &middot; ", slot_of(s), r->single[s].topo.cpu);
            esc(f, r->single[s].topo.kind);
            fputs("</span>", f);
        }
    fputs("</div>", f);
    fputs("<div class='chart-wrap' id='chart-lat'></div><div class='chart-wrap' id='chart-bw'></div>", f);

    /* table view */
    fputs("<details><summary>Table view</summary><div class='scroll'><table class='data'><thead><tr><th>Working set</th>", f);
    for (int s = 0; s < r->nsingle; s++)
        if (r->single[s].npts > 1)
            fprintf(f, "<th class='num'>CPU %d ns</th><th class='num'>CPU %d cycles</th><th class='num'>CPU %d read GB/s</th>",
                    r->single[s].topo.cpu, r->single[s].topo.cpu, r->single[s].topo.cpu);
    fputs("</tr></thead><tbody>", f);
    int npts = 0;
    for (int s = 0; s < r->nsingle; s++)
        if (r->single[s].npts > npts)
            npts = r->single[s].npts;
    for (int i = 0; i < npts; i++) {
        size_t ws = 0;
        for (int s = 0; s < r->nsingle && !ws; s++)
            if (i < r->single[s].npts)
                ws = r->single[s].pts[i].ws;
        char sz[32];
        fprintf(f, "<tr><td>%s</td>", fsize(ws, sz, sizeof sz));
        for (int s = 0; s < r->nsingle; s++) {
            if (r->single[s].npts <= 1)
                continue;
            if (i >= r->single[s].npts) {
                fputs("<td></td><td></td><td></td>", f);
                continue;
            }
            const nb_rpt_pt_t *p = &r->single[s].pts[i];
            fprintf(f, "<td class='num'>%.2f</td><td class='num'>%.1f</td><td class='num'>%.1f</td>",
                    p->lat_ns, p->lat_cyc, p->read_gbs);
        }
        fputs("</tr>", f);
    }
    fputs("</tbody></table></div></details></section>\n", f);
}

static void w_mt(FILE *f, const nb_report_t *r)
{
    if (!r->has_mt)
        return;
    fprintf(f, "<section class='card'><h2>Multi-thread aggregate bandwidth</h2>"
               "<p class='sub'>%d threads on CPUs ", r->mt_threads);
    esc(f, r->mt_cpus);
    fputs(". Each thread streams its own buffer, sized to its share of each cache level; "
          "the figure is the sum of per-thread rates. A cache row runs only on CPUs that have that level.</p>", f);
    w_rows(f, r->mt_rows, r->mt_nrows, false, true, true, "Total working set", "Per-thread working set");
    fputs("</section>\n", f);
}

static int cmp_dbl(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void w_c2c(FILE *f, const nb_report_t *r)
{
    if (!r->has_c2c || r->c2c_n < 2)
        return;
    int n = r->c2c_n;

    /* Colour scale: 9 buckets from the minimum to the 95th percentile, so a
     * few noisy outliers don't wash out the structure. */
    double *v = malloc((size_t)n * (size_t)n * sizeof *v);
    int nv = 0;
    for (int i = 0; i < n * n; i++)
        if (!isnan(r->c2c_m[i]))
            v[nv++] = r->c2c_m[i];
    double lo = 0, hi = 1;
    if (nv) {
        qsort(v, (size_t)nv, sizeof *v, cmp_dbl);
        lo = v[0];
        hi = v[(int)((nv - 1) * 0.95)];
        if (hi <= lo)
            hi = lo + 1;
    }
    free(v);

    fprintf(f, "<section class='card'><h2>Core-to-core latency</h2>"
               "<p class='sub'>One-way latency in ns (half a cache-line round trip), best of %d samples "
               "&times; %d round trips, CPUs ", r->c2c_samples, r->c2c_iters);
    esc(f, r->c2c_cpulist);
    fputs(".</p>", f);

    fputs("<table class='data' style='width:auto'><thead><tr><th>Pair type</th><th class='num'>Pairs</th>"
          "<th class='num'>Min (ns)</th><th class='num'>Avg (ns)</th><th class='num'>Max (ns)</th></tr></thead><tbody>", f);
    for (int c = 0; c < 3; c++)
        if (r->cat_cnt[c])
            fprintf(f, "<tr><td>%s</td><td class='num'>%d</td><td class='num'>%.1f</td>"
                       "<td class='num'>%.1f</td><td class='num'>%.1f</td></tr>",
                    nb_c2c_cat_names[c], r->cat_cnt[c], r->cat_min[c], r->cat_avg[c], r->cat_max[c]);
    fputs("</tbody></table>", f);

    fputs("<h3>Matrix</h3><div class='legend'>", f);
    for (int q = 0; q < 9; q++) {
        double a = lo + (hi - lo) * q / 9.0, b = lo + (hi - lo) * (q + 1) / 9.0;
        if (q == 8)
            fprintf(f, "<span><span class='sw q%d'></span>&ge;%.0f</span>", q, a);
        else
            fprintf(f, "<span><span class='sw q%d'></span>%.0f&ndash;%.0f</span>", q, a, b);
    }
    fputs("</div><div class='scroll'><table class='heat'><thead><tr><th class='rh'>CPU</th>", f);
    for (int j = 0; j < n; j++)
        fprintf(f, "<th data-cpu='%d'>%d</th>", r->c2c_cpus[j], r->c2c_cpus[j]);
    fputs("</tr></thead><tbody>", f);
    for (int i = 0; i < n; i++) {
        fprintf(f, "<tr><th class='rh'>%d</th>", r->c2c_cpus[i]);
        for (int j = 0; j < n; j++) {
            double x = r->c2c_m[i * n + j];
            if (isnan(x)) {
                fprintf(f, "<td class='diag'>%s</td>", i == j ? "&middot;" : "?");
                continue;
            }
            int q = (int)((x - lo) / (hi - lo) * 9);
            q = q < 0 ? 0 : q > 8 ? 8 : q;
            fprintf(f, "<td class='q%d' data-a='%d' data-b='%d' data-v='%.1f' data-k='%d'>%.0f</td>", q,
                    r->c2c_cpus[i], r->c2c_cpus[j], x, r->c2c_cat ? r->c2c_cat[i * n + j] : 2, x);
        }
        fputs("</tr>", f);
    }
    fputs("</tbody></table></div></section>\n", f);
}

static void w_data(FILE *f, const nb_report_t *r)
{
    fputs("<script type='application/json' id='nb-data'>{", f);
    fputs("\"c2cCats\":[", f);
    for (int c = 0; c < 3; c++) {
        if (c)
            fputc(',', f);
        jstr(f, nb_c2c_cat_names[c]);
    }
    fputc(']', f);

    if (sweep_count(r)) {
        int first = -1;
        for (int s = 0; s < r->nsingle; s++)
            if (r->single[s].npts > 1 && (first < 0 || r->single[s].npts > r->single[first].npts))
                first = s;
        const nb_rpt_single_t *ref = &r->single[first];
        fputs(",\"sweep\":{\"xs\":[", f);
        for (int i = 0; i < ref->npts; i++)
            fprintf(f, "%s%zu", i ? "," : "", ref->pts[i].ws);
        fputs("],\"series\":[", f);
        bool firstser = true;
        for (int s = 0; s < r->nsingle; s++) {
            const nb_rpt_single_t *x = &r->single[s];
            if (x->npts <= 1)
                continue;
            char name[64], shortname[32];
            snprintf(name, sizeof name, "CPU %d \xc2\xb7 %s", x->topo.cpu, x->topo.kind);
            snprintf(shortname, sizeof shortname, "%s", x->topo.kind);
            fprintf(f, "%s{\"slot\":%d,\"name\":", firstser ? "" : ",", slot_of(s));
            jstr(f, name);
            fputs(",\"short\":", f);
            jstr(f, shortname);
            const char *fields[2] = { "lat", "bw" };
            for (int fi = 0; fi < 2; fi++) {
                fprintf(f, ",\"%s\":[", fields[fi]);
                for (int i = 0; i < ref->npts; i++) {
                    if (i)
                        fputc(',', f);
                    if (i >= x->npts || x->pts[i].ws != ref->pts[i].ws)
                        fputs("null", f);
                    else
                        jnum(f, fi == 0 ? x->pts[i].lat_ns : x->pts[i].read_gbs);
                }
                fputc(']', f);
            }
            fputc('}', f);
            firstser = false;
        }
        /* cache-size markers, de-duplicated across core types */
        fputs("],\"markers\":[", f);
        bool firstmk = true;
        for (int s = 0; s < r->nsingle; s++) {
            const nb_rpt_single_t *x = &r->single[s];
            if (x->npts <= 1)
                continue;
            for (int j = 0; j < x->topo.ncache; j++) {
                const nb_cache_t *c = &x->topo.cache[j];
                bool dup = false;
                for (int p = 0; p < s && !dup; p++)
                    for (int q = 0; q < r->single[p].topo.ncache; q++)
                        if (r->single[p].npts > 1 && r->single[p].topo.cache[q].level == c->level &&
                            r->single[p].topo.cache[q].size == c->size)
                            dup = true;
                if (dup)
                    continue;
                char lbl[48], nm[16], sz[32];
                nb_fmt_size(c->size, sz, sizeof sz);
                snprintf(lbl, sizeof lbl, "%s %s", cache_label(c, nm, sizeof nm), sz);
                fprintf(f, "%s{\"x\":%zu,\"slot\":%d,\"label\":", firstmk ? "" : ",", c->size, slot_of(s));
                jstr(f, lbl);
                fputc('}', f);
                firstmk = false;
            }
        }
        fputs("]}", f);
    }
    fputs("}</script>\n", f);
}

static void w_footer(FILE *f, const nb_report_t *r)
{
    fprintf(f, "<footer><p>Generated by NekoBench %s.</p><ul>"
        "<li>Bandwidth is GB/s (10<sup>9</sup> bytes/s) of program-visible traffic; copy counts bytes read plus bytes written. "
        "Regular writes to RAM also cause read-for-ownership traffic that is not counted; NT-Write avoids it.</li>"
        "<li>Latency: every 64 B line of the buffer is linked into one random cycle and walked with dependent loads, "
        "defeating hardware prefetchers. Cycles use a clock measured on the same core with a dependent-add chain.</li>"
        "<li>Working sets: L1 at half its size; each further level at the geometric midpoint between the levels below and its own capacity. "
        "Buffers are 2 MiB-aligned and THP-advised to keep TLB misses out of the results.</li>"
        "<li>Core-to-core: two pinned threads bounce a sequence number through one 128 B-aligned cache line; one-way = round trip / 2.</li>"
        "</ul></footer>", r->version);
}

int nb_html_write(const char *path, const nb_report_t *r)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;

    fputs("<!doctype html>\n<html lang='en'><head><meta charset='utf-8'>"
          "<meta name='viewport' content='width=device-width,initial-scale=1'>"
          "<title>NekoBench report</title><style>\n", f);
    fputs(nb_asset_report_css, f);
    fputs("</style></head><body><div class='wrap'>\n", f);

    fputs("<header class='top'><div><h1>NekoBench report</h1><p class='sub'>", f);
    esc(f, r->model);
    fputs(" &middot; ", f);
    esc(f, r->date);
    fputs("</p></div><button id='nb-theme' type='button'>Toggle theme</button></header>\n", f);

    w_system(f, r);
    w_topology(f, r);
    w_aida(f, r);
    w_single(f, r);
    w_sweep(f, r);
    w_mt(f, r);
    w_c2c(f, r);
    w_footer(f, r);

    fputs("</div>\n", f);
    w_data(f, r);
    fputs("<script>\n", f);
    fputs(nb_asset_report_js, f);
    fputs("</script></body></html>\n", f);

    int err = ferror(f);
    if (fclose(f) != 0)
        err = 1;
    return err ? -1 : 0;
}
