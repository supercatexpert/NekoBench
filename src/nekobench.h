/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Kyosuke Nekoyashiki
 */
/* NekoBench - memory hierarchy and inter-core latency benchmark. */
#ifndef NEKOBENCH_H
#define NEKOBENCH_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NB_MAX_CACHES 8
#define NB_LINE       64          /* granularity of the pointer chase */
#define NB_KiB        ((size_t)1 << 10)
#define NB_MiB        ((size_t)1 << 20)
#define NB_GiB        ((size_t)1 << 30)

/* ------------------------------------------------------------------ util */

extern cpu_set_t nb_allowed;          /* affinity mask the process started with */
extern volatile uint64_t nb_sink;     /* defeats dead-code elimination */

void        nb_init(void);
uint64_t    nb_now_ns(void);
void        nb_sleep_ms(unsigned ms);
int         nb_pin_self(int cpu);     /* 0 on success */
void        nb_unpin_self(void);      /* back to nb_allowed */
void       *nb_alloc(size_t size);    /* 2 MiB aligned, THP-advised, pre-faulted */
/* When false, nb_alloc advises MADV_NOHUGEPAGE (4 KiB pages even with THP
 * "always"). Set only from the orchestrating thread between runs. */
extern bool nb_huge_pages;
void        nb_free(void *p, size_t size);
uint64_t    nb_rand(uint64_t *state);
bool        nb_parse_size(const char *s, size_t *out);
int         nb_parse_cpulist(const char *s, cpu_set_t *set); /* -1 on error */
void        nb_fmt_size(size_t size, char *buf, size_t n);
void        nb_fmt_cpulist(const cpu_set_t *set, char *buf, size_t n);
const char *nb_thp_mode(void);
int         nb_cpus_in(const cpu_set_t *set, int *out, int max);

/* -------------------------------------------------------------- topology */

typedef struct {
    int       level;      /* 0..4 (0 = Intel "L0" on Lion/Cougar Cove) */
    char      type;       /* 'D' = data, 'U' = unified */
    size_t    size;       /* bytes */
    size_t    line;       /* bytes */
    cpu_set_t shared;     /* logical CPUs sharing this cache instance */
    int       nshared;
    bool      inferred;   /* not enumerated by the OS; added from the CPU model table */
    int       os_nshared; /* if shared_fixed: the sharer count the OS reported */
    bool      shared_fixed; /* sharing rebuilt from CPUID initial APIC IDs */
    bool      inclusive;  /* holds the lower levels' data (CPUID leaf 4), x86 Intel only */
} nb_cache_t;

typedef struct {
    int         cpu;
    const char *kind;     /* "P-core", "E-core", "LP E-core" or "core" */
    int         package;  /* physical package (socket) id, -1 if unknown */
    int         ncache;
    nb_cache_t  cache[NB_MAX_CACHES];   /* data/unified only, sorted by level */
} nb_cpu_t;

int  nb_topo_cpu(int cpu, nb_cpu_t *out);
unsigned nb_cpu_max_mhz(int cpu);              /* cpufreq max (boost) clock, 0 if unknown */
void nb_cpufreq_policy(char *buf, size_t n);   /* "governor / EPP" of the first CPU */
bool nb_topo_replayed(void);   /* NEKOBENCH_SYSFS_ROOT in effect */
/* The kernel's APIC IDs differ from CPUID's initial ones (firmware
 * renumbering); packages and cache sharing were rebuilt from CPUID. */
bool nb_topo_apic_rebuilt(void);
void nb_cpu_model(char *buf, size_t n);

/* --------------------------------------------------------------- latency */

double nb_measure_freq_ghz(int cpu);
/* Average load-to-use latency (ns) of a random pointer chase over `size`. */
double nb_latency_ns(int cpu, size_t size, unsigned trial_ms, int repeat);
/* Same, with a block-random chain (random order inside each `block`). */
double nb_latency_block_ns(int cpu, size_t size, size_t block, unsigned trial_ms, int repeat);

/* ------------------------------------------------------------- bandwidth */

/* NB_BW_NTCOPY (normal loads, streaming stores) is only used by --aida. */
enum { NB_BW_READ, NB_BW_WRITE, NB_BW_COPY, NB_BW_NTWRITE, NB_BW_NTCOPY, NB_BW_NKERNELS };
extern const char *const nb_bw_names[NB_BW_NKERNELS];
bool nb_bw_kernel_available(int kernel);

/* Run `n` threads (thread i pinned to cpus[i], working set sizes[i]) and
 * report aggregate GB/s per kernel in out[] (-1 for kernels not requested).
 * Returns 0 on success. */
int nb_bw_run(int n, const int *cpus, const size_t *sizes, unsigned kernel_mask,
              unsigned trial_ms, int repeat, double out[NB_BW_NKERNELS]);

/* ------------------------------------------------------------------- c2c */

enum { NB_C2C_CAS, NB_C2C_LOAD, NB_C2C_NMETHODS };
extern const char *const nb_c2c_method_names[NB_C2C_NMETHODS];

/* One-way core-to-core latency between CPUs a and b (half a round trip of a
 * cache line ping-pong, see c2c.c for the methods). Returns 0 on success. */
int nb_c2c_pair(int a, int b, int method, int samples, int iters, double *median_ns,
                double *min_ns);

#endif
