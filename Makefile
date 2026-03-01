# Monorepo Makefile: builds both fastp and fastplong from shared sources
#
# Directory layout:
#   src/common/     - shared source files (compiled twice, per-target -I)
#   src/fastp/      - fastp-specific sources
#   src/fastplong/  - fastplong-specific sources
#   test/fastp/     - googletest-based tests for fastp
#   test/fastplong/ - googletest-based tests for fastplong
#
# Include resolution: project dir is searched first, then common, then third_party.
# This lets each target pick up its own common.h, options.h, etc. while
# sharing headers like writer.h from src/common/.

# --- Source directories ---
DIR_COMMON     := ./src/common
DIR_FASTP      := ./src/fastp
DIR_FASTPLONG  := ./src/fastplong
DIR_TEST_FP    := ./test/fastp
DIR_TEST_FL    := ./test/fastplong
DIR_HWY        := ./third_party/highway
DIR_ISAL       := ./third_party/isa-l
DIR_LIBDEFLATE := ./third_party/libdeflate
DIR_GTEST      := ./third_party/googletest

# --- Object directories (per-target to avoid collisions) ---
OBJ_FASTP      := ./build/fastp
OBJ_FASTPLONG  := ./build/fastplong
OBJ_COMMON_FP  := ./build/common_fp
OBJ_COMMON_FL  := ./build/common_fl
OBJ_TEST_FP    := ./build/test_fp
OBJ_TEST_FL    := ./build/test_fl

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

# --- Source file discovery ---
COMMON_SRC    := $(wildcard $(DIR_COMMON)/*.cpp)
FASTP_SRC     := $(wildcard $(DIR_FASTP)/*.cpp)
FASTPLONG_SRC := $(wildcard $(DIR_FASTPLONG)/*.cpp)
TEST_FP_SRC   := $(wildcard $(DIR_TEST_FP)/*.cpp)
TEST_FL_SRC   := $(wildcard $(DIR_TEST_FL)/*.cpp)

# --- Object file lists ---
COMMON_FP_OBJ  := $(patsubst %.cpp,$(OBJ_COMMON_FP)/%.o,$(notdir $(COMMON_SRC)))
COMMON_FL_OBJ  := $(patsubst %.cpp,$(OBJ_COMMON_FL)/%.o,$(notdir $(COMMON_SRC)))
FASTP_OBJ      := $(patsubst %.cpp,$(OBJ_FASTP)/%.o,$(notdir $(FASTP_SRC)))
FASTPLONG_OBJ  := $(patsubst %.cpp,$(OBJ_FASTPLONG)/%.o,$(notdir $(FASTPLONG_SRC)))
TEST_FP_OBJ    := $(patsubst %.cpp,$(OBJ_TEST_FP)/%.o,$(notdir $(TEST_FP_SRC)))
TEST_FL_OBJ    := $(patsubst %.cpp,$(OBJ_TEST_FL)/%.o,$(notdir $(TEST_FL_SRC)))

# Highway runtime dispatch support (one set per target)
HWY_FP_OBJS := $(OBJ_FASTP)/hwy_targets.o $(OBJ_FASTP)/hwy_abort.o
HWY_FL_OBJS := $(OBJ_FASTPLONG)/hwy_targets.o $(OBJ_FASTPLONG)/hwy_abort.o

# --- Per-target include flags ---
# Project dir first -> common -> third_party
# This means #include "common.h" finds src/fastp/common.h (or src/fastplong/common.h)
# while #include "writer.h" falls through to src/common/writer.h
THIRD_PARTY_INC := -I$(DIR_HWY) -I$(DIR_ISAL)/include -I$(DIR_LIBDEFLATE)
FASTP_INC       := -I$(DIR_FASTP) -I$(DIR_COMMON) $(THIRD_PARTY_INC)
FASTPLONG_INC   := -I$(DIR_FASTPLONG) -I$(DIR_COMMON) $(THIRD_PARTY_INC)

# --- Compiler flags ---
CXX ?= g++
CXXFLAGS_BASE := -std=c++17 -pthread -g -O3 -MD -MP $(CXXFLAGS)

# --- Static libraries from submodules ---
ISAL_LIB       := $(DIR_ISAL)/bin/isa-l.a
LIBDEFLATE_LIB := $(DIR_LIBDEFLATE)/build/libdeflate.a
GTEST_LIB      := $(DIR_GTEST)/build/lib/libgtest.a
GTEST_MAIN_LIB := $(DIR_GTEST)/build/lib/libgtest_main.a

# --- Platform detection ---
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

# On Linux: fully static binary; on macOS: static libs, dynamic system runtime
ifeq ($(UNAME_S),Linux)
  LD_FLAGS := -static -Wl,--no-as-needed -lpthread
else
  LD_FLAGS := -lpthread
endif

# isa-l: macOS reports arm64 but isa-l expects aarch64 for NEON assembly
ISAL_MAKE_ARGS :=
ifeq ($(UNAME_M),arm64)
  ISAL_MAKE_ARGS := host_cpu=aarch64 arch=aarch64
endif

# =====================================================================
# Targets
# =====================================================================

.PHONY: all clean install clean-deps test test-fastp test-fastplong bench-fastp bench-fastplong

all: bin/fastp bin/fastplong

# --- fastp binary ---
bin/fastp: $(ISAL_LIB) $(LIBDEFLATE_LIB) $(FASTP_OBJ) $(COMMON_FP_OBJ) $(HWY_FP_OBJS)
	@mkdir -p bin
	$(CXX) $(FASTP_OBJ) $(COMMON_FP_OBJ) $(HWY_FP_OBJS) -o $@ \
		$(ISAL_LIB) $(LIBDEFLATE_LIB) $(LD_FLAGS)

# --- fastplong binary ---
bin/fastplong: $(ISAL_LIB) $(LIBDEFLATE_LIB) $(FASTPLONG_OBJ) $(COMMON_FL_OBJ) $(HWY_FL_OBJS)
	@mkdir -p bin
	$(CXX) $(FASTPLONG_OBJ) $(COMMON_FL_OBJ) $(HWY_FL_OBJS) -o $@ \
		$(ISAL_LIB) $(LIBDEFLATE_LIB) $(LD_FLAGS)

# =====================================================================
# Compilation rules
# =====================================================================

# fastp-specific sources
$(OBJ_FASTP)/%.o: $(DIR_FASTP)/%.cpp
	@mkdir -p $(@D)
	$(CXX) -c $< -o $@ $(CXXFLAGS_BASE) $(FASTP_INC)

# common sources compiled for fastp
$(OBJ_COMMON_FP)/%.o: $(DIR_COMMON)/%.cpp
	@mkdir -p $(@D)
	$(CXX) -c $< -o $@ $(CXXFLAGS_BASE) $(FASTP_INC)

# fastplong-specific sources
$(OBJ_FASTPLONG)/%.o: $(DIR_FASTPLONG)/%.cpp
	@mkdir -p $(@D)
	$(CXX) -c $< -o $@ $(CXXFLAGS_BASE) $(FASTPLONG_INC)

# common sources compiled for fastplong
$(OBJ_COMMON_FL)/%.o: $(DIR_COMMON)/%.cpp
	@mkdir -p $(@D)
	$(CXX) -c $< -o $@ $(CXXFLAGS_BASE) $(FASTPLONG_INC)

# Highway runtime objects (fastp)
$(OBJ_FASTP)/hwy_targets.o: $(DIR_HWY)/hwy/targets.cc
	@mkdir -p $(@D)
	$(CXX) -c $< -o $@ $(CXXFLAGS_BASE) $(FASTP_INC)

$(OBJ_FASTP)/hwy_abort.o: $(DIR_HWY)/hwy/abort.cc
	@mkdir -p $(@D)
	$(CXX) -c $< -o $@ $(CXXFLAGS_BASE) $(FASTP_INC)

# Highway runtime objects (fastplong)
$(OBJ_FASTPLONG)/hwy_targets.o: $(DIR_HWY)/hwy/targets.cc
	@mkdir -p $(@D)
	$(CXX) -c $< -o $@ $(CXXFLAGS_BASE) $(FASTPLONG_INC)

$(OBJ_FASTPLONG)/hwy_abort.o: $(DIR_HWY)/hwy/abort.cc
	@mkdir -p $(@D)
	$(CXX) -c $< -o $@ $(CXXFLAGS_BASE) $(FASTPLONG_INC)

# =====================================================================
# Third-party library builds (shared, built once)
# =====================================================================

$(ISAL_LIB):
	$(MAKE) -C $(DIR_ISAL) -f Makefile.unx lib $(ISAL_MAKE_ARGS)

$(LIBDEFLATE_LIB):
	cd $(DIR_LIBDEFLATE) && cmake -B build \
		-DCMAKE_BUILD_TYPE=Release \
		-DLIBDEFLATE_BUILD_SHARED_LIB=OFF \
		-DLIBDEFLATE_BUILD_GZIP=OFF && \
		cmake --build build

$(GTEST_LIB) $(GTEST_MAIN_LIB):
	cd $(DIR_GTEST) && cmake -B build \
		-DCMAKE_BUILD_TYPE=Release \
		-DBUILD_GMOCK=OFF && \
		cmake --build build

# =====================================================================
# Test targets (per-project googletest suites)
# =====================================================================

GTEST_INC := -I$(DIR_GTEST)/googletest/include

# fastp test objects
$(OBJ_TEST_FP)/%.o: $(DIR_TEST_FP)/%.cpp
	@mkdir -p $(@D)
	$(CXX) -c $< -o $@ $(CXXFLAGS_BASE) $(FASTP_INC) $(GTEST_INC)

# fastplong test objects
$(OBJ_TEST_FL)/%.o: $(DIR_TEST_FL)/%.cpp
	@mkdir -p $(@D)
	$(CXX) -c $< -o $@ $(CXXFLAGS_BASE) $(FASTPLONG_INC) $(GTEST_INC)

TEST_FP_TARGET := bin/fastp_unittest
TEST_FL_TARGET := bin/fastplong_unittest

test-fastp: $(ISAL_LIB) $(LIBDEFLATE_LIB) $(GTEST_LIB) \
            $(TEST_FP_OBJ) $(FASTP_OBJ) $(COMMON_FP_OBJ) $(HWY_FP_OBJS)
	@mkdir -p bin
	$(CXX) $(TEST_FP_OBJ) $(filter-out $(OBJ_FASTP)/main.o,$(FASTP_OBJ)) \
		$(COMMON_FP_OBJ) $(HWY_FP_OBJS) -o $(TEST_FP_TARGET) \
		$(ISAL_LIB) $(LIBDEFLATE_LIB) $(GTEST_LIB) $(GTEST_MAIN_LIB) $(LD_FLAGS)
	./$(TEST_FP_TARGET) --gtest_filter=-'FastpBench*'

bench-fastp: bin/fastp $(ISAL_LIB) $(LIBDEFLATE_LIB) $(GTEST_LIB) \
             $(TEST_FP_OBJ) $(FASTP_OBJ) $(COMMON_FP_OBJ) $(HWY_FP_OBJS)
	@mkdir -p bin
	$(CXX) $(TEST_FP_OBJ) $(filter-out $(OBJ_FASTP)/main.o,$(FASTP_OBJ)) \
		$(COMMON_FP_OBJ) $(HWY_FP_OBJS) -o $(TEST_FP_TARGET) \
		$(ISAL_LIB) $(LIBDEFLATE_LIB) $(GTEST_LIB) $(GTEST_MAIN_LIB) $(LD_FLAGS)
	./$(TEST_FP_TARGET) --gtest_filter='FastpBench*'

test-fastplong: $(ISAL_LIB) $(LIBDEFLATE_LIB) $(GTEST_LIB) \
                $(TEST_FL_OBJ) $(FASTPLONG_OBJ) $(COMMON_FL_OBJ) $(HWY_FL_OBJS)
	@mkdir -p bin
	$(CXX) $(TEST_FL_OBJ) $(filter-out $(OBJ_FASTPLONG)/main.o,$(FASTPLONG_OBJ)) \
		$(COMMON_FL_OBJ) $(HWY_FL_OBJS) -o $(TEST_FL_TARGET) \
		$(ISAL_LIB) $(LIBDEFLATE_LIB) $(GTEST_LIB) $(GTEST_MAIN_LIB) $(LD_FLAGS)
	./$(TEST_FL_TARGET) --gtest_filter=-'FastplongBench*'

bench-fastplong: bin/fastplong $(ISAL_LIB) $(LIBDEFLATE_LIB) $(GTEST_LIB) \
                 $(TEST_FL_OBJ) $(FASTPLONG_OBJ) $(COMMON_FL_OBJ) $(HWY_FL_OBJS)
	@mkdir -p bin
	$(CXX) $(TEST_FL_OBJ) $(filter-out $(OBJ_FASTPLONG)/main.o,$(FASTPLONG_OBJ)) \
		$(COMMON_FL_OBJ) $(HWY_FL_OBJS) -o $(TEST_FL_TARGET) \
		$(ISAL_LIB) $(LIBDEFLATE_LIB) $(GTEST_LIB) $(GTEST_MAIN_LIB) $(LD_FLAGS)
	./$(TEST_FL_TARGET) --gtest_filter='FastplongBench*'

test: test-fastp test-fastplong

# =====================================================================
# Maintenance
# =====================================================================

clean:
	@rm -rf build bin

clean-deps:
	-$(MAKE) -C $(DIR_ISAL) -f Makefile.unx clean 2>/dev/null || true
	-rm -rf $(DIR_LIBDEFLATE)/build 2>/dev/null || true
	-rm -rf $(DIR_GTEST)/build 2>/dev/null || true

install:
	install bin/fastp $(BINDIR)/fastp
	install bin/fastplong $(BINDIR)/fastplong
	@echo "Installed."

# Automatic dependency tracking (generated by -MD -MP)
-include $(FASTP_OBJ:.o=.d) $(COMMON_FP_OBJ:.o=.d)
-include $(FASTPLONG_OBJ:.o=.d) $(COMMON_FL_OBJ:.o=.d)
-include $(TEST_FP_OBJ:.o=.d) $(TEST_FL_OBJ:.o=.d)
