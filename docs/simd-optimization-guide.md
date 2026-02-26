# C++ Bioinformatics SIMD Optimization Guide

**Lessons learned from fastp: 1.76x speedup on FASTQ processing**

This document distills the techniques, pitfalls, and patterns we applied to accelerate [fastp](https://github.com/OpenGene/fastp) using Google Highway SIMD, heap allocation elimination, and lookup tables. Everything here is portable, battle-tested on ARM NEON and x86 SSE2/AVX2, and suitable for any C++ project that processes byte-oriented biological data.

---

## Table of Contents

1. [Overview & Results](#1-overview--results)
2. [Optimization 1: Heap Allocation Elimination](#2-optimization-1-heap-allocation-elimination)
3. [Optimization 2: SIMD Vectorization with Google Highway](#3-optimization-2-simd-vectorization-with-google-highway)
4. [Optimization 3: Lookup Table Conversions](#4-optimization-3-lookup-table-conversions)
5. [Highway Integration Guide](#5-highway-integration-guide)
6. [Highway API Pitfalls & Portability](#6-highway-api-pitfalls--portability)
7. [Testing Strategy](#7-testing-strategy)
8. [Hardware-Specific Notes](#8-hardware-specific-notes)
9. [Checklist for Other Projects](#9-checklist-for-other-projects)

---

## 1. Overview & Results

### What we optimized

fastp is a high-performance FASTQ preprocessor. We targeted four categories of hot-path code:

| Category | Technique | Files Changed |
|----------|-----------|---------------|
| Per-read allocations | Stack allocation + move semantics | 4 files, -143/+72 lines |
| Byte-loop hot paths | SIMD via Google Highway | 2 new files (simd.h/cpp), 4 callers |
| Switch-based dispatch | Static lookup tables | 3 files, -50/+63 lines |
| Build system | Git submodules for dependencies | Makefile, .gitmodules |

### Performance impact

```
                    Compressed (.fq.gz)     Uncompressed (.fq)
                    ───────────────────     ──────────────────
  baseline (v1.1.0)     26.39s                  26.50s
  optimized              24.98s                  15.08s
                        ────────                ────────
  speedup               1.06x                   1.76x
  improvement            5.3%                   43.1%
```

- **10M PE150 read pairs** (3 Gbp), Apple M4 Pro, 4 threads
- Output bit-identical between baseline and optimized
- Compressed I/O is gzip-bound; uncompressed isolates compute

---

## 2. Optimization 1: Heap Allocation Elimination

### Principle

Every `new`/`delete` in a per-read loop costs ~50-100ns (malloc overhead + cache pollution). For 100M reads, that's 5-10 seconds of pure allocator overhead.

### Pattern: Stack-allocate fixed-size arrays

```cpp
// BEFORE: heap allocation per read
void processDuplicate(Read* r) {
    uint64* positions = new uint64[8];
    // ... use positions ...
    delete[] positions;
}

// AFTER: stack allocation (zero allocator cost)
void processDuplicate(Read* r) {
    uint64 positions[8];  // fixed-size, known at compile time
    // ... use positions ...
}
```

### Pattern: Direct append instead of temp buffer

```cpp
// BEFORE: allocate temp string, copy into output
void appendToString(string& output, ...) {
    string tmp(len, '\0');
    // fill tmp
    output += tmp;  // copy
}

// AFTER: reserve + direct append (zero temp allocation)
void appendToString(string& output, ...) {
    size_t oldSize = output.size();
    output.resize(oldSize + len);
    char* dst = &output[oldSize];
    // fill dst directly
}
```

### Pattern: Stack-local with move-to-heap handoff

```cpp
// BEFORE: heap strings in inner loop
for (auto& read : pack) {
    string* out = new string();
    read.appendTo(*out);
    writer.push(out);  // writer owns pointer
}

// AFTER: stack string, move to heap at handoff point
for (auto& read : pack) {
    string out;
    out.reserve(512);  // SSO-bust once, reuse buffer
    read.appendTo(out);
    writer.push(new string(std::move(out)));  // one alloc, moved
}
```

### When to apply

- Profile shows allocator functions (`malloc`, `operator new`) in top-10 samples
- Fixed upper bound on allocation size is known
- Allocation is per-read or per-base (millions of calls)

---

## 3. Optimization 2: SIMD Vectorization with Google Highway

### Why Highway over intrinsics

| Factor | Raw intrinsics | Highway |
|--------|---------------|---------|
| Portability | One ISA at a time | x86/ARM/RISC-V/WASM/PPC/s390x |
| Dispatch | Manual `cpuid` + function pointers | `HWY_DYNAMIC_DISPATCH` |
| Maintenance | N copies of every function | One implementation, N compile passes |
| C++ standard | N/A | C++11 minimum |
| Code size | Controlled | Larger (multi-target), but automatic |

### Four vectorized functions

#### 3.1 Quality Metrics (countQualityMetrics)

**Before:** Scalar loop counting low-quality bases, N-bases, and summing qualities.

**After:** Process N bytes per iteration (N=16 on NEON, 32 on AVX2):

```cpp
const hn::ScalableTag<uint8_t> d;
const int N = hn::Lanes(d);  // 16 (NEON), 32 (AVX2), 64 (AVX-512)

for (i = 0; i + N <= len; i += N) {
    const auto vQual = hn::LoadU(d, qualstr + i);
    const auto vSeq  = hn::LoadU(d, seqstr + i);

    // Count qual < threshold (vectorized comparison + popcount)
    lowQual += hn::CountTrue(d, hn::Lt(vQual, vThresh));

    // Count N bases
    nBase += hn::CountTrue(d, hn::Eq(vSeq, vN));

    // Sum qualities via pairwise widening (u8 → u16)
    auto adj = hn::Sub(vQual, v33);
    vQualSum16 = hn::Add(vQualSum16, hn::SumsOf2(adj));
}
```

Key insight: Use `SumsOf2()` for widening accumulation instead of `PromoteLowerTo`/`PromoteUpperTo`, because `PromoteUpperTo` is unavailable on `HWY_SCALAR` target.

#### 3.2 Reverse Complement (reverseComplement)

**Before:** Per-base switch statement + reverse loop.

**After:** Vectorized complement via chained `IfThenElse` + `Reverse`:

```cpp
auto comp = vN;  // default: N
comp = hn::IfThenElse(hn::Eq(v, vA), vT, comp);
comp = hn::IfThenElse(hn::Eq(v, vT), vA, comp);
comp = hn::IfThenElse(hn::Eq(v, vC), vG, comp);
comp = hn::IfThenElse(hn::Eq(v, vG), vC, comp);
// ... lowercase variants ...

const auto revComp = hn::Reverse(d, comp);
hn::StoreU(revComp, d, dst + len - i - N);
```

The `Reverse` op is crucial — it reverses lane order within the vector, so we store the complemented+reversed chunk at the mirrored position in one step.

#### 3.3 Adjacent Difference Count (countAdjacentDiffs)

Used for low-complexity filtering. Loads two overlapping windows and counts mismatches:

```cpp
for (; i + N < len; i += N) {
    const auto v1 = hn::LoadU(d, data + i);
    const auto v2 = hn::LoadU(d, data + i + 1);  // offset by 1
    diff += hn::CountTrue(d, hn::Ne(v1, v2));
}
```

Note: `data + i + 1` is inherently unaligned — `LoadU` is mandatory here.

#### 3.4 Mismatch Count (countMismatches)

Used for overlap detection in paired-end merging:

```cpp
for (; i + N <= len; i += N) {
    const auto va = hn::LoadU(d, a + i);
    const auto vb = hn::LoadU(d, b + i);
    diff += hn::CountTrue(d, hn::Ne(va, vb));
}
```

### Vectorization selection criteria

Good candidates for SIMD acceleration:

- Per-byte operations on DNA/quality strings (comparison, lookup, counting)
- Operations that are data-parallel (no loop-carried dependencies)
- Hot inner loops confirmed by profiling (> 5% of total CPU time)
- Simple control flow (no early exits dependent on individual bytes)

Poor candidates:

- I/O-bound code (gzip dominates; vectorize the compute, not the I/O)
- Complex state machines (adapter trimming with backtracking)
- Loops with frequent early exits (alignment scoring with thresholds)

---

## 4. Optimization 3: Lookup Table Conversions

### Principle

A `switch` with 4-8 cases on ASCII characters compiles to a branch tree. With random DNA input, branch prediction accuracy drops to ~75%, costing ~15 cycles per misprediction.

A 256-byte lookup table replaces this with a single indexed load (~4 cycles, always predictable).

### Pattern

```cpp
// BEFORE: switch per base (branch-heavy)
int base2val(char base) {
    switch (base) {
        case 'A': return 0;
        case 'T': return 1;
        case 'C': return 2;
        case 'G': return 3;
        default:  return -1;
    }
}

// AFTER: lookup table (branchless)
static const int BASE2VAL[256] = {
    // Initialize all to -1, then set specific entries
    ['A'] = 0, ['T'] = 1, ['C'] = 2, ['G'] = 3,
    // ... all others default to -1
};

int base2val(char base) {
    return BASE2VAL[static_cast<uint8_t>(base)];
}
```

For C++11 compatibility (no designated initializers), use a helper function:

```cpp
static const int* initBase2Val() {
    static int table[256];
    std::fill(table, table + 256, -1);
    table['A'] = table['a'] = 0;
    table['T'] = table['t'] = 1;
    table['C'] = table['c'] = 2;
    table['G'] = table['g'] = 3;
    return table;
}
static const int* BASE2VAL = initBase2Val();
```

### Applied in fastp

| Location | Switch cases | Table name | Used for |
|----------|-------------|------------|----------|
| `stats.cpp` base2val() | A/T/C/G | `BASE2VAL[256]` | Kmer computation |
| `duplicate.cpp` seq2intvector() | A/T/C/G | `SEQ_HASH_VAL[256]` | Bloom filter hashing |
| `polyx.cpp` trimPolyX() | A/T/C/G/N | `POLYX_BASE_IDX[256]` | Poly-X tail detection |

---

## 5. Highway Integration Guide

### File structure

```
project/
├── third_party/
│   └── highway/              # git submodule
├── src/
│   ├── simd.h                # Public API declarations
│   └── simd.cpp              # Highway implementation + dispatch
└── Makefile
```

### Minimal simd.h

```cpp
#ifndef PROJECT_SIMD_H
#define PROJECT_SIMD_H

namespace my_simd {

void myFunction(const char* input, int len, int& result);
bool testSimd();  // unit tests

}  // namespace my_simd

#endif
```

### Minimal simd.cpp

```cpp
// Step 1: Highway multi-target preamble
#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "src/simd.cpp"  // path to THIS file
#include "hwy/foreach_target.h"
#include "hwy/highway.h"

// Step 2: Implementation (compiled once per SIMD target)
HWY_BEFORE_NAMESPACE();
namespace my_simd {
namespace HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

void MyFunctionImpl(const char* input, int len, int& result) {
    const hn::ScalableTag<uint8_t> d;
    const int N = hn::Lanes(d);
    int sum = 0, i = 0;

    for (; i + N <= len; i += N) {
        const auto v = hn::LoadU(d, reinterpret_cast<const uint8_t*>(input + i));
        // ... vectorized work ...
    }

    // Scalar tail (ALWAYS required)
    for (; i < len; i++) {
        // ... scalar fallback ...
    }
    result = sum;
}

}  // namespace HWY_NAMESPACE
}  // namespace my_simd
HWY_AFTER_NAMESPACE();

// Step 3: Dynamic dispatch wrappers (compiled once)
#if HWY_ONCE
namespace my_simd {

HWY_EXPORT(MyFunctionImpl);

void myFunction(const char* input, int len, int& result) {
    HWY_DYNAMIC_DISPATCH(MyFunctionImpl)(input, len, result);
}

}  // namespace my_simd
#endif
```

### Makefile additions

```makefile
# Add Highway include path
CXXFLAGS += -I./third_party/highway

# Compile Highway runtime (only two files needed)
obj/hwy_targets.o: third_party/highway/hwy/targets.cc
	$(CXX) -c $< -o $@ $(CXXFLAGS)

obj/hwy_abort.o: third_party/highway/hwy/abort.cc
	$(CXX) -c $< -o $@ $(CXXFLAGS)

# Link them with your binary
LDFLAGS += obj/hwy_targets.o obj/hwy_abort.o
```

No cmake required. No library to pre-build. Just two .cc files.

### Adding as a git submodule

```bash
git submodule add https://github.com/google/highway.git third_party/highway
cd third_party/highway && git checkout 1.3.0 && cd ../..
git add .gitmodules third_party/highway
```

---

## 6. Highway API Pitfalls & Portability

### Critical: Always use `LoadU`/`StoreU`, never `Load`/`Store`

```cpp
// WRONG: Load requires pointer aligned to sizeof(Vec)
const auto v = hn::Load(d, ptr);   // SIGSEGV on AVX2 if ptr is 16-byte aligned

// CORRECT: LoadU handles any alignment
const auto v = hn::LoadU(d, ptr);  // Safe on all targets
```

`std::string::c_str()` returns heap-allocated data (typically 8 or 16-byte aligned). This is sufficient for SSE (16B) but NOT for AVX2 (32B) or AVX-512 (64B). On modern x86, `LoadU` has identical throughput to `Load` when data happens to be aligned, so there is zero penalty.

### Critical: Avoid `PromoteUpperTo` — use `SumsOf2` instead

`PromoteUpperTo`, `PromoteOddTo`, `UpperHalf`, and `Combine` are **not available** on `HWY_SCALAR`. If someone compiles with `HWY_COMPILE_ONLY_SCALAR` or runs Highway's test suite (`HWY_COMPILE_ALL_ATTAINABLE`), your code won't compile.

```cpp
// WRONG: won't compile on HWY_SCALAR
const auto lo = hn::PromoteLowerTo(d16, v_u8);
const auto hi = hn::PromoteUpperTo(d16, v_u8);  // ERROR on SCALAR
vSum = hn::Add(vSum, hn::Add(lo, hi));

// CORRECT: SumsOf2 works on ALL targets including SCALAR
vSum16 = hn::Add(vSum16, hn::SumsOf2(v_u8));
```

`SumsOf2(v)` pairwise-adds adjacent lanes with widening: `u8→u16`, `u16→u32`, etc. It's semantically equivalent to `PromoteLowerTo + PromoteUpperTo + Add` but compiles everywhere.

### Overflow management for accumulation

When accumulating u8 values into u16, you must block the loop to prevent u16 overflow:

```
Max per iteration: SumsOf2 adds 2 u8 values → max 510 per u16 lane
Max iterations before u16 overflow: 65535 / 510 ≈ 128
Safe block size: 128 * N (or use 255 * N with tighter max-per-element bounds)
```

Then reduce to u32 between blocks:

```cpp
const int blockSize = 255 * N;  // safe for quality values (max 93 per byte)

for (int blockStart = 0; blockStart < len; blockStart += blockSize) {
    auto vSum16 = hn::Zero(d16);
    for (i = blockStart; i + N <= blockEnd; i += N) {
        vSum16 = hn::Add(vSum16, hn::SumsOf2(vQualAdj));
    }
    // Widen u16 → u32 and reduce
    totalSum += hn::ReduceSum(d32, hn::SumsOf2(vSum16));
}
```

### In-place buffer aliasing

If your function reads from `src` and writes to `dst`, and the SIMD version processes chunks in non-sequential order (e.g., `Reverse` writes to `dst[len-i-N]`), then **`src == dst` is unsafe** — later reads will see overwritten data. Document this clearly:

```cpp
// dst: output buffer of at least len bytes (must NOT alias src).
void reverseComplement(const char* src, char* dst, int len);
```

### Always include a scalar tail

SIMD loops process N elements at a time. The remainder must be handled:

```cpp
// SIMD loop
for (i = 0; i + N <= len; i += N) { /* vectorized */ }

// Scalar tail — handles 0 to N-1 remaining elements
for (; i < len; i++) { /* scalar fallback */ }
```

This is not optional. It handles len=0, len=1, and any length not divisible by N.

---

## 7. Testing Strategy

### Principle: Cross-validate SIMD against scalar reference

Every SIMD function needs a scalar reference implementation and tests comparing them:

```cpp
// Scalar reference (trusted, simple)
static int scalarCountMismatches(const char* a, const char* b, int len) {
    int diff = 0;
    for (int i = 0; i < len; i++)
        if (a[i] != b[i]) diff++;
    return diff;
}

// Test
int simd_result = countMismatches(a, b, len);
int scalar_result = scalarCountMismatches(a, b, len);
assert(simd_result == scalar_result);
```

### Required test cases

| Category | Why |
|----------|-----|
| len=0 | Empty input, no crash |
| len=1 | Pure scalar path |
| len < N | No SIMD iterations, only scalar tail |
| len = N | Exactly one SIMD iteration, no tail |
| len = N+1 | One SIMD iteration + 1 scalar |
| len = large (e.g., 68) | Multiple SIMD iterations + tail |
| All-same input | Edge case for counting functions |
| All-different input | Maximum count |
| Mixed case (a/A/t/T) | Verify case handling |

### End-to-end correctness

Always verify that the full pipeline produces bit-identical output:

```bash
# Run both versions on same input
./fastp_orig -i R1.fq -I R2.fq -o orig_R1.fq -O orig_R2.fq
./fastp_opt  -i R1.fq -I R2.fq -o opt_R1.fq  -O opt_R2.fq

# Compare outputs
md5 -q orig_R1.fq opt_R1.fq  # must match
md5 -q orig_R2.fq opt_R2.fq  # must match
```

---

## 8. Hardware-Specific Notes

### ARM NEON (Apple Silicon, Graviton, etc.)

- Vector width: 128-bit (16 bytes)
- `LoadU` vs `Load`: No performance difference (NEON doesn't distinguish aligned/unaligned loads)
- `Reverse` for u8: Uses `vrev16q_u8` — native, single-cycle

### x86 SSE2/SSE4/AVX2

- Vector width: 128-bit (SSE) or 256-bit (AVX2)
- `LoadU` vs `Load`: Identical throughput since Haswell (2013). On older CPUs, `LoadU` may be 1 cycle slower but won't crash
- `Reverse` for u8 on AVX2: Two-step — `PSHUFB` within 128-bit lanes + `VPERM2I128` to swap lanes

### x86 AVX-512

- Vector width: 512-bit (64 bytes)
- **Throttling warning**: On Skylake-X/Cascade Lake, first AVX-512 instruction causes ~10-20% core frequency drop lasting ~1ms. For short-lived SIMD bursts, this can hurt overall throughput
- Disable if needed: `CXXFLAGS="-DHWY_DISABLED_TARGETS=HWY_AVX3"`

### RISC-V Vector (RVV)

- Variable vector width (runtime-determined)
- Broken on Clang < 19 (auto-excluded by Highway)
- `Reverse` uses `vrgather` — may be slow on some implementations

### Compiler-specific broken targets (auto-excluded by Highway)

| Target | Broken on | Fallback |
|--------|-----------|----------|
| SVE/SVE2 | GCC < 14 / < 12 | NEON |
| RVV | Clang < 19 | EMU128 |
| AVX-512 | MSVC (all versions) | AVX2 |
| AVX2+ | 32-bit Clang | SSE4 |
| EMU128 | GCC < 16 | SCALAR |

These are all handled automatically — no user intervention needed.

---

## 9. Checklist for Other Projects

Use this when applying the same optimizations to another bioinformatics tool:

### Profiling (do first)

- [ ] Profile with `perf record`/`perf report` or Instruments
- [ ] Identify top-5 functions by CPU sample count
- [ ] Check if allocator functions appear in top-10
- [ ] Determine if workload is I/O-bound or compute-bound

### Heap allocation elimination

- [ ] Search for `new`/`delete` in per-read loops
- [ ] Replace fixed-size heap arrays with stack arrays
- [ ] Replace temp string construction with direct `reserve()`+write
- [ ] Use `std::move` for stack-to-heap handoff at thread boundaries
- [ ] Verify: output unchanged, no memory leaks (ASAN)

### SIMD vectorization

- [ ] Add Highway as git submodule (`third_party/highway`)
- [ ] Create `simd.h` (public API) and `simd.cpp` (Highway implementation)
- [ ] Add `hwy/targets.cc` and `hwy/abort.cc` to build
- [ ] Use `LoadU`/`StoreU` everywhere (never `Load`/`Store`)
- [ ] Avoid `PromoteUpperTo` — use `SumsOf2` for widening accumulation
- [ ] Always include scalar tail after SIMD loop
- [ ] Document aliasing constraints in header comments
- [ ] Add scalar reference implementations for cross-validation tests
- [ ] Test edge cases: len=0, len=1, len<N, len=N, len=N+1, large

### Lookup tables

- [ ] Search for `switch` statements on base characters (A/T/C/G)
- [ ] Replace with `static const` 256-entry tables
- [ ] Cast `char` to `uint8_t` for table indexing (avoid signed index)
- [ ] Verify: output unchanged

### Benchmarking

- [ ] Generate deterministic test data (fixed RNG seed)
- [ ] Test both compressed and uncompressed I/O
- [ ] Run minimum 3 repetitions, report median
- [ ] Warm filesystem cache before timing
- [ ] Verify bit-identical output between versions

---

## Appendix: Commit History

```
1ba3e21 perf: eliminate hot-path heap allocations in processing pipeline
193df28 perf: add SIMD acceleration via Google Highway for core hot paths
1bfcd73 perf: replace per-base switch statements with lookup tables
f37387a build: replace bundled isa-l/libdeflate with git submodules
1dc777b fix: harden SIMD implementation for correctness and portability
```

## Appendix: Benchmark Environment

| Item | Value |
|------|-------|
| CPU | Apple M4 Pro (14 cores) |
| RAM | 48 GB |
| OS | Darwin 25.3.0 arm64 (macOS Sequoia) |
| Compiler | Apple clang 17.0.0 |
| Flags | `-std=c++11 -O3` |
| SIMD | ARM NEON (128-bit) via Highway 1.3.0 |
| Test data | 10M PE150 pairs, synthetic, RNG seed 42 |
