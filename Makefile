DIR_INC := ./inc
DIR_SRC := ./src
DIR_OBJ := ./obj

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
INCLUDE_DIRS ?=
LIBRARY_DIRS ?=

SRC_CPP := $(wildcard ${DIR_SRC}/*.cpp)
SRC := $(SRC_CPP)
OBJ_CPP := $(patsubst %.cpp,${DIR_OBJ}/%.o,$(notdir ${SRC_CPP}))
OBJ := $(OBJ_CPP)

TARGET := fastp

BIN_TARGET := ${TARGET}

GIT_COMMIT := $(shell git rev-parse --short HEAD 2>/dev/null || echo "unknown")

CXX ?= g++
CXXFLAGS := -std=c++11 -pthread -g -O3 -MD -MP -I. -I${DIR_INC} $(foreach includedir,$(INCLUDE_DIRS),-I$(includedir)) -DGIT_COMMIT=\"$(GIT_COMMIT)\" ${CXXFLAGS}
LIBS := -lisal -ldeflate -lhwy -lpthread

UNAME_S := $(shell uname -s)
# Require static linkage for non-system third-party libs.
# Keep only system dynamic libs in final binary.
FIND_STATIC = $(firstword $(foreach d,$(LIBRARY_DIRS),$(wildcard $(d)/lib$(1).a)) $(wildcard /usr/local/lib/lib$(1).a /opt/homebrew/lib/lib$(1).a))
REQUIRED_STATIC_LIBS := isal deflate hwy zstd
STATIC_LIBS :=
$(foreach lib,$(REQUIRED_STATIC_LIBS),\
  $(if $(call FIND_STATIC,$(lib)),\
    $(eval STATIC_LIBS += $(call FIND_STATIC,$(lib))),\
    $(error missing required static library: lib$(lib).a (set LIBRARY_DIRS or install static lib))))
LD_FLAGS := $(foreach librarydir,$(LIBRARY_DIRS),-L$(librarydir)) $(STATIC_LIBS) -lpthread $(LD_FLAGS)


${BIN_TARGET}:${OBJ}
	$(CXX) $(OBJ) -o $@ $(LD_FLAGS)

${DIR_OBJ}/%.o:${DIR_SRC}/%.cpp
	@mkdir -p $(@D)
	$(CXX) -c $< -o $@ $(CXXFLAGS)

.PHONY:clean
clean:
	@rm -rf $(DIR_OBJ)
	@rm -f $(TARGET)

install:
	install $(TARGET) $(BINDIR)/$(TARGET)
	@echo "Installed."

-include $(OBJ:.o=.d)
