# SIMD Highway Refactor Design

## Goal

Centralize all SIMD code into a single `simd.h`/`simd.cpp` pair using Google Highway's `foreach_target.h` multi-target compilation + `HWY_DYNAMIC_DISPATCH` runtime dispatch pattern. Port proven implementations from fastp and accelerate remaining scalar hot loops.

## Current State

- fastplong links `-lhwy` as system library
- SIMD exists in `adaptertrimmer.cpp` (mismatch counting) and `sequence.cpp` (reverse complement) but without multi-target dispatch
- `simdutil.h` has a custom `Transform1Reversed` template
- `filter.cpp` hot loops (`passFilter`, `passLowComplexityFilter`) are pure scalar

## Changes

### New Files

- `third_party/highway/` — Highway source (header-only + `targets.cc` / `abort.cc`)
- `src/simd.h` — Public SIMD API
- `src/simd.cpp` — Multi-target implementation with `foreach_target.h`

### Modified Files

- `Makefile` — Add `DIR_HWY`, compile `hwy_targets.o` / `hwy_abort.o`, remove `-lhwy`, add `-I${DIR_HWY}`
- `src/filter.cpp` — Replace scalar loops with `countQualityMetrics` and `countAdjacentDiffs`
- `src/sequence.cpp` — Replace inline Highway code with `reverseComplement` call
- `src/adaptertrimmer.cpp` — Replace inner SIMD loop with `countMismatches` call

### Deleted Files

- `src/simdutil.h` — Functionality merged into `simd.cpp`

## API

```cpp
namespace fastplong_simd {
    void countQualityMetrics(const char* qualstr, const char* seqstr, int len,
                             char qualThreshold, int& lowQualNum, int& nBaseNum,
                             int& totalQual);
    void reverseComplement(const char* src, char* dst, int len);
    int countAdjacentDiffs(const char* data, int len);
    int countMismatches(const char* a, const char* b, int len);
    bool testSimd();
}
```

## simd.cpp Structure

```
#define HWY_TARGET_INCLUDE "src/simd.cpp"
#include "hwy/foreach_target.h"

namespace fastplong_simd::HWY_NAMESPACE {
    CountQualityMetricsImpl()   — vectorized quality stats (SumsOf2 overflow-safe)
    ReverseComplementImpl()     — vectorized complement + reverse
    CountAdjacentDiffsImpl()    — vectorized adjacent comparison
    CountMismatchesImpl()       — vectorized byte comparison
}

#if HWY_ONCE
    HWY_EXPORT(...)             — register all target versions
    dispatch wrappers           — HWY_DYNAMIC_DISPATCH
    scalar references           — for test validation
    testSimd()                  — unit tests
#endif
```

## Key Technical Details

- Block-based accumulation in `countQualityMetrics` (255*N elements per block) prevents u16 overflow for long reads (10k-100k+ bp)
- `SumsOf2` for u8->u16 promotion works on all targets including HWY_SCALAR
- `searchAdapter` outer logic (branching, edit_distance) stays in `adaptertrimmer.cpp`; only inner mismatch loop delegates to `countMismatches`

## Reference

Based on fastp's `src/simd.h` / `src/simd.cpp` implementation pattern.
