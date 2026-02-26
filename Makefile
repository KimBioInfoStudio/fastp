DIR_INC := ./inc
DIR_SRC := ./src
DIR_OBJ := ./obj
DIR_TEST := ./test
DIR_HWY := ./third_party/highway
DIR_ISAL := ./third_party/isa-l
DIR_LIBDEFLATE := ./third_party/libdeflate

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
INCLUDE_DIRS ?=
LIBRARY_DIRS ?=

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
	$(foreach includedir,$(INCLUDE_DIRS),-I$(includedir)) \
	${CXXFLAGS}

# --- Link mode (default): link against system-installed libraries ---
LIBS := -lisal -ldeflate -lpthread
LD_FLAGS := $(foreach librarydir,$(LIBRARY_DIRS),-L$(librarydir)) $(LIBS) $(LD_FLAGS)

# --- Static mode: build from submodules and link statically ---
ISAL_LIB := $(DIR_ISAL)/bin/isa-l.a
LIBDEFLATE_LIB := $(DIR_LIBDEFLATE)/build/libdeflate.a

# On Linux: fully static binary; on macOS: static libs, dynamic system runtime
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
  STATIC_LD_FLAGS := -static -Wl,--no-as-needed -lpthread
else
  STATIC_LD_FLAGS := -lpthread
endif

# Default target: link against system libraries
${BIN_TARGET}: ${OBJ}
	$(CXX) $(OBJ) -o $@ $(LD_FLAGS)

# Static target: build deps from submodules and link statically
static: $(ISAL_LIB) $(LIBDEFLATE_LIB) ${OBJ}
	$(CXX) $(OBJ) -o ${BIN_TARGET} $(ISAL_LIB) $(LIBDEFLATE_LIB) $(STATIC_LD_FLAGS)

# Build isa-l static library from submodule
$(ISAL_LIB):
	$(MAKE) -C $(DIR_ISAL) -f Makefile.unx lib

# Build libdeflate static library from submodule
$(LIBDEFLATE_LIB):
	cd $(DIR_LIBDEFLATE) && cmake -B build \
		-DCMAKE_BUILD_TYPE=Release \
		-DLIBDEFLATE_BUILD_SHARED_LIB=OFF \
		-DLIBDEFLATE_BUILD_GZIP=OFF && \
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

.PHONY: clean static install clean-deps

clean:
	@rm -rf $(DIR_OBJ)
	@rm -f $(TARGET)
	@rm -f $(TEST_TARGET)

clean-deps:
	-$(MAKE) -C $(DIR_ISAL) -f Makefile.unx clean 2>/dev/null || true
	-rm -rf $(DIR_LIBDEFLATE)/build 2>/dev/null || true

install:
	install $(TARGET) $(BINDIR)/$(TARGET)
	@echo "Installed."

${DIR_OBJ}/%.o:${DIR_TEST}/%.cpp
	@mkdir -p $(@D)
	$(CXX) -c $< -o $@ $(CXXFLAGS)

test-static: $(ISAL_LIB) $(LIBDEFLATE_LIB) ${TEST_OBJ} ${OBJ}
	@mkdir -p bin
	$(CXX) $(TEST_OBJ) ${OBJ:./obj/main.o=} -o ${TEST_TARGET} $(ISAL_LIB) $(LIBDEFLATE_LIB) $(foreach librarydir,$(LIBRARY_DIRS),-L$(librarydir)) $(STATIC_LD_FLAGS) -lgtest -lgtest_main
	./${TEST_TARGET}

test: ${TEST_OBJ} ${OBJ}
	@mkdir -p bin
	$(CXX) $(TEST_OBJ) ${OBJ:./obj/main.o=} -o ${TEST_TARGET} $(LD_FLAGS) -lgtest -lgtest_main
	./${TEST_TARGET}

-include $(OBJ:.o=.d)
