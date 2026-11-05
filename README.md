# NekoBench

A C benchmark for the memory hierarchy and the inter-core fabric:

* **Latency** of L1d, L2, L3, L4 (if present) and RAM: random pointer chase
* **Bandwidth** of every level: read / write / copy, plus non-temporal write for RAM.
  Measured single-threaded and as an all-core aggregate.
* **Core-to-core latency** matrix for every pair of logical CPUs
* Optional **sweep** of latency and read bandwidth from 4 KiB to the RAM working set
* Optional **HTML report** (`--html report.html`): one self-contained file with
  tables, interactive sweep charts, and a core-to-core heatmap. Light/dark themes.

Linux only: it reads cache topology from sysfs and pins threads with
`pthread_setaffinity_np`. See [Platforms](#platforms) for CPU architectures.

## Build

```sh
meson setup build              # release build, -march=native by default
ninja -C build
./build/nekobench
```

`meson setup build -Dnative=false` builds a portable binary (SSE2 baseline on x86-64).

## Platforms

Any Linux with GCC or Clang. Windows and macOS are not supported, because
NekoBench depends on Linux affinity, sysfs and THP interfaces.

| Architecture | Read kernel | Write / copy | NT stores | Status |
|---|---|---|---|---|
| x86-64 | inline asm, pure loads (SSE2 / AVX / AVX-512) | native-width vectors | MOVNT | tested on hardware |
| i686 | portable C vectors (SSE2 with `-march`) | C vectors | MOVNT with SSE2 | cross-built, run under qemu |
| aarch64 | inline asm, `ldp q` pure loads | C vectors (NEON) | `STNP` | cross-built, run under qemu |
| armv7 (hf), riscv64, others | portable C vectors | C vectors | none | cross-built, run under qemu |

"Run under qemu" means every mode (latency, bandwidth, multi-thread,
core-to-core, sweep, `--aida`, HTML) was exercised. Emulated *numbers* are
meaningless, so these targets still need a check on real hardware.

Notes per platform:

* **i686:** Debian's `i686` baseline has no SSE2, so a cross build with
  `-Dnative=false` uses scalar code. Building on the machine itself
  (`-Dnative=true`, the default) enables whatever `-march=native` finds.
  For a portable SSE2 binary, use `-Dc_args=-msse2`. 32-bit address space
  limits the RAM working set, and `--aida` with many threads may need a
  smaller `-m`.
* **ARM big.LITTLE / DynamIQ:** core types come from Linux's
  `cpu_capacity` and are labelled big, mid or LITTLE core. One CPU per type
  gets the single-thread tests. The CPU name comes from the device tree
  model, or the MIDR implementer/part fields in `/proc/cpuinfo`.
* **Missing cache info:** some ARM boards describe no caches to Linux, so
  `/sys/.../cache` is empty. NekoBench then falls back to `sysconf`, which
  glibc often can't answer on ARM. In that case only the RAM row is shown,
  and `-t sweep` still reveals the cache levels as steps in the curve.
* **Frequency probe:** the dependent-add chain is architecture-neutral (1
  add/cycle on all mainstream cores), so cycle counts work everywhere.

Cross-compiling (cross files for aarch64, i686, armhf and riscv64 are in
`cross/`, using Debian's `gcc-<triple>` packages and `qemu-user`):

```sh
meson setup build-aarch64 --cross-file cross/aarch64-linux-gnu.txt -Dnative=false
ninja -C build-aarch64
qemu-aarch64 -L /usr/aarch64-linux-gnu build-aarch64/nekobench -q   # smoke test
```

## Usage

```
nekobench [options]
  -t, --tests LIST     latency, bandwidth, mt, c2c, sweep, all  (default: latency,bandwidth,mt,c2c)
  -c, --cpu N          single-thread tests on CPU N only (default: one CPU per core type)
  -C, --cpus LIST      CPUs for multi-thread and c2c tests, e.g. 0-7,16
  -m, --mem-size SIZE  RAM working set (default: max(1G, 32 x LLC))
  -S, --sweep-max SIZE largest working set in the sweep (default: mem-size)
  -d, --duration MS    one bandwidth trial (default 200)
  -r, --repeat N       trials per measurement; the best is kept (default 3)
  -q, --quick          shorter trials, fewer c2c samples
  -o, --html FILE      also write a self-contained HTML report
      --aida           AIDA64 Cache & Memory Benchmark-style grid (runs alone
                       unless --tests is also given)
      --aida-lat-block SIZE|random  --aida memory latency pattern
                       (default: 64M blocks, block-random)
      --aida-huge-pages  --aida memory bandwidth on 2 MiB instead of 4 KiB pages
```

`--tests memlat` (not part of `all`) prints memory latency for block-random
patterns from 4 KiB blocks up to fully random. It shows how much prefetchers
shorten "latency" when accesses stay inside a region, and how to pick
`--aida-lat-block`.

Examples:

```sh
./build/nekobench -q                       # everything except the sweep, fast (~45 s)
./build/nekobench -t sweep -c 0 -S 256M    # latency/bandwidth curve on CPU 0
./build/nekobench -t c2c -C 0-15           # core-to-core matrix for P-cores only
./build/nekobench -t all -o report.html    # everything, plus an HTML report
./build/nekobench --aida                   # AIDA64-style grid, ~15 s
```

## AIDA64-compatible mode

`--aida` prints a grid laid out like AIDA64's Cache & Memory Benchmark, for
side-by-side comparison with Windows results:

```
                        Read          Write           Copy     Latency
   Memory         88921 MB/s     85524 MB/s     85168 MB/s     73.4 ns
   L1 Cache      4549.5 GB/s    3397.1 GB/s    6922.2 GB/s      1.0 ns
   ...
```

What follows AIDA64's documented behaviour (developer posts on the AIDA64 forum):

* Read/write/copy run multi-threaded on all CPUs (`--cpus` to restrict).
* Copy is scored as bytes read + bytes written (AIDA64: 64 MB / time for a
  32 MB → 32 MB copy). Each memory thread uses 64 MiB (32 → 32 MiB copy).
* Memory latency is "block-random": random order inside a block, then on to
  the next block.
* Memory bandwidth uses 4 KiB pages, as AIDA64 documents for its memory
  tests. On a 14700K this made no measurable difference compared to 2 MiB
  pages; `--aida-huge-pages` lets you compare.
* Cache rows group by *position*, matching AIDA64 on Panther Lake: the "L0"
  row is every core's first-level D-cache (P-core L0 plus E-core L1d), and
  the "L1" row is the P-cores' 192 KiB L1 only.

NekoBench's own choices, where AIDA64 doesn't document its method:

* Memory write and copy use non-temporal stores. With regular stores, RAM
  writes also read every line first (read-for-ownership), which roughly
  halves the score.
* Memory latency uses 64 MiB blocks. AIDA64 doesn't publish its block size.
  On a Core Ultra X7 358H, `--tests memlat` gave 93.7 ns at 64 MiB against
  AIDA64's 93.1 ns. Smaller blocks read far lower because the prefetchers
  cover the region (45 ns at 1 MiB, 21 ns at 4 KiB), and fully random gave
  106 ns. Recalibrate on other CPUs with `--tests memlat` and
  `--aida-lat-block`.
* Latency runs first, on the fastest-boosting CPU of the first core type,
  before the all-core bandwidth runs heat the chip. On a laptop those runs
  pulled the single-core clock from ~4.7 to 4.3 GHz. The clock is measured
  right after the latency runs, and latency is also printed in cycles.
  **Compare latency across tools in cycles**: on the 358H, NekoBench and
  AIDA64 agree on L0/L1 latency in cycles (3.9 vs 4.2, 9.0 vs 8.9) while
  the ns figures differ because the clocks differ.
* MB/s and GB/s are decimal (10^6 / 10^9). If AIDA64 uses 2^20, its numbers
  read ~5% lower.
* L2/L3 working sets are NekoBench's (see Methodology). AIDA64's appear to
  be larger: on the 358H its L2 read equals its L3 read, which suggests it
  spills into L3. Expect the L2/L3 rows to differ most.

Expect results in the same ballpark, not identical. Both tools are sensitive
to background load, boost clocks and memory training.

## Boost clocks

Single-thread tests run on the CPU of each core type with the highest
`cpuinfo_max_freq`, i.e. the "favored" core on Turbo Boost Max 3.0 parts.
The measured clock is printed next to that maximum, with a warning when it's
more than 8% below. Busy cores lower the turbo ratio, and so do power and
thermal limits. Latency in **cycles** is converted with the clock measured on
the same core, so it is unaffected. Latency in ns and L1/L2 bandwidth scale
with the clock, and RAM results barely change.

## HTML report

`--html FILE` writes one file with no external resources, so it can be
opened offline, attached, or archived. The text output on stdout is
unchanged. The report contains:

* system info and the cache topology for each core type
* single-thread and multi-thread tables, with data bars scaled to each column
* the sweep (with `-t sweep`/`all`) as log-log latency and bandwidth charts.
  Dashed lines mark cache sizes. A hover/keyboard crosshair shows exact
  values, and a table view is included.
* the core-to-core matrix as a heatmap. Hovering a cell shows the pair and
  its category (SMT sibling, same L2 cluster, other).

Every number is in a table, so the page is fully readable without JavaScript.
The script only adds the charts, tooltips and the theme toggle. CSS and JS are
in `src/assets/`. At build time they are embedded into the binary by
`tools/embed.py`.

For stable numbers, close other workloads. If you can, also set the
`performance` governor and disable SMT siblings you don't want in the
multi-thread runs (via `--cpus`).

## Methodology

**Topology.** Cache levels, sizes and sharing come from
`/sys/devices/system/cpu/cpuN/cache/` (or `sysconf` as a fallback). An L4
(e.g. Crystalwell eDRAM) shows up there as level 4 and is benchmarked like
any other level. CPUs are grouped by core type and cache layout. Hybrid parts
(Intel P-/E-cores, detected via `/sys/devices/cpu_core` and `cpu_atom`) get one
single-thread run per core type.

**Caches the OS doesn't report.** Lion Cove and Cougar Cove P-cores (Lunar
Lake, Arrow Lake, Panther Lake) have a 48 KiB first-level D-cache, which Intel
calls **L0**, backed by a **192 KiB L1** D-cache. CPUID leaf 4, and therefore
Linux, enumerates only the 48 KiB cache (as "L1d") plus L2/L3, so the 192 KiB
level would be invisible. For these models (from `/proc/cpuinfo`), NekoBench
renames the 48 KiB cache to L0d and inserts the 192 KiB L1d, using Intel's
names. Inserted levels are flagged in the text output and starred in the HTML
report. The insertion only happens when the reported layout matches the
expected one. Low-power E-cores that lack the shared L3 are labelled
"LP E-core".

In the multi-thread table, a cache row runs only on CPUs that have that
level, so LP E-cores sit out the L3 row. The first row is each CPU's
*first-level* D-cache (e.g. "L0d/L1d" on Panther Lake). Intel reuses the
name "L1" for different positions in the hierarchy, so the first level is
grouped by position rather than by name.

**Working sets.** L1 is tested at half its size. Level *n* is tested at the
geometric midpoint between the sum of the levels below it and its own size.
Caches are treated as non-inclusive, and random-access hit rates fall off well
before a cache is "full", so this keeps away from both edges. For the
multi-thread runs, a shared cache's capacity is split between the threads of
the run that share it. Buffers are 2 MiB-aligned, `madvise(MADV_HUGEPAGE)`'d
and pre-faulted, so with THP the measurements are almost free of TLB misses.

**Latency.** Every 64 B line of the buffer goes into one random cyclic
permutation (Sattolo's algorithm), and the chain is followed with dependent
loads (`p = *p`). This defeats the prefetchers. The result is ns per load,
best of N trials. It is converted to cycles using a clock measured on the same
core: a chain of dependent register adds, which runs at 1 add/cycle.

**Bandwidth.** Each kernel streams over its buffer in 512 B blocks using the
widest SIMD available. On x86 the read kernel is pure loads in inline asm:
adding even one ALU op per load holds Golden Cove to ~2 loads/cycle instead
of 3. Write and copy kernels contain a compiler barrier so they can't be
turned into `memset`/`memcpy`. The copy destination is skewed by 2 KiB to
avoid 4K aliasing. Each thread runs its kernel until a stop flag is raised
after the trial duration and times itself. Aggregate GB/s is the sum of
per-thread `bytes / time`, which stays correct when the cores run at
different speeds.

* GB/s is 10^9 bytes/s of program-visible traffic.
* Copy counts bytes read + bytes written.
* Regular writes to RAM also cause read-for-ownership traffic that is not counted.
  NT-Write avoids it, which is why it is faster.

**Core-to-core latency.** Two threads pinned to CPUs *a* and *b* bounce a
sequence number through one cache line (128 B-aligned). Every round trip moves
the line a→b→a, and the matrix shows one-way latency = RTT / 2. The spin loops
use no `PAUSE`, because it costs 100+ cycles on recent Intel cores. Each value
is the best of several samples, and each sample averages hundreds of round
trips, so samples hit by preemption are filtered out. The summary groups pairs
into SMT siblings, same-L2 cluster, and others.

## Replaying another machine's topology

`NEKOBENCH_SYSFS_ROOT=/dir` makes NekoBench read `/sys/...` and
`/proc/cpuinfo` from `/dir/sys/...` and `/dir/proc/cpuinfo`. This tests the
topology handling (core types, inserted levels, working-set sizing, table
rows) against a dump from a machine you don't have. The measurements still
run on the local CPUs, so restrict them to matching CPU numbers, e.g.
`taskset -c 0-15`.

## Source layout

| File | Purpose |
|------|---------|
| `src/main.c` | CLI, core-type detection, working-set sizing, reports |
| `src/topology.c` | sysfs/sysconf cache topology, CPU model |
| `src/latency.c` | pointer chase, clock measurement |
| `src/bandwidth.c` | SIMD kernels, pinned thread-group runner |
| `src/c2c.c` | cache-line ping-pong |
| `src/util.c` | timing, pinning, huge-page allocation, parsing/formatting |
| `src/report.h`, `src/html.c` | in-memory results and the HTML renderer |
| `src/assets/report.{css,js}` | report styling, charts and tooltips (embedded at build time) |
| `tools/embed.py` | turns the assets into a C header (meson `custom_target`) |
