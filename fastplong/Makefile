DIR_INC := ./inc
DIR_SRC := ./src
DIR_OBJ := ./obj
DIR_TEST := ./test
DIR_HWY := ./third_party/highway
DIR_ISAL := ./third_party/isa-l
DIR_LIBDEFLATE := ./third_party/libdeflate
DIR_GTEST := ./third_party/googletest

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

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
CXXFLAGS := -std=c++17 -pthread -g -O3 -MD -MP \
	-I. -I${DIR_INC} -I${DIR_SRC} -I${DIR_HWY} \
	-I${DIR_ISAL}/include -I${DIR_LIBDEFLATE} \
	${CXXFLAGS}

# Static libraries built from submodules
ISAL_LIB := $(DIR_ISAL)/bin/isa-l.a
LIBDEFLATE_LIB := $(DIR_LIBDEFLATE)/build/libdeflate.a
GTEST_LIB := $(DIR_GTEST)/build/lib/libgtest.a
GTEST_MAIN_LIB := $(DIR_GTEST)/build/lib/libgtest_main.a

# On Linux: fully static binary; on macOS: static libs, dynamic system runtime
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)
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

# Default target: build deps from submodules and link statically
${BIN_TARGET}: $(ISAL_LIB) $(LIBDEFLATE_LIB) ${OBJ}
	$(CXX) $(OBJ) -o $@ $(ISAL_LIB) $(LIBDEFLATE_LIB) $(LD_FLAGS)

# Build isa-l static library from submodule
$(ISAL_LIB):
	$(MAKE) -C $(DIR_ISAL) -f Makefile.unx lib $(ISAL_MAKE_ARGS)

# Build libdeflate static library from submodule
$(LIBDEFLATE_LIB):
	cd $(DIR_LIBDEFLATE) && cmake -B build \
		-DCMAKE_BUILD_TYPE=Release \
		-DLIBDEFLATE_BUILD_SHARED_LIB=OFF \
		-DLIBDEFLATE_BUILD_GZIP=OFF && \
		cmake --build build

# Build googletest static library from submodule
$(GTEST_LIB) $(GTEST_MAIN_LIB):
	cd $(DIR_GTEST) && cmake -B build \
		-DCMAKE_BUILD_TYPE=Release \
		-DBUILD_GMOCK=OFF && \
		cmake --build build

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

.PHONY: clean install clean-deps test

clean:
	@rm -rf $(DIR_OBJ)
	@rm -f $(TARGET)
	@rm -f $(TEST_TARGET)

clean-deps:
	-$(MAKE) -C $(DIR_ISAL) -f Makefile.unx clean 2>/dev/null || true
	-rm -rf $(DIR_LIBDEFLATE)/build 2>/dev/null || true
	-rm -rf $(DIR_GTEST)/build 2>/dev/null || true

install:
	install $(TARGET) $(BINDIR)/$(TARGET)
	@echo "Installed."

${DIR_OBJ}/%.o:${DIR_TEST}/%.cpp
	@mkdir -p $(@D)
	$(CXX) -c $< -o $@ $(CXXFLAGS) -I${DIR_GTEST}/googletest/include

test: $(ISAL_LIB) $(LIBDEFLATE_LIB) $(GTEST_LIB) ${TEST_OBJ} ${OBJ}
	@mkdir -p bin
	$(CXX) $(TEST_OBJ) ${OBJ:./obj/main.o=} -o ${TEST_TARGET} $(ISAL_LIB) $(LIBDEFLATE_LIB) $(GTEST_LIB) $(GTEST_MAIN_LIB) $(LD_FLAGS)
	./${TEST_TARGET}

-include $(OBJ:.o=.d)
