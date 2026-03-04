# RFC: Adaptive Timeout for In-Flight Gzip Batch Compression (Internal Only)

## Status
Draft — flight batching + pwrite implemented; adaptive timeout pending.

## Summary
Add an internal adaptive timeout to in-flight `.gz` batch compression so flush condition becomes:

`flush when (accumulated_bytes >= batch_size) OR (elapsed_since_last_flush >= adaptive_timeout)`

No new CLI flags or user-facing options.

## Motivation
Current behavior flushes only on fixed batch size (plus final completion). Under low/irregular throughput, buffered data may wait too long, increasing tail latency.
Adaptive timeout reduces latency while retaining throughput benefits of batching.

## Goals
- Keep existing batch-size optimization.
- Improve latency for sparse/uneven input.
- Auto-tune per thread from real runtime behavior.
- No user-facing configuration changes.

## Non-Goals
- Changing compression level mapping.
- Exposing timeout/batch controls in CLI.
- Reworking writer architecture.

---

## Current Architecture

### Two output modes

WriterThread operates in one of two modes, selected at construction:

| Condition | Mode | Data flow |
|---|---|---|
| Multi-thread + file output | **pwrite mode** | Workers → `pwrite()` directly to fd |
| Single-thread / STDOUT | **legacy mode** | Workers → SPSC queues → writer thread → `Writer` |

Mode is set in `WriterThread()` (`writerthread.cpp:32`):
```cpp
mPwriteMode = !isSTDOUT && mOptions->thread > 1;
```

### Data flow — pwrite mode (multi-thread `.gz` output)

```
Worker 0                Worker 1                Worker 2
   │                       │                       │
   ▼                       ▼                       ▼
input(tid=0, data)     input(tid=1, data)     input(tid=2, data)
   │                       │                       │
   ▼                       ▼                       ▼
┌──────────────┐   ┌──────────────┐   ┌──────────────┐
│ mAccumBuf[0] │   │ mAccumBuf[1] │   │ mAccumBuf[2] │
│ (append raw) │   │ (append raw) │   │ (append raw) │
└──────┬───────┘   └──────┬───────┘   └──────┬───────┘
       │ >= 512KB?        │ >= 512KB?        │ >= 512KB?
       ▼                  ▼                  ▼
  isal_gzip_compress  isal_gzip_compress  isal_gzip_compress
       │                  │                  │
       ▼                  ▼                  ▼
  spin-wait prev      spin-wait prev     spin-wait prev
  seq offset          seq offset         seq offset
       │                  │                  │
       ▼                  ▼                  ▼
  pwrite(fd, data,    pwrite(fd, data,   pwrite(fd, data,
    offset)             offset)            offset)
       │                  │                  │
       ▼                  ▼                  ▼
  publish offset      publish offset     publish offset
  to OffsetRing       to OffsetRing      to OffsetRing
```

### Data flow — legacy mode (single-thread / STDOUT `.gz` output)

```
Worker 0                Worker 1
   │                       │
   ▼                       ▼
input(tid=0, data)     input(tid=1, data)
   │                       │
   ▼                       ▼
┌──────────────┐   ┌──────────────┐
│ mAccumBuf[0] │   │ mAccumBuf[1] │
│ (append raw) │   │ (append raw) │
└──────┬───────┘   └──────┬───────┘
       │ >= 512KB?        │ >= 512KB?
       ▼                  ▼
  isal_gzip_compress  isal_gzip_compress
       │                  │
       ▼                  ▼
  SPSC queue[0]       SPSC queue[1]
       │                  │
       └────────┬─────────┘
                ▼
         Writer thread
        (round-robin
         consume from
         queue[0],[1],...)
                │
                ▼
         Writer::write()
```

### Sequence coordination (pwrite mode)

Workers determine write order via interleaved sequence numbering:

```
Thread count = 3

Worker 0 sequences: 0, 3, 6, 9, ...
Worker 1 sequences: 1, 4, 7, 10, ...
Worker 2 sequences: 2, 5, 8, 11, ...

OffsetRing (ring buffer, size 512):
┌─────────┬─────────┬─────────┬─────────┐
│ slot 0  │ slot 1  │ slot 2  │ slot 3  │ ...
│ seq=0   │ seq=1   │ seq=2   │ seq=3   │
│ off=1200│ off=2400│ off=3600│ off=4800│
└─────────┴─────────┴─────────┴─────────┘

Each slot stores:
  - cumulative_offset: file position after this write
  - published_seq: confirms which sequence owns this slot

Writer for seq N:
  1. spin-wait until slot[(N-1) % 512].published_seq == N-1
  2. read prev cumulative_offset as its write position
  3. pwrite(fd, data, offset)
  4. publish slot[N % 512] = { offset + wsize, N }
```

The `OffsetSlot` struct is cache-line aligned (`alignas(64)`) to prevent false sharing between workers spinning on adjacent slots.

### Zero-size passthrough

When a `.gz` accumulation buffer hasn't reached the batch threshold, `inputPwrite()` still participates in the sequence protocol — it publishes the same offset it received, allowing downstream sequences to proceed without stalling. This maintains ordering correctness even when a worker has no data to write.

### Key constants

| Constant | Value | Location | Purpose |
|---|---|---|---|
| `ISAL_BATCH_SIZE` | 512 KB | `writerthread.cpp:8` | Raw data accumulation threshold before compression |
| `OFFSET_RING_SIZE` | 512 | `writerthread.h:16` | Slots in offset coordination ring buffer |
| ISA-L levels | 0-3 | `writerthread.cpp:21-29` | Mapped from fastp compression 1-9 |

ISA-L level mapping:
```
fastp 1-2  → ISA-L 0 (fastest)
fastp 3-5  → ISA-L 1 (default)
fastp 6-7  → ISA-L 2
fastp 8-9  → ISA-L 3 (best compression)
```

---

## Proposal: Adaptive Timeout

### Flush policy
Per thread, maintain:
- `accum_buf` — already exists as `mAccumBuf[tid]`
- `last_input_ts` — timestamp of last `input()` call
- `last_flush_ts` — timestamp of last compression flush
- `ingress_bps_ema` — exponential moving average of input rate (bytes/sec)
- `dynamic_timeout_us` — computed adaptive timeout (microseconds)

Trigger flush when:
1. `accum_buf.size() >= ISAL_BATCH_SIZE` — (existing, unchanged)
2. `now - last_flush_ts >= dynamic_timeout_us` **and** `accum_buf` not empty — (new)

### Adaptive tuning
Update on each `input()` call:
1. Measure instantaneous ingress rate from `data->size() / (now - last_input_ts)`.
2. Update EMA:
   `ingress_bps_ema = 0.8 * old + 0.2 * instant`
3. Compute target timeout to approximately fill one batch:
   `target_us = ISAL_BATCH_SIZE / max(ingress_bps_ema, 1.0) * 1e6`
4. Clamp to bounds: `min_timeout_us <= timeout <= max_timeout_us`
5. Apply smoothing to timeout to avoid oscillation.

### Timing mechanism

For both code paths (`input()` and `inputPwrite()`), the timeout check uses `std::chrono::steady_clock`. The check happens at the start of each `input()` call, before the size-threshold check:

```cpp
auto now = std::chrono::steady_clock::now();
auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
    now - mLastFlushTs[tid]).count();

// Time-triggered flush (before accumulating new data)
if (elapsed_us >= mDynamicTimeoutUs[tid] && !mAccumBuf[tid].empty()) {
    // compress + flush mAccumBuf[tid]
    // update mLastFlushTs[tid] = now
}

// Then accumulate new data and check size threshold (existing logic)
mAccumBuf[tid].append(data->data(), data->length());
if (mAccumBuf[tid].size() >= ISAL_BATCH_SIZE) {
    // compress + flush
    // update mLastFlushTs[tid] = now
}
```

This design keeps the hot path (size-only check) nearly unchanged. The `steady_clock::now()` call adds ~20-25ns overhead per `input()`.

### Optional pressure term
- If output backlog is high (many offset slots awaiting previous writes), shrink timeout → faster flush → smaller compressed blocks → less spin-wait time for downstream sequences.

## Why this works
- **High ingress**: timeout naturally drops below fill time; size-trigger dominates. No behavioral change on the throughput-critical path.
- **Low ingress**: timeout caps waiting time; improves latency. Small tail flushes are compressed individually (slight compression ratio loss, acceptable for responsiveness).
- **EMA smoothing**: avoids jitter and excessive tiny flushes from bursty input.
- **Zero overhead when disabled**: compile-time constant `ADAPTIVE_TIMEOUT_ENABLED` can gate the feature for benchmarking.

---

## Implementation scope

### New per-thread state (`writerthread.h`)
```cpp
// Adaptive timeout state (per worker thread)
std::chrono::steady_clock::time_point* mLastInputTs;
std::chrono::steady_clock::time_point* mLastFlushTs;
double* mIngressBpsEma;
int64_t* mDynamicTimeoutUs;
```

### Files changed

| File | Change |
|---|---|
| `src/writerthread.h` | Add per-thread adaptive state arrays |
| `src/writerthread.cpp` | Update `input()` and `inputPwrite()` flush logic; add EMA tuning in both paths |
| No changes | `main.cpp`, `options.*` (internal-only) |

### Suggested timeout bounds

| Constant | Value | Rationale |
|---|---|---|
| `ADAPTIVE_MIN_TIMEOUT_US` | 2,000 (2ms) | Below this, overhead of small flushes dominates |
| `ADAPTIVE_MAX_TIMEOUT_US` | 50,000 (50ms) | Above this, tail latency is noticeable |
| `ADAPTIVE_EMA_ALPHA` | 0.2 | Smoothing factor for ingress rate EMA |
| `ISAL_BATCH_SIZE` | 512 KB | Unchanged; drives the target fill calculation |

---

## Known risk and required fix: partial `pwrite` handling

### Problem statement
Current code paths in `src/writerthread.cpp` can silently tolerate partial/failed `pwrite` writes:

**`inputPwrite()`** (lines 206-212):
```cpp
while (written < wsize) {
    ssize_t ret = pwrite(mFd, writeData.data() + written, wsize - written, offset + written);
    if (ret <= 0) break;  // ← silent break on failure
    written += ret;
}
```

**`setInputCompletedPwrite()`** (lines 120-127):
```cpp
while (written < compressed.size()) {
    ssize_t ret = pwrite(mFd, compressed.data() + written,
                       compressed.size() - written, offset + written);
    if (ret <= 0) break;  // ← silent break on failure
    written += ret;
}
offset += compressed.size();  // ← advances by planned size, not actual written
```

After the loop, cumulative offset is advanced by `wsize` (line 217), not by `written`, so the file layout drifts.

### Impact
- File layout can develop gaps/offset drift.
- Subsequent writes use incorrect base offsets, propagating corruption.
- Output may be truncated/corrupted while run appears successful (silent data integrity risk).

### Required behavior
- Treat write failure as fatal (except retryable `EINTR`).
- Advance offsets using actual `written` bytes only.
- Never publish cumulative offset for bytes not successfully persisted.

### Minimal remediation

```cpp
// Replace: if (ret <= 0) break;
// With:
if (ret < 0) {
    if (errno == EINTR) continue;
    error_exit("pwrite failed: " + string(strerror(errno)));
}
if (ret == 0) {
    error_exit("pwrite returned 0 (disk full?)");
}
```

And for offset publishing:
```cpp
// Replace: mOffsetRing[mySlot].cumulative_offset.store(offset + wsize, ...)
// With:    mOffsetRing[mySlot].cumulative_offset.store(offset + written, ...)
```

Same pattern for `setInputCompletedPwrite()`.

### Priority
P2 (high-value reliability fix).
Rationale: not always reproducible, but once triggered can produce silent corruption.

---

## Testing plan

### 1. Functional correctness
- `.gz` output integrity: decompress with `gzip -d`, diff against baseline (non-compressed output).
- Verify output is identical with and without adaptive timeout enabled.
- Test with 1, 2, 4, 8 threads.

### 2. Latency improvement
- Synthetic low-rate input (e.g., 10 KB/s FASTQ stream): measure time-to-first-output-byte.
- Expected: max flush delay drops from unbounded (wait for 512KB) to ~50ms.

### 3. Throughput regression
- High-rate workload (e.g., SRR dataset, 8 threads, `.fq` → `.gz`): compare throughput before/after.
- Acceptance: < 2% throughput regression on sustained high-rate workloads.

### 4. Regression matrix

| Input | Output | Mode | Validate |
|---|---|---|---|
| `.fq` | `.fq` | pwrite | byte-identical output |
| `.fq` | `.gz` | pwrite + flight batch | decompressed matches |
| `.fq.gz` | `.fq` | legacy | byte-identical output |
| `.fq.gz` | `.gz` | pwrite + flight batch | decompressed matches |
| any | STDOUT | legacy | byte-identical output |
| `.fq` | `.gz` | 1 thread, legacy | decompressed matches |

### 5. pwrite fault injection
- Simulate short writes / forced `pwrite` failures (e.g., `LD_PRELOAD` shim or filesystem quota).
- Verify: process exits with error, no silent offset drift or corrupted output.
- After remediation: confirm offsets advance by `written`, not planned size.

---

## Rollout
- Ship as always-on internal behavior (compile-time `constexpr` gate for benchmarking).
- Benchmark on representative PE/SE datasets before merging.
- Keep constants easy to adjust in source (not CLI).

## Open questions
- Exact timeout clamp bounds (`min/max`) — 2ms/50ms proposed, needs validation under real workloads.
- Whether backlog pressure term materially helps vs. rate-only tuning — can be deferred to a follow-up after initial measurements.
- `OFFSET_RING_SIZE` (512) limits max in-flight packs — adequate for current thread counts but may need scaling for very high thread counts with small packs.
