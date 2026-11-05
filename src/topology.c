#include "nekobench.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* NEKOBENCH_SYSFS_ROOT=/some/dir makes every /sys and /proc read come from
 * /some/dir/sys/... and /some/dir/proc/..., so the topology logic can be
 * exercised against a dump of another machine. */
static const char *sys_root(void)
{
    static const char *root;
    static int init;
    if (!init) {
        init = 1;
        root = getenv("NEKOBENCH_SYSFS_ROOT");
        if (root && !*root)
            root = NULL;
    }
    return root;
}

static FILE *open_sys(const char *path)
{
    const char *root = sys_root();
    if (!root)
        return fopen(path, "r");
    char full[1024];
    snprintf(full, sizeof full, "%s%s", root, path);
    return fopen(full, "r");
}

bool nb_topo_replayed(void) { return sys_root() != NULL; }

static bool read_line(const char *path, char *buf, size_t n)
{
    FILE *f = open_sys(path);
    if (!f)
        return false;
    bool ok = fgets(buf, (int)n, f) != NULL;
    fclose(f);
    if (ok)
        buf[strcspn(buf, "\r\n")] = 0;
    return ok;
}

static size_t parse_sysfs_size(const char *s)
{
    char *end;
    size_t v = strtoull(s, &end, 10);
    if (*end == 'K')
        v *= NB_KiB;
    else if (*end == 'M')
        v *= NB_MiB;
    else if (*end == 'G')
        v *= NB_GiB;
    return v;
}

/* Hybrid Intel parts expose separate PMUs listing the CPUs of each core type. */
static const char *core_kind(int cpu)
{
    static int init;
    static cpu_set_t pcores, ecores;
    static bool hybrid;
    if (!init) {
        char buf[1024];
        init = 1;
        CPU_ZERO(&pcores);
        CPU_ZERO(&ecores);
        if (read_line("/sys/devices/cpu_core/cpus", buf, sizeof buf) &&
            nb_parse_cpulist(buf, &pcores) > 0 &&
            read_line("/sys/devices/cpu_atom/cpus", buf, sizeof buf) &&
            nb_parse_cpulist(buf, &ecores) > 0)
            hybrid = true;
    }
    if (hybrid && CPU_ISSET(cpu, &pcores))
        return "P-core";
    if (hybrid && CPU_ISSET(cpu, &ecores))
        return "E-core";

    /* Heterogeneous ARM (big.LITTLE / DynamIQ): Linux exposes each CPU's
     * relative compute capacity (max 1024). */
    static int cap_init, cap_min = -1, cap_max = -1;
    if (!cap_init) {
        cap_init = 1;
        for (int c = 0; c < CPU_SETSIZE; c++) {
            if (!CPU_ISSET(c, &nb_allowed))
                continue;
            char path[128], buf[32];
            snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpu_capacity", c);
            if (!read_line(path, buf, sizeof buf))
                continue;
            int v = atoi(buf);
            if (cap_min < 0 || v < cap_min) cap_min = v;
            if (v > cap_max) cap_max = v;
        }
    }
    if (cap_min >= 0 && cap_min != cap_max) {
        char path[128], buf[32];
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpu_capacity", cpu);
        if (read_line(path, buf, sizeof buf)) {
            int v = atoi(buf);
            return v == cap_max ? "big core" : v == cap_min ? "LITTLE core" : "mid core";
        }
    }
    return "core";
}

/* Highest cache level reported by any usable CPU. */
static int sys_max_level(void)
{
    static int max = -1;
    if (max >= 0)
        return max;
    max = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
        if (!CPU_ISSET(cpu, &nb_allowed))
            continue;
        for (int idx = 0; idx < 16; idx++) {
            char path[256], buf[64];
            snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cache/index%d/level", cpu, idx);
            if (!read_line(path, buf, sizeof buf))
                break;
            if (atoi(buf) > max)
                max = atoi(buf);
        }
    }
    return max;
}

/* vendor/family/model of the first CPU in /proc/cpuinfo. */
static void cpu_ident(char *vendor, size_t vn, int *family, int *model)
{
    static int init, fam = -1, mod = -1;
    static char ven[32];
    if (!init) {
        init = 1;
        FILE *f = open_sys("/proc/cpuinfo");
        if (f) {
            char line[512];
            while (fgets(line, sizeof line, f)) {
                char *v = strchr(line, ':');
                if (!v)
                    continue;
                v++;
                while (*v == ' ' || *v == '\t')
                    v++;
                v[strcspn(v, "\r\n")] = 0;
                if (!ven[0] && strncmp(line, "vendor_id", 9) == 0)
                    snprintf(ven, sizeof ven, "%s", v);
                else if (fam < 0 && strncmp(line, "cpu family", 10) == 0)
                    fam = atoi(v);
                else if (mod < 0 && strncmp(line, "model", 5) == 0 && (line[5] == ' ' || line[5] == '\t'))
                    mod = atoi(v);
                if (ven[0] && fam >= 0 && mod >= 0)
                    break;
            }
            fclose(f);
        }
    }
    snprintf(vendor, vn, "%s", ven);
    *family = fam;
    *model = mod;
}

/* Caches that CPUID leaf 4 (and so Linux) does not enumerate.
 *
 * Lion Cove and Cougar Cove P-cores have a 48 KiB first-level D-cache that
 * Intel calls L0, backed by a 192 KiB "L1" D-cache (~9 cycles), then L2.
 * Linux reports only the 48 KiB cache (as L1d) plus L2/L3, so the 192 KiB
 * level would be invisible. For those cores we renumber the 48 KiB cache to
 * L0 and insert the 192 KiB L1, matching Intel's naming. The E-cores
 * (Skymont/Darkmont) don't have this level and are left alone. */
static const struct {
    int model;
    const char *name;
} l0_models[] = {
    { 0xBD, "Lunar Lake (Lion Cove)" },
    { 0xC5, "Arrow Lake-H (Lion Cove)" },
    { 0xC6, "Arrow Lake-S (Lion Cove)" },
    { 0xCC, "Panther Lake (Cougar Cove)" },
};

static void apply_quirks(nb_cpu_t *out)
{
    char vendor[32];
    int family, model;
    cpu_ident(vendor, sizeof vendor, &family, &model);
    if (strcmp(vendor, "GenuineIntel") != 0 || family != 6)
        return;
    if (strcmp(out->kind, "P-core") != 0)
        return;
    bool match = false;
    for (size_t i = 0; i < sizeof l0_models / sizeof l0_models[0]; i++)
        if (l0_models[i].model == model)
            match = true;
    /* Only touch the layout we expect: 48K L1d followed by a >= 1 MiB L2. */
    if (!match || out->ncache < 2 || out->ncache >= NB_MAX_CACHES ||
        out->cache[0].level != 1 || out->cache[0].type != 'D' ||
        out->cache[0].size != 48 * NB_KiB || out->cache[1].level != 2 ||
        out->cache[1].size < NB_MiB)
        return;

    memmove(&out->cache[2], &out->cache[1], (size_t)(out->ncache - 1) * sizeof out->cache[0]);
    out->ncache++;
    out->cache[0].level = 0;                 /* the 48 KiB cache is Intel's L0 */
    out->cache[1] = out->cache[0];           /* private to the core, like L0 */
    out->cache[1].level = 1;
    out->cache[1].size = 192 * NB_KiB;
    out->cache[1].inferred = true;
}

static int cmp_cache(const void *a, const void *b)
{
    const nb_cache_t *x = a, *y = b;
    return x->level - y->level;
}

static void fallback_sysconf(nb_cpu_t *out)
{
    static const struct { int lvl, size, line; } q[] = {
#ifdef _SC_LEVEL1_DCACHE_SIZE
        { 1, _SC_LEVEL1_DCACHE_SIZE, _SC_LEVEL1_DCACHE_LINESIZE },
        { 2, _SC_LEVEL2_CACHE_SIZE, _SC_LEVEL2_CACHE_LINESIZE },
        { 3, _SC_LEVEL3_CACHE_SIZE, _SC_LEVEL3_CACHE_LINESIZE },
        { 4, _SC_LEVEL4_CACHE_SIZE, _SC_LEVEL4_CACHE_LINESIZE },
#endif
        { 0, 0, 0 }
    };
    for (int i = 0; q[i].lvl && out->ncache < NB_MAX_CACHES; i++) {
        long sz = sysconf(q[i].size), ln = sysconf(q[i].line);
        if (sz <= 0)
            continue;
        nb_cache_t *c = &out->cache[out->ncache++];
        c->level = q[i].lvl;
        c->type = q[i].lvl == 1 ? 'D' : 'U';
        c->size = (size_t)sz;
        c->line = ln > 0 ? (size_t)ln : 64;
        CPU_ZERO(&c->shared);
        if (q[i].lvl <= 2)
            CPU_SET(out->cpu, &c->shared);       /* assume private */
        else
            c->shared = nb_allowed;              /* assume shared by all */
        c->nshared = CPU_COUNT(&c->shared);
    }
}

static int topo_read(int cpu, nb_cpu_t *out, bool quirks);

int nb_topo_cpu(int cpu, nb_cpu_t *out) { return topo_read(cpu, out, true); }

unsigned nb_cpu_max_mhz(int cpu)
{
    char path[128], buf[64];
    snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
    return read_line(path, buf, sizeof buf) ? (unsigned)(strtoul(buf, NULL, 10) / 1000) : 0;
}

void nb_cpufreq_policy(char *buf, size_t n)
{
    char gov[64] = "", epp[64] = "";
    read_line("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", gov, sizeof gov);
    read_line("/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference", epp, sizeof epp);
    if (!gov[0])
        snprintf(buf, n, "unknown");
    else if (epp[0])
        snprintf(buf, n, "%s / %s", gov, epp);
    else
        snprintf(buf, n, "%s", gov);
}

static int topo_read(int cpu, nb_cpu_t *out, bool quirks)
{
    memset(out, 0, sizeof *out);
    out->cpu = cpu;
    out->kind = core_kind(cpu);

    for (int idx = 0; idx < 16 && out->ncache < NB_MAX_CACHES; idx++) {
        char path[256], buf[1024];
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cache/index%d/", cpu, idx);
        size_t plen = strlen(path);

        strcpy(path + plen, "level");
        if (!read_line(path, buf, sizeof buf))
            break;
        int level = atoi(buf);

        strcpy(path + plen, "type");
        if (!read_line(path, buf, sizeof buf) || strcmp(buf, "Instruction") == 0)
            continue;
        char type = buf[0] == 'D' ? 'D' : 'U';

        strcpy(path + plen, "size");
        if (!read_line(path, buf, sizeof buf))
            continue;
        size_t size = parse_sysfs_size(buf);
        if (!size)
            continue;

        nb_cache_t *c = &out->cache[out->ncache++];
        c->level = level;
        c->type = type;
        c->size = size;
        c->line = 64;
        strcpy(path + plen, "coherency_line_size");
        if (read_line(path, buf, sizeof buf) && atoi(buf) > 0)
            c->line = (size_t)atoi(buf);
        CPU_ZERO(&c->shared);
        strcpy(path + plen, "shared_cpu_list");
        if (!read_line(path, buf, sizeof buf) || nb_parse_cpulist(buf, &c->shared) <= 0)
            CPU_SET(cpu, &c->shared);
        c->nshared = CPU_COUNT(&c->shared);
    }

    if (out->ncache == 0 && !sys_root())
        fallback_sysconf(out);
    qsort(out->cache, (size_t)out->ncache, sizeof out->cache[0], cmp_cache);

    /* Low-power E-cores (Meteor/Lunar/Panther Lake LP islands) are listed
     * as cpu_atom like the regular E-cores but lack the shared LLC. */
    if (strcmp(out->kind, "E-core") == 0 && out->ncache > 0 &&
        out->cache[out->ncache - 1].level < sys_max_level())
        out->kind = "LP E-core";

    if (quirks)
        apply_quirks(out);
    return out->ncache;
}

void nb_cpu_model(char *buf, size_t n)
{
    snprintf(buf, n, "unknown CPU");
    FILE *f = open_sys("/proc/cpuinfo");
    if (!f)
        return;
    char line[512];
    long impl = -1, part = -1;
    bool found = false;
    while (fgets(line, sizeof line, f)) {
        /* aarch64 /proc/cpuinfo usually has no model name, only MIDR fields. */
        if (impl < 0 && strncmp(line, "CPU implementer", 15) == 0 && strchr(line, ':'))
            impl = strtol(strchr(line, ':') + 1, NULL, 0);
        if (part < 0 && strncmp(line, "CPU part", 8) == 0 && strchr(line, ':'))
            part = strtol(strchr(line, ':') + 1, NULL, 0);
        if (strncmp(line, "model name", 10) == 0 || strncmp(line, "Model", 5) == 0 ||
            strncmp(line, "Hardware", 8) == 0) {
            char *v = strchr(line, ':');
            if (v) {
                v++;
                while (*v == ' ' || *v == '\t')
                    v++;
                v[strcspn(v, "\r\n")] = 0;
                snprintf(buf, n, "%s", v);
                found = true;
                break;
            }
        }
    }
    fclose(f);
    if (found)
        return;

    /* Device-tree boards name the machine/SoC here. */
    char dt[160] = "";
    FILE *m = open_sys("/sys/firmware/devicetree/base/model");
    if (m) {
        size_t len = fread(dt, 1, sizeof dt - 1, m);
        dt[len] = 0;
        fclose(m);
    }
    if (dt[0] && impl >= 0)
        snprintf(buf, n, "%s (implementer 0x%02lx, part 0x%03lx)", dt, impl, part);
    else if (dt[0])
        snprintf(buf, n, "%s", dt);
    else if (impl >= 0)
        snprintf(buf, n, "CPU implementer 0x%02lx, part 0x%03lx", impl, part);
}
