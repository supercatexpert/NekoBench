#include "nekobench.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#define HUGE_PAGE ((size_t)2 << 20)

cpu_set_t nb_allowed;
volatile uint64_t nb_sink;
bool nb_huge_pages = true;

void nb_init(void)
{
    CPU_ZERO(&nb_allowed);
    if (sched_getaffinity(0, sizeof nb_allowed, &nb_allowed) != 0) {
        perror("sched_getaffinity");
        exit(1);
    }
}

uint64_t nb_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void nb_sleep_ms(unsigned ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        ;
}

int nb_pin_self(int cpu)
{
    cpu_set_t s;
    CPU_ZERO(&s);
    CPU_SET(cpu, &s);
    return pthread_setaffinity_np(pthread_self(), sizeof s, &s);
}

void nb_unpin_self(void)
{
    pthread_setaffinity_np(pthread_self(), sizeof nb_allowed, &nb_allowed);
}

static size_t round_up(size_t v, size_t a) { return (v + a - 1) / a * a; }

void *nb_alloc(size_t size)
{
    size_t len = round_up(size ? size : 1, HUGE_PAGE);
    size_t map = len + HUGE_PAGE;
    uint8_t *p = mmap(NULL, map, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return NULL;

    uint8_t *a = (uint8_t *)round_up((uintptr_t)p, HUGE_PAGE);
    if (a > p)
        munmap(p, (size_t)(a - p));
    size_t tail = (size_t)((p + map) - (a + len));
    if (tail)
        munmap(a + len, tail);
#if defined(MADV_HUGEPAGE) && defined(MADV_NOHUGEPAGE)
    madvise(a, len, nb_huge_pages ? MADV_HUGEPAGE : MADV_NOHUGEPAGE);
#endif
    memset(a, 0, len);  /* fault everything in now (on the calling CPU's node) */
    return a;
}

void nb_free(void *p, size_t size)
{
    if (p)
        munmap(p, round_up(size ? size : 1, HUGE_PAGE));
}

uint64_t nb_rand(uint64_t *s)
{
    /* xorshift64* */
    uint64_t x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 0x2545F4914F6CDD1Dull;
}

bool nb_parse_size(const char *s, size_t *out)
{
    char *end;
    errno = 0;
    double v = strtod(s, &end);
    if (errno || end == s || v <= 0)
        return false;
    while (isspace((unsigned char)*end))
        end++;
    double mul = 1;
    switch (toupper((unsigned char)*end)) {
    case 'K': mul = (double)NB_KiB; end++; break;
    case 'M': mul = (double)NB_MiB; end++; break;
    case 'G': mul = (double)NB_GiB; end++; break;
    case 'T': mul = (double)NB_GiB * 1024; end++; break;
    default: break;
    }
    if (*end == 'i' || *end == 'I')
        end++;
    if (*end == 'b' || *end == 'B')
        end++;
    if (*end || v * mul >= (double)SIZE_MAX)   /* e.g. "8T" on a 32-bit build */
        return false;
    *out = (size_t)(v * mul);
    return *out > 0;
}

int nb_parse_cpulist(const char *s, cpu_set_t *set)
{
    CPU_ZERO(set);
    const char *p = s;
    while (*p) {
        while (*p == ',' || isspace((unsigned char)*p))
            p++;
        if (!*p)
            break;
        char *end;
        long a = strtol(p, &end, 10), b = a;
        if (end == p || a < 0)
            return -1;
        p = end;
        if (*p == '-') {
            p++;
            b = strtol(p, &end, 10);
            if (end == p || b < a)
                return -1;
            p = end;
        }
        if (b >= CPU_SETSIZE)
            return -1;
        for (long i = a; i <= b; i++)
            CPU_SET((int)i, set);
        if (*p && *p != ',' && !isspace((unsigned char)*p))
            return -1;
    }
    return CPU_COUNT(set);
}

void nb_fmt_size(size_t size, char *buf, size_t n)
{
    static const char *const units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double v = (double)size;
    int u = 0;
    while (v >= 1024 && u < 4) {
        v /= 1024;
        u++;
    }
    if (v == (double)(uint64_t)v)
        snprintf(buf, n, "%.0f %s", v, units[u]);
    else
        snprintf(buf, n, "%.1f %s", v, units[u]);
}

void nb_fmt_cpulist(const cpu_set_t *set, char *buf, size_t n)
{
    size_t len = 0;
    buf[0] = 0;
    for (int i = 0; i < CPU_SETSIZE; i++) {
        if (!CPU_ISSET(i, set))
            continue;
        int j = i;
        while (j + 1 < CPU_SETSIZE && CPU_ISSET(j + 1, set))
            j++;
        int w = (j == i) ? snprintf(buf + len, n - len, "%s%d", len ? "," : "", i)
                         : snprintf(buf + len, n - len, "%s%d-%d", len ? "," : "", i, j);
        if (w < 0 || (size_t)w >= n - len)
            break;
        len += (size_t)w;
        i = j;
    }
}

int nb_cpus_in(const cpu_set_t *set, int *out, int max)
{
    int n = 0;
    for (int i = 0; i < CPU_SETSIZE && n < max; i++)
        if (CPU_ISSET(i, set))
            out[n++] = i;
    return n;
}

const char *nb_thp_mode(void)
{
    static char mode[32] = "unavailable";
    FILE *f = fopen("/sys/kernel/mm/transparent_hugepage/enabled", "r");
    if (!f)
        return mode;
    char line[128];
    if (fgets(line, sizeof line, f)) {
        char *a = strchr(line, '['), *b = a ? strchr(a, ']') : NULL;
        if (a && b && (size_t)(b - a - 1) < sizeof mode) {
            memcpy(mode, a + 1, (size_t)(b - a - 1));
            mode[b - a - 1] = 0;
        }
    }
    fclose(f);
    return mode;
}
