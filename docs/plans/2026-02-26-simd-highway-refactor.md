# SIMD Highway Refactor Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Centralize all SIMD code into `simd.h`/`simd.cpp` using Highway's `foreach_target.h` multi-target dispatch, port fastp's proven implementations, and accelerate remaining scalar hot loops.

**Architecture:** Single `simd.cpp` compiled via `foreach_target.h` produces code for all CPU targets (SSE2, AVX2, NEON, scalar). Runtime dispatch via `HWY_DYNAMIC_DISPATCH` selects the fastest version. Highway is vendored as `third_party/highway` git submodule.

**Tech Stack:** C++14, Google Highway 1.3, gtest

---

### Task 1: Add Highway as git submodule

**Files:**
- Create: `third_party/highway/` (git submodule)
- Modify: `.gitmodules`

**Step 1: Add the submodule**

```bash
cd /Users/kimy/workspace/fastplong
mkdir -p third_party
git submodule add https://github.com/google/highway.git third_party/highway
cd third_party/highway
git checkout ac0d5d29
cd ../..
```

**Step 2: Verify the submodule**

Run: `ls third_party/highway/hwy/highway.h`
Expected: File exists

Run: `grep HWY_MAJOR third_party/highway/hwy/base.h | head -2`
Expected: `#define HWY_MAJOR 1` and `#define HWY_MINOR 3`

**Step 3: Commit**

```bash
git add .gitmodules third_party/highway
git commit -m "feat: add Highway SIMD library as git submodule (v1.3)"
```

---

### Task 2: Update Makefile for Highway

**Files:**
- Modify: `Makefile`

**Step 1: Update the Makefile**

Replace the entire Makefile with:

```makefile
DIR_INC := ./inc
DIR_SRC := ./src
DIR_OBJ := ./obj
DIR_TEST := ./test
DIR_HWY := ./third_party/highway

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
INCLUDE_DIRS ?= /opt/homebrew/include
LIBRARY_DIRS ?= /opt/homebrew/lib

SRC := $(wildcard ${DIR_SRC}/*.cpp)
TEST := $(wildcard ${DIR_TEST}/*.cpp)
OBJ := $(patsubst %.cpp,${DIR_OBJ}/%.o,$(notdir ${SRC}))
TEST_OBJ := $(patsubst %.cpp,${DIR_OBJ}/%.o,$(notdir ${TEST}))

# Highway runtime dispatch support
HWY_OBJS := ${DIR_OBJ}/hwy_targets.o ${DIR_OBJ}/hwy_abort.o
OBJ += $(HWY_OBJS)

TARGET := fastplong

BIN_TARGET := ${TARGET}
TEST_TARGET := bin/fastplong_unittest

CXX ?= g++
CXXFLAGS := -std=c++14 -pthread -g -O3 -MP -MD -I. -I${DIR_INC} -I${DIR_SRC} -I${DIR_HWY} $(foreach includedir,$(INCLUDE_DIRS),-I$(includedir)) ${CXXFLAGS}
LIBS := -lisal -ldeflate -lpthread
STATIC_FLAGS := -static -L. -Wl,--no-as-needed -pthread
LD_FLAGS := $(foreach librarydir,$(LIBRARY_DIRS),-L$(librarydir)) $(LIBS) $(LD_FLAGS)
STATIC_LD_FLAGS := $(foreach librarydir,$(LIBRARY_DIRS),-L$(librarydir)) $(STATIC_FLAGS) $(LIBS) $(STATIC_LD_FLAGS)


${BIN_TARGET}:${OBJ}
	$(CXX) $(OBJ) -o $@ $(LD_FLAGS)

static:${OBJ}
	$(CXX) $(OBJ) -o ${BIN_TARGET} $(STATIC_LD_FLAGS)

${DIR_OBJ}/%.o:${DIR_SRC}/%.cpp
	@mkdir -p $(@D)
	$(CXX) -c $< -o $@ $(CXXFLAGS)

# Highway source files for runtime CPU detection and error handling
${DIR_OBJ}/hwy_targets.o:${DIR_HWY}/hwy/targets.cc
	@mkdir -p $(@D)
	$(CXX) -c $< -o $@ $(CXXFLAGS)

${DIR_OBJ}/hwy_abort.o:${DIR_HWY}/hwy/abort.cc
	@mkdir -p $(@D)
	$(CXX) -c $< -o $@ $(CXXFLAGS)

.PHONY:clean
.PHONY:static
clean:
	@rm -rf $(DIR_OBJ)
	@rm -f $(TARGET)
	@rm -f $(TEST_TARGET)

install:
	install $(TARGET) $(BINDIR)/$(TARGET)
	@echo "Installed."

${DIR_OBJ}/%.o:${DIR_TEST}/%.cpp
	@mkdir -p $(@D)
	$(CXX) -c $< -o $@ $(CXXFLAGS)

test-static: ${TEST_OBJ} ${OBJ}
	@mkdir -p bin
	$(CXX) $(TEST_OBJ) ${OBJ:./obj/main.o=} -o ${TEST_TARGET} $(STATIC_LD_FLAGS) -lgtest -lgtest_main
	./${TEST_TARGET}

test:${TEST_OBJ} ${OBJ}
	@mkdir -p bin
	$(CXX) $(TEST_OBJ) ${OBJ:./obj/main.o=} -o ${TEST_TARGET} $(LD_FLAGS) -lgtest -lgtest_main
	./${TEST_TARGET}

-include $(OBJ:.o=.d)
```

Key changes from original:
- Added `DIR_HWY := ./third_party/highway`
- Added `-I. -I${DIR_HWY}` to CXXFLAGS
- Removed `-lhwy` from LIBS
- Added `HWY_OBJS` for `hwy_targets.o` and `hwy_abort.o`
- Added build rules for Highway source files

**Step 2: Verify it compiles (will fail at link due to simd changes not yet done, but object files should build)**

Run: `make clean && make -j 2>&1 | tail -5`
Expected: Compilation succeeds (may have link errors due to missing `-lhwy` until we finish simd.cpp, that's OK)

**Step 3: Commit**

```bash
git add Makefile
git commit -m "build: switch Highway from system library to vendored third_party submodule"
```

---

### Task 3: Create simd.h and simd.cpp

**Files:**
- Create: `src/simd.h`
- Create: `src/simd.cpp`

**Step 1: Create `src/simd.h`**

```cpp
#ifndef FASTPLONG_SIMD_H
#define FASTPLONG_SIMD_H

#include <cstddef>

namespace fastplong_simd {

// Count quality metrics for a read in one pass.
// qualstr/seqstr: quality and sequence strings of length len.
// qualThreshold: phred+33 encoded quality threshold for "low quality".
// Outputs: lowQualNum, nBaseNum, totalQual (sum of qual-33 values).
void countQualityMetrics(const char* qualstr, const char* seqstr, int len,
                         char qualThreshold, int& lowQualNum, int& nBaseNum,
                         int& totalQual);

// Reverse complement a DNA sequence.
// src: input sequence of length len.
// dst: output buffer of at least len bytes (must NOT alias src).
void reverseComplement(const char* src, char* dst, int len);

// Count adjacent-base differences for low complexity filter.
// Returns the number of positions where data[i] != data[i+1].
int countAdjacentDiffs(const char* data, int len);

// Count mismatches between two byte strings.
// Returns the number of positions where a[i] != b[i], up to len bytes.
int countMismatches(const char* a, const char* b, int len);

// Run all SIMD unit tests. Returns true if all pass.
bool testSimd();

}  // namespace fastplong_simd

#endif  // FASTPLONG_SIMD_H
```

**Step 2: Create `src/simd.cpp`**

```cpp
// SIMD-accelerated functions for fastplong using Google Highway.
// This file is compiled once per SIMD target via foreach_target.h.

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "src/simd.cpp"
#include "hwy/foreach_target.h"  // IWYU pragma: keep
#include "hwy/highway.h"

HWY_BEFORE_NAMESPACE();
namespace fastplong_simd {
namespace HWY_NAMESPACE {

namespace hn = hwy::HWY_NAMESPACE;

void CountQualityMetricsImpl(const char* qualstr, const char* seqstr, int len,
                             char qualThreshold, int& lowQualNum, int& nBaseNum,
                             int& totalQual) {
    const hn::ScalableTag<uint8_t> d;
    const int N = hn::Lanes(d);

    const auto vThresh = hn::Set(d, static_cast<uint8_t>(qualThreshold));
    const auto vN = hn::Set(d, static_cast<uint8_t>('N'));
    const auto v33 = hn::Set(d, static_cast<uint8_t>(33));

    int lowQual = 0;
    int nBase = 0;
    int qualSum = 0;
    int i = 0;

    // Process in blocks of up to 255 vectors to avoid u16 overflow during
    // quality sum accumulation (255 * 255 = 65025 fits in u16).
    const int blockSize = 255 * N;

    for (int blockStart = 0; blockStart < len; blockStart += blockSize) {
        int blockEnd = blockStart + blockSize;
        if (blockEnd > len) blockEnd = len;

        const hn::ScalableTag<uint16_t> d16;
        auto vQualSum16 = hn::Zero(d16);

        for (i = blockStart; i + N <= blockEnd; i += N) {
            const auto vQual = hn::LoadU(d, reinterpret_cast<const uint8_t*>(qualstr + i));
            const auto vSeq = hn::LoadU(d, reinterpret_cast<const uint8_t*>(seqstr + i));

            const auto maskLowQ = hn::Lt(vQual, vThresh);
            lowQual += hn::CountTrue(d, maskLowQ);

            const auto maskN = hn::Eq(vSeq, vN);
            nBase += hn::CountTrue(d, maskN);

            const auto vQualAdj = hn::Sub(vQual, v33);
            vQualSum16 = hn::Add(vQualSum16, hn::SumsOf2(vQualAdj));
        }

        const hn::ScalableTag<uint32_t> d32;
        qualSum += static_cast<int>(
            hn::ReduceSum(d32, hn::SumsOf2(vQualSum16)));
    }

    // Scalar tail
    for (; i < len; i++) {
        uint8_t qual = static_cast<uint8_t>(qualstr[i]);
        qualSum += qual - 33;
        if (qual < static_cast<uint8_t>(qualThreshold)) lowQual++;
        if (seqstr[i] == 'N') nBase++;
    }

    lowQualNum = lowQual;
    nBaseNum = nBase;
    totalQual = qualSum;
}

void ReverseComplementImpl(const char* src, char* dst, int len) {
    const hn::ScalableTag<uint8_t> d;
    const int N = hn::Lanes(d);

    const auto vA = hn::Set(d, static_cast<uint8_t>('A'));
    const auto vT = hn::Set(d, static_cast<uint8_t>('T'));
    const auto vC = hn::Set(d, static_cast<uint8_t>('C'));
    const auto vG = hn::Set(d, static_cast<uint8_t>('G'));
    const auto va = hn::Set(d, static_cast<uint8_t>('a'));
    const auto vt = hn::Set(d, static_cast<uint8_t>('t'));
    const auto vc = hn::Set(d, static_cast<uint8_t>('c'));
    const auto vg = hn::Set(d, static_cast<uint8_t>('g'));
    const auto vNch = hn::Set(d, static_cast<uint8_t>('N'));

    int i = 0;
    for (; i + N <= len; i += N) {
        const auto v = hn::LoadU(d, reinterpret_cast<const uint8_t*>(src + i));

        auto comp = vNch;
        comp = hn::IfThenElse(hn::Eq(v, vA), vT, comp);
        comp = hn::IfThenElse(hn::Eq(v, vT), vA, comp);
        comp = hn::IfThenElse(hn::Eq(v, vC), vG, comp);
        comp = hn::IfThenElse(hn::Eq(v, vG), vC, comp);
        comp = hn::IfThenElse(hn::Eq(v, va), vT, comp);
        comp = hn::IfThenElse(hn::Eq(v, vt), vA, comp);
        comp = hn::IfThenElse(hn::Eq(v, vc), vG, comp);
        comp = hn::IfThenElse(hn::Eq(v, vg), vC, comp);

        const auto revComp = hn::Reverse(d, comp);
        hn::StoreU(revComp, d, reinterpret_cast<uint8_t*>(dst + len - i - N));
    }

    // Scalar tail
    for (; i < len; i++) {
        char base = src[i];
        char comp;
        switch (base) {
            case 'A': case 'a': comp = 'T'; break;
            case 'T': case 't': comp = 'A'; break;
            case 'C': case 'c': comp = 'G'; break;
            case 'G': case 'g': comp = 'C'; break;
            default: comp = 'N'; break;
        }
        dst[len - 1 - i] = comp;
    }
}

int CountAdjacentDiffsImpl(const char* data, int len) {
    if (len <= 1) return 0;

    const hn::ScalableTag<uint8_t> d;
    const int N = hn::Lanes(d);

    int diff = 0;
    int i = 0;

    for (; i + N < len; i += N) {
        const auto v1 = hn::LoadU(d, reinterpret_cast<const uint8_t*>(data + i));
        const auto v2 = hn::LoadU(d, reinterpret_cast<const uint8_t*>(data + i + 1));
        const auto maskNe = hn::Ne(v1, v2);
        diff += hn::CountTrue(d, maskNe);
    }

    // Scalar tail
    for (; i < len - 1; i++) {
        if (data[i] != data[i + 1]) diff++;
    }

    return diff;
}

int CountMismatchesImpl(const char* a, const char* b, int len) {
    const hn::ScalableTag<uint8_t> d;
    const int N = hn::Lanes(d);

    int diff = 0;
    int i = 0;

    for (; i + N <= len; i += N) {
        const auto va = hn::LoadU(d, reinterpret_cast<const uint8_t*>(a + i));
        const auto vb = hn::LoadU(d, reinterpret_cast<const uint8_t*>(b + i));
        const auto maskNe = hn::Ne(va, vb);
        diff += hn::CountTrue(d, maskNe);
    }

    // Scalar tail
    for (; i < len; i++) {
        if (a[i] != b[i]) diff++;
    }

    return diff;
}

// NOLINTNEXTLINE(google-readability-namespace-comments)
}  // namespace HWY_NAMESPACE
}  // namespace fastplong_simd
HWY_AFTER_NAMESPACE();

// ---- Dynamic dispatch wrappers (compiled once) ----
#if HWY_ONCE

#include <cstdio>
#include <cstring>
#include <cstdint>

namespace fastplong_simd {

HWY_EXPORT(CountQualityMetricsImpl);
HWY_EXPORT(ReverseComplementImpl);
HWY_EXPORT(CountAdjacentDiffsImpl);
HWY_EXPORT(CountMismatchesImpl);

void countQualityMetrics(const char* qualstr, const char* seqstr, int len,
                         char qualThreshold, int& lowQualNum, int& nBaseNum,
                         int& totalQual) {
    HWY_DYNAMIC_DISPATCH(CountQualityMetricsImpl)(qualstr, seqstr, len,
                                                   qualThreshold, lowQualNum,
                                                   nBaseNum, totalQual);
}

void reverseComplement(const char* src, char* dst, int len) {
    HWY_DYNAMIC_DISPATCH(ReverseComplementImpl)(src, dst, len);
}

int countAdjacentDiffs(const char* data, int len) {
    return HWY_DYNAMIC_DISPATCH(CountAdjacentDiffsImpl)(data, len);
}

int countMismatches(const char* a, const char* b, int len) {
    return HWY_DYNAMIC_DISPATCH(CountMismatchesImpl)(a, b, len);
}

// ---- Scalar reference implementations for testing ----

static void scalarCountQualityMetrics(const char* qualstr, const char* seqstr,
                                       int len, char qualThreshold,
                                       int& lowQualNum, int& nBaseNum,
                                       int& totalQual) {
    lowQualNum = 0;
    nBaseNum = 0;
    totalQual = 0;
    for (int i = 0; i < len; i++) {
        uint8_t q = static_cast<uint8_t>(qualstr[i]);
        totalQual += q - 33;
        if (q < static_cast<uint8_t>(qualThreshold)) lowQualNum++;
        if (seqstr[i] == 'N') nBase++;
    }
}

static void scalarReverseComplement(const char* src, char* dst, int len) {
    for (int i = 0; i < len; i++) {
        char c;
        switch (src[i]) {
            case 'A': case 'a': c = 'T'; break;
            case 'T': case 't': c = 'A'; break;
            case 'C': case 'c': c = 'G'; break;
            case 'G': case 'g': c = 'C'; break;
            default: c = 'N'; break;
        }
        dst[len - 1 - i] = c;
    }
}

static int scalarCountAdjacentDiffs(const char* data, int len) {
    int diff = 0;
    for (int i = 0; i < len - 1; i++) {
        if (data[i] != data[i + 1]) diff++;
    }
    return diff;
}

static int scalarCountMismatches(const char* a, const char* b, int len) {
    int diff = 0;
    for (int i = 0; i < len; i++) {
        if (a[i] != b[i]) diff++;
    }
    return diff;
}

bool testSimd() {
    bool pass = true;

    // --- reverseComplement ---
    {
        const char* in = "AAAATTTTCCCCGGGG";
        int len = 16;
        char out[16];
        reverseComplement(in, out, len);
        char ref[16];
        scalarReverseComplement(in, ref, len);
        if (memcmp(out, ref, len) != 0) {
            fprintf(stderr, "FAIL: reverseComplement basic\n");
            pass = false;
        }
    }
    {
        const char* in = "AaTtCcGgN";
        int len = 9;
        char out[9];
        reverseComplement(in, out, len);
        char ref[9];
        scalarReverseComplement(in, ref, len);
        if (memcmp(out, ref, len) != 0) {
            fprintf(stderr, "FAIL: reverseComplement mixed case\n");
            pass = false;
        }
    }
    {
        char out[1] = {'X'};
        reverseComplement("", out, 0);
    }
    {
        char out[1];
        reverseComplement("A", out, 1);
        if (out[0] != 'T') {
            fprintf(stderr, "FAIL: reverseComplement len=1\n");
            pass = false;
        }
    }
    {
        const char* in = "ATCGATCGATCGATCGATCGATCGATCGATCGATCGATCGATCGATCGATCGATCGATCGATCGATCG";
        int len = 68;
        char out[68], ref[68];
        reverseComplement(in, out, len);
        scalarReverseComplement(in, ref, len);
        if (memcmp(out, ref, len) != 0) {
            fprintf(stderr, "FAIL: reverseComplement long string\n");
            pass = false;
        }
    }

    // --- countQualityMetrics ---
    {
        const char* qual = "IIIII";
        const char* seq = "ACGTN";
        int lowQ = 0, nBase = 0, totalQ = 0;
        countQualityMetrics(qual, seq, 5, '5', lowQ, nBase, totalQ);
        int refLowQ = 0, refNBase = 0, refTotalQ = 0;
        scalarCountQualityMetrics(qual, seq, 5, '5', refLowQ, refNBase, refTotalQ);
        if (lowQ != refLowQ || nBase != refNBase || totalQ != refTotalQ) {
            fprintf(stderr, "FAIL: countQualityMetrics basic\n");
            pass = false;
        }
    }
    {
        const char* qual = "!!!!!!!!!!";
        const char* seq = "AAAAAAAAAA";
        int lowQ = 0, nBase = 0, totalQ = 0;
        countQualityMetrics(qual, seq, 10, '5', lowQ, nBase, totalQ);
        int refLowQ = 0, refNBase = 0, refTotalQ = 0;
        scalarCountQualityMetrics(qual, seq, 10, '5', refLowQ, refNBase, refTotalQ);
        if (lowQ != refLowQ || nBase != refNBase || totalQ != refTotalQ) {
            fprintf(stderr, "FAIL: countQualityMetrics all-low\n");
            pass = false;
        }
    }
    {
        const char qual[] = "IIIIII!!!!!IIIII55555NNNNN!!!!!IIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIII";
        const char seq[]  = "ACGTNNACGTNACGTNACGTNACGTNACGTNACGTNACGTNACGTNACGTNACGTNACGTNACGTNACG";
        int len = 68;
        int lowQ = 0, nBase = 0, totalQ = 0;
        countQualityMetrics(qual, seq, len, '5', lowQ, nBase, totalQ);
        int refLowQ = 0, refNBase = 0, refTotalQ = 0;
        scalarCountQualityMetrics(qual, seq, len, '5', refLowQ, refNBase, refTotalQ);
        if (lowQ != refLowQ || nBase != refNBase || totalQ != refTotalQ) {
            fprintf(stderr, "FAIL: countQualityMetrics long\n");
            pass = false;
        }
    }

    // --- countAdjacentDiffs ---
    {
        int d = countAdjacentDiffs("AAAAAAAAAA", 10);
        if (d != 0) {
            fprintf(stderr, "FAIL: countAdjacentDiffs all-same got %d\n", d);
            pass = false;
        }
    }
    {
        const char* s = "ACACACACAC";
        int d = countAdjacentDiffs(s, 10);
        int ref = scalarCountAdjacentDiffs(s, 10);
        if (d != ref) {
            fprintf(stderr, "FAIL: countAdjacentDiffs all-diff got %d expected %d\n", d, ref);
            pass = false;
        }
    }
    {
        if (countAdjacentDiffs("A", 1) != 0) {
            fprintf(stderr, "FAIL: countAdjacentDiffs len=1\n");
            pass = false;
        }
        if (countAdjacentDiffs("", 0) != 0) {
            fprintf(stderr, "FAIL: countAdjacentDiffs len=0\n");
            pass = false;
        }
    }
    {
        const char* s = "ATCGATCGATCGATCGATCGATCGATCGATCGATCGATCGATCGATCGATCGATCGATCGATCGATCG";
        int len = 68;
        int d = countAdjacentDiffs(s, len);
        int ref = scalarCountAdjacentDiffs(s, len);
        if (d != ref) {
            fprintf(stderr, "FAIL: countAdjacentDiffs long got %d expected %d\n", d, ref);
            pass = false;
        }
    }

    // --- countMismatches ---
    {
        const char* s = "ACGTACGTACGT";
        int d = countMismatches(s, s, 12);
        if (d != 0) {
            fprintf(stderr, "FAIL: countMismatches identical got %d\n", d);
            pass = false;
        }
    }
    {
        int d = countMismatches("AAAA", "TTTT", 4);
        if (d != 4) {
            fprintf(stderr, "FAIL: countMismatches all-diff got %d\n", d);
            pass = false;
        }
    }
    {
        const char* a = "ATCGATCGATCGATCGATCGATCGATCGATCGATCGATCGATCGATCGATCGATCGATCGATCGATCG";
        const char* b = "ATCGATCGATCGATCGATCGATCGATCGATCGATCGATCGATCGATCGATCGATCGATCGATCGATCG";
        int d = countMismatches(a, b, 68);
        if (d != 0) {
            fprintf(stderr, "FAIL: countMismatches long-identical got %d\n", d);
            pass = false;
        }
    }
    {
        const char* a = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
        const char* b = "TTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTT";
        int d = countMismatches(a, b, 66);
        if (d != 66) {
            fprintf(stderr, "FAIL: countMismatches long-alldiff got %d\n", d);
            pass = false;
        }
    }
    {
        int d = countMismatches("A", "T", 0);
        if (d != 0) {
            fprintf(stderr, "FAIL: countMismatches len=0 got %d\n", d);
            pass = false;
        }
    }

    return pass;
}

}  // namespace fastplong_simd

#endif  // HWY_ONCE
```

NOTE: There is a bug in the scalar reference above — `nBase++` should be `nBaseNum++`. Fix it when implementing. Copy the exact code from fastp's `src/simd.cpp` as the reference, adjusting only the namespace from `fastp_simd` to `fastplong_simd`.

**Step 3: Verify it compiles**

Run: `make clean && make -j 2>&1 | tail -10`
Expected: Compiles without errors (simd.cpp may generate warnings but no errors)

**Step 4: Commit**

```bash
git add src/simd.h src/simd.cpp
git commit -m "feat: add centralized SIMD module with Highway multi-target dispatch"
```

---

### Task 4: Add SIMD unit test

**Files:**
- Create: `test/simd_test.cpp`

**Step 1: Create the test file**

```cpp
#include <gtest/gtest.h>
#include "../src/simd.h"

TEST(SimdTest, allTests) {
    EXPECT_TRUE(fastplong_simd::testSimd());
}
```

**Step 2: Run tests**

Run: `make clean && make test 2>&1 | tail -20`
Expected: All tests pass including SimdTest.allTests

**Step 3: Commit**

```bash
git add test/simd_test.cpp
git commit -m "test: add SIMD unit tests"
```

---

### Task 5: Refactor filter.cpp to use SIMD

**Files:**
- Modify: `src/filter.cpp`

**Step 1: Update filter.cpp**

Add include at the top (after existing includes):
```cpp
#include "simd.h"
```

Replace the scalar loop in `passFilter()` (the `for(int i=0; i<rlen; i++)` block at lines 27-38) with:
```cpp
        fastplong_simd::countQualityMetrics(qualstr, seqstr, rlen,
                                            mOptions->qualfilter.qualifiedQual,
                                            lowQualNum, nBaseNum, totalQual);
```

Replace the scalar loop in `passLowComplexityFilter()` (the `for(int i=0; i<length-1; i++)` block at lines 73-76) with:
```cpp
    int diff = fastplong_simd::countAdjacentDiffs(data, length);
```
Remove the `int diff = 0;` declaration at line 68 since `countAdjacentDiffs` returns it.

**Step 2: Run tests**

Run: `make clean && make test 2>&1 | tail -20`
Expected: All tests pass (FilterTest.trimAndCut still passes)

**Step 3: Commit**

```bash
git add src/filter.cpp
git commit -m "perf: use SIMD countQualityMetrics and countAdjacentDiffs in filter"
```

---

### Task 6: Refactor sequence.cpp to use SIMD

**Files:**
- Modify: `src/sequence.cpp`

**Step 1: Replace entire sequence.cpp**

```cpp
#include "sequence.h"
#include "simd.h"

Sequence::Sequence(){
}

Sequence::Sequence(string* seq){
    mStr = seq;
}

Sequence::~Sequence(){
    if(mStr)
        delete mStr;
}

void Sequence::print(){
    std::cerr << *mStr;
}

int Sequence::length(){
    return mStr->length();
}

string Sequence::reverseComplement(string* origin) {
    int len = origin->length();
    string str(len, 0);
    fastplong_simd::reverseComplement(origin->c_str(), &str[0], len);
    return str;
}

Sequence Sequence::reverseComplement() {
    string* reversed = new string(Sequence::reverseComplement(mStr));
    return Sequence(reversed);
}

Sequence Sequence::operator~(){
    return reverseComplement();
}
```

Key changes:
- Removed `#include <hwy/highway.h>`, `#include <hwy/contrib/algo/transform-inl.h>`, `#include <hwy/aligned_allocator.h>`, `#include "simdutil.h"`
- Removed `namespace hn = hwy::HWY_NAMESPACE;`
- Simplified `reverseComplement(string*)` to a 4-line function calling `fastplong_simd::reverseComplement`
- Removed the `#if _MSC_VER` / VLA / AllocateAligned branching

**Step 2: Run tests**

Run: `make clean && make test 2>&1 | tail -20`
Expected: All tests pass (SequenceTests.reverse still passes)

**Step 3: Commit**

```bash
git add src/sequence.cpp
git commit -m "refactor: use centralized SIMD reverseComplement in sequence.cpp"
```

---

### Task 7: Refactor adaptertrimmer.cpp to use SIMD

**Files:**
- Modify: `src/adaptertrimmer.cpp`

**Step 1: Update adaptertrimmer.cpp**

Replace includes — change:
```cpp
#include "hwy/highway.h"
```
to:
```cpp
#include "simd.h"
```

In `searchAdapter()`, remove the Highway setup at lines 61-63:
```cpp
    namespace hn = hwy::HWY_NAMESPACE;
    hn::ScalableTag<uint8_t> d8;
    const size_t N = hn::Lanes(d8);
```

Replace the three inner SIMD loops. Each occurrence of this pattern (appears at lines 89-96, 115-122, 139-146):
```cpp
            size_t mismatch = 0;
            for (size_t i = 0; i < alen; i += N)
            {
                const size_t lanesToLoad = min(alen - i, N);
                const auto rdata_v = hn::LoadN(d8, reinterpret_cast<const uint8_t *>(rdata + i + p), lanesToLoad);
                const auto adata_v = hn::LoadN(d8, reinterpret_cast<const uint8_t *>(adata + i), lanesToLoad);
                const auto mismatch_mask = rdata_v != adata_v;
                mismatch += hn::CountTrue(d8, mismatch_mask);
            }
```

Replace each with:
```cpp
            int mismatch = fastplong_simd::countMismatches(rdata + p, adata, alen);
```

Also change `size_t mismatch` → `int mismatch` in the comparison logic (the variable type is returned as `int` by `countMismatches`).

**Step 2: Run tests**

Run: `make clean && make test 2>&1 | tail -20`
Expected: All tests pass (AdapterTrimmer tests still pass)

**Step 3: Commit**

```bash
git add src/adaptertrimmer.cpp
git commit -m "refactor: use centralized SIMD countMismatches in adaptertrimmer"
```

---

### Task 8: Remove simdutil.h

**Files:**
- Delete: `src/simdutil.h`

**Step 1: Verify no remaining references**

Run: `grep -r "simdutil" src/ test/`
Expected: No output (no files reference simdutil.h anymore after Task 6)

**Step 2: Delete the file**

```bash
rm src/simdutil.h
```

**Step 3: Run full build and tests**

Run: `make clean && make test 2>&1 | tail -20`
Expected: All tests pass, no compilation errors

**Step 4: Commit**

```bash
git add -A src/simdutil.h
git commit -m "cleanup: remove simdutil.h, functionality merged into simd.cpp"
```

---

### Task 9: Final verification

**Step 1: Clean build**

Run: `make clean && make -j 2>&1`
Expected: Clean compilation with no errors or warnings

**Step 2: Run all tests**

Run: `make test 2>&1`
Expected: All tests pass

**Step 3: Quick smoke test with real data (if available)**

Run: `./fastplong --help`
Expected: Help output appears normally

**Step 4: Verify no remaining direct Highway includes in source files (except simd.cpp)**

Run: `grep -r '#include.*hwy/' src/ | grep -v simd.cpp`
Expected: No output — only simd.cpp should include Highway headers directly
