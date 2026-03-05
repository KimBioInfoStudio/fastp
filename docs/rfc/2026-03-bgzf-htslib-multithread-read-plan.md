# RFC: BGZF-only htslib Multi-thread Read Plan

**Date:** 2026-03-05
**Status:** In Progress
**Branch:** main

## Context

Current gzip input path is based on single-thread inflate in `FastqReader`. For BGZF-compressed input, we can leverage `htslib`'s BGZF multi-thread read capability.

This RFC proposes:
- only when input is confirmed BGZF, use htslib multi-thread reading
- non-BGZF gzip keeps current path unchanged
- isolate implementation into dedicated `.cc/.h` files

## Scope

1. Detect BGZF format at reader init.
2. Route BGZF input to htslib multi-thread reader.
3. Keep existing parser logic and output behavior unchanged.
4. Keep fallback path for non-BGZF gzip and read errors.

## Non-Goals

1. Replacing all gzip handling with htslib.
2. Refactoring FASTQ record parser architecture.
3. Adding random-access/query features.

## Design Overview

### Routing Strategy

1. Input is plain FASTQ: existing `fread` path.
2. Input is `.gz` but not BGZF: existing igzip path.
3. Input is BGZF: use new htslib BGZF multi-thread reader.

### BGZF Detection

At init stage, inspect gzip header and extra field:
- gzip magic (`1f 8b`)
- FEXTRA flag set
- extra subfield `BC` exists (BGZF marker)

If detection fails or parsing is uncertain, treat as non-BGZF and use current path.

## File-level Implementation Plan

### 1) New BGZF Reader Adapter (separate .h/.cc)

New files:
- `src/bgzf_reader.h`
- `src/bgzf_reader.cc`

Responsibilities:
1. Open/close BGZF stream using htslib APIs.
2. Configure multi-thread read count.
3. Provide chunk-based sequential read interface for `FastqReader`.
4. Report compressed bytes read for progress API compatibility.

Proposed interface:

```cpp
class BgzfReader {
public:
    BgzfReader();
    ~BgzfReader();

    bool open(const std::string& filename, int threads);
    void close();

    // Read uncompressed bytes into buf; returns bytes read, 0 on EOF, <0 on error
    int read(char* buf, int cap);

    // For progress reporting compatibility
    size_t compressedBytesRead() const;

    std::string lastError() const;
};
```

### 2) New BGZF Detect Utility (separate .h/.cc)

New files:
- `src/bgzf_detect.h`
- `src/bgzf_detect.cc`

Responsibilities:
1. Lightweight BGZF signature check on file header.
2. Avoid full decompression during probe.
3. Return deterministic yes/no + diagnostic reason for debug logs.

Proposed API:

```cpp
struct BgzfProbeResult {
    bool isBgzf;
    std::string reason;
};

BgzfProbeResult probeBgzfFile(const std::string& filename);
```

### 3) FastqReader Integration

Modified files:
- `src/fastqreader.h`
- `src/fastqreader.cpp`

Changes:
1. Add mode enum:
   - `Plain`
   - `GzipIsal`
   - `BgzfHtslib`
2. Add member:
   - `BgzfReader* mBgzfReader;`
3. In `init()`:
   - if `.gz`, run BGZF probe
   - BGZF => initialize `BgzfReader`
   - otherwise keep current igzip init
4. In `readToBuf()`:
   - dispatch to `readToBufBgzf()` for BGZF mode
   - keep `readToBufIgzip()` unchanged for non-BGZF gzip
5. In `getBytes()`:
   - BGZF mode reads progress from `BgzfReader::compressedBytesRead()`

### 4) Option and CLI Wiring

Decision updated (2026-03-05):
1. Do not expose BGZF tuning knobs via CLI.
2. Keep BGZF thread split in internal auto mode only.
3. Keep fallback behavior automatic; no user-facing disable switch.

Current behavior:
1. `FastqReader` receives total worker budget from processor.
2. BGZF threads use internal heuristic: `clamp(1, 8, total/3)`.
3. When BGZF path is unavailable, auto-fallback to igzip path.

### 5) Build System Changes

Likely modified:
- `Makefile` and/or cmake files in repo

Tasks:
1. add optional htslib include/lib linkage
2. compile `bgzf_reader.cc` and `bgzf_detect.cc`
3. gate with macro, e.g. `FASTP_HAS_HTSLIB`

Fallback behavior:
- if not linked with htslib, BGZF route is disabled at compile time and code automatically uses igzip path

## Runtime Behavior

1. Default behavior remains backward-compatible.
2. BGZF + htslib available + not disabled => multi-thread BGZF read enabled.
3. Any BGZF adapter init/read error => log and fallback to existing gzip path when possible; fatal only when fallback is impossible.

## Implementation Snapshot (2026-03-05)

### Implemented

1. BGZF probe utility added:
   - `src/bgzf_detect.h`
   - `src/bgzf_detect.cc`
2. BGZF reader adapter added:
   - `src/bgzf_reader.h`
   - `src/bgzf_reader.cc`
3. `FastqReader` routing integrated:
   - `.gz` input probes BGZF marker.
   - BGZF routes to htslib reader.
   - non-BGZF `.gz` keeps igzip path unchanged.
4. Worker thread budget is passed from processors to `FastqReader`.
5. Build system supports optional htslib and static `libhts.a` linking.
6. No BGZF-specific CLI parameters are exposed.
7. BGZF reader uses producer/consumer chunk queue (V2 style).
8. Backlog-aware runtime autotune code path exists but is disabled by default (fixed thread mode is default).
9. Experimental enable switch: set environment variable `FASTP_BGZF_AUTOTUNE=1`.

### Not Yet Implemented

1. Shared `hts_tpool` reused across R1/R2 readers (current code uses `bgzf_mt` per handle).
2. Shared `hts_tpool` reused across R1/R2 readers (current implementation keeps per-reader htslib BGZF handle model).
3. Full robustness test matrix in repo (truncated/malformed/concatenated BGZF).
4. Dedicated BGZF benchmark report in `docs/bench` matching RFC matrix.

### Current Benchmark Evidence

10M pairs, PE, `-w 10`, single run:

| mode | time(s) | throughput (M pairs/s) | peak RSS |
|---|---:|---:|---:|
| gz->gz | 11.24 | 0.89 | 1242 MB |
| bgzf->gz | 9.77 | 1.02 | 1379 MB |

Observed:
1. `bgzf->gz` is about 13.1% faster than `gz->gz` in this setup.
2. BGZF path uses about 11% more peak memory.

## Implementation Checklist

### Must (merge gate)

- [x] BGZF probe utility implemented (`src/bgzf_detect.h/.cc`)
- [x] BGZF reader adapter implemented (`src/bgzf_reader.h/.cc`)
- [x] `FastqReader` routes BGZF -> htslib, non-BGZF `.gz` -> igzip
- [x] BGZF thread split is internal auto mode only (no public CLI knobs)
- [x] Build supports static `libhts.a` linking when available
- [x] CI builds htslib from source as static lib and builds fastp with `HTSLIB_DIR`
- [x] 10M PE e2e run completed for `gz->gz` and `bgzf->gz`
- [x] Output correctness checked (md5 generated for outputs)

### Should (next iteration)

- [ ] Replace per-handle `bgzf_mt` with shared `hts_tpool` across R1/R2
- [x] Runtime autotune loop for BGZF thread split implemented (internal), but disabled by default pending stronger benchmark win
- [x] Robustness tests: truncated BGZF / malformed extra / concatenated BGZF members (probe-level unit test in `FastqReader::test`)
- [ ] Compatibility tests for interleaved BGZF input path
- [ ] Add dedicated BGZF benchmark report in `docs/bench/` (thread grid + interpretation)

### Should Prioritization (Risk + Effort)

| Item | Priority | Risk if delayed | Estimated effort | Suggested order |
|---|---|---|---|---|
| Shared `hts_tpool` across R1/R2 | P1 | Medium: extra BGZF thread overhead, weaker scaling under PE | 0.5-1.5 day | 1 |
| Runtime BGZF autotune | Done (disabled by default) | Keep as experimental path; re-enable only with clear benchmark gain | done | done |
| Robustness tests (truncated/malformed/concatenated) | P0 | High: silent parsing/decompression regressions may escape CI | 0.5-1 day | 2 |
| Interleaved BGZF compatibility tests | P1 | Medium: behavior gap may appear only in less common input mode | 0.5 day | 3 |
| Dedicated BGZF bench report | P1 | Medium: optimization claims hard to verify/regress over time | 0.5 day | 5 |

### Validation Commands and Artifacts

Commands:
1. `make -j4 HTSLIB_DIR=/tmp/htslib-1.20`
2. `python3 scripts/bench_e2e.py --mode gz-gz --pairs 10000000 --runs 1 --opt ./fastp`
3. `./fastp -w 10 -i <R1.bgzf.fq.gz> -I <R2.bgzf.fq.gz> -o <out1.gz> -O <out2.gz> -j <out.json> -h <out.html>`

Artifacts:
1. `/tmp/fastp_benchmark/results.json`
2. `/tmp/fastp_e2e_10m_bgzf/out.json`
3. `/tmp/fastp_e2e_10m_bgzf/out.html`

## Test Plan

### Functional

1. plain `.fq` unchanged.
2. non-BGZF `.fq.gz` unchanged.
3. BGZF `.fq.gz` output identical to baseline.

### Robustness

1. truncated BGZF.
2. malformed gzip extra field.
3. concatenated BGZF members.

### Compatibility

1. SE mode.
2. PE mode.
3. interleaved input.

### Performance

1. BGZF input benchmark at threads 1/2/4/8/16.
2. verify monotonic throughput scaling and end-to-end wall-time gain.

## Rollout Plan

1. PR-1: add BGZF probe + unit tests.
2. PR-2: add `BgzfReader` adapter and compile guards.
3. PR-3: integrate with `FastqReader` routing.
4. PR-4: CLI switches + docs + benchmark report.
5. PR-5: enable by default for BGZF when htslib is present.

## Acceptance Criteria

1. Only BGZF inputs route to htslib path.
2. Non-BGZF inputs keep existing behavior.
3. BGZF path supports multi-thread read and passes correctness tests.
4. Build works both with and without htslib.
5. New code is isolated in dedicated `.cc/.h` files as planned.

## Decisions from Discussion (2026-03-05)

### 1) Reusing Worker Threads with BGZF-htslib

Decision:
1. Do **not** directly reuse fastp processing worker threads for BGZF decompression.
2. Reuse via **htslib shared thread pool** (`hts_tpool`) across BGZF readers instead.
3. Keep fastp processing workers independent; only coordinate by thread budget.

Reason:
- Directly sharing processing workers with decompression risks starvation and unstable throughput.
- `htslib` already supports a dedicated shared pool model for BGZF read tasks.

Implementation note:
- Create one `hts_tpool` per process.
- Attach R1/R2 BGZF handles to the same `htsThreadPool`.
- Destroy pool on reader shutdown.

### 2) Runtime Auto-tuning for Thread Split

Decision:
1. Keep auto-tuning as internal mode only (no external parameter).
2. Replace static heuristic (`bgzf=min(4,total/3)`) with runtime adaptive control.

Auto-tuning plan:
1. Warmup probe (first 2-5 seconds): evaluate candidate BGZF thread counts (e.g. 1,2,4,min(8,total/2)).
2. Select initial value by maximizing end-to-end throughput while avoiding worker starvation.
3. Runtime adjustment every 1-2 seconds with small step (`+1/-1`) and hysteresis.

Control signals:
1. Decompression throughput (MB/s).
2. Parser/worker queue occupancy.
3. Worker idle ratio.

Stability constraints:
1. Thread changes have cooldown (3-5 seconds).
2. Bounds: `1 <= bgzf_threads <= min(8, total-1)`.
3. Increase only if sustained benefit >5%; decrease after sustained degradation windows.

### 3) How Decompressed Chunks Are Consumed by fastp Parser

Decision:
1. V1 uses a simple pull path; no flight-batch scheduler at first.
2. V2 may add lightweight prefetch queue only if V1 shows parser starvation.

V1 (default):
1. `FastqReader::readToBufBgzf()` calls `BgzfReader::read(buf, cap)` directly.
2. Existing `getLine()/read()` parser consumes `mFastqBuf` unchanged.
3. htslib internal BGZF threads handle decompression parallelism.

V2 (optional optimization):
1. Add one producer (BGZF read) + one consumer (parser) SPSC ring.
2. Fixed chunk pool + inflight byte backpressure.
3. Keep design much simpler than writer-side flight-batch logic.

Default tuning starting point for V2:
1. `chunk_size=2MB`
2. `queue_depth=16`
3. inflight target around ~32MB

### 4) gzi Usage Decision

Decision:
1. `gzi` is **not required** for this RFC path.
2. For sequential FASTQ consumption, BGZF streaming read is sufficient.

Reason:
- `gzi` is mainly useful for random seek/indexed jumps, not required for linear parser consumption.

## Updated Rollout (with above decisions)

1. PR-1: BGZF probe + tests.
2. PR-2: `BgzfReader` + shared `hts_tpool` wiring + compile guards.
3. PR-3: `FastqReader` V1 pull integration (no extra queue).
4. PR-4: internal runtime autotune + metrics logging (no CLI exposure).
5. PR-5: optional V2 prefetch queue (only if profiling proves benefit).
