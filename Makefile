# Build the `bee` interpreter, with an optional LLVM JIT backend.
VERSION  ?= 0.2.0
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pthread
CXXFLAGS += -DBEE_VERSION=\"$(VERSION)\"
LDFLAGS  ?= -pthread
BIN      := bee
SRC      := src/lexer.cpp src/parser.cpp src/resolver.cpp src/interpreter.cpp src/builtins_sys.cpp src/builtins_extra.cpp src/main.cpp

# The `hive` package manager. A separate binary with no LLVM dependency -- it
# shares the repo, not the runtime.
HIVE_BIN := hive
HIVE_SRC := src/hive/json.cpp src/hive/sha256.cpp src/hive/util.cpp src/hive/archive.cpp \
            src/hive/manifest.cpp src/hive/registry.cpp src/hive/commands.cpp src/hive/main.cpp
HIVE_LDFLAGS := -pthread

# ---- optional LLVM JIT ---------------------------------------------------
# If an llvm-config is on PATH, compile the LLVM backend and define BEE_JIT.
# BEE_JIT is defined for *every* translation unit (the layout of the Jit member
# embedded in Interpreter depends on it), but only jit_llvm.cpp includes the
# LLVM headers.
LLVM_CONFIG ?= $(shell which llvm-config-18 llvm-config-17 llvm-config 2>/dev/null | head -n1)

ifneq ($(LLVM_CONFIG),)
  $(info Building with LLVM JIT via $(LLVM_CONFIG))
  CXXFLAGS += -DBEE_JIT
  SRC      += src/jit_llvm.cpp
  LLVM_INC := $(shell $(LLVM_CONFIG) --includedir)
  LDFLAGS  += $(shell $(LLVM_CONFIG) --ldflags) \
              $(shell $(LLVM_CONFIG) --libs core orcjit native passes) \
              $(shell $(LLVM_CONFIG) --system-libs)
  # jit_llvm.cpp needs the LLVM headers, and -fno-rtti to match how LLVM's own
  # libraries are built (LLVM disables RTTI). Exceptions stay enabled.
  src/jit_llvm.o: CXXFLAGS += -I$(LLVM_INC) -fno-rtti
else
  $(info Building interpreter only -- no llvm-config found. Install e.g. llvm-18-dev to enable the JIT.)
endif

OBJ      := $(SRC:.cpp=.o)
HIVE_OBJ := $(HIVE_SRC:.cpp=.o)
DEP      := $(OBJ:.o=.d) $(HIVE_OBJ:.o=.d)

.PHONY: all clean run test

# `make bee` / `make hive` build just one of the two binaries.
all: $(BIN) $(HIVE_BIN)

$(BIN): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(HIVE_BIN): $(HIVE_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(HIVE_LDFLAGS)

# Compile with automatic header-dependency tracking.
src/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# hive gets its own version macro but never the LLVM include path.
src/hive/%.o: src/hive/%.cpp
	$(CXX) $(CXXFLAGS) -DHIVE_VERSION=\"$(VERSION)\" -MMD -MP -c $< -o $@

-include $(DEP)

# `make run FILE=examples/foo.bee`
run: $(BIN)
	./$(BIN) $(FILE)

# Language diagnostics, then hive and module resolution, end to end.
test: $(BIN) $(HIVE_BIN)
	bash tests/lang_test.sh
	bash tests/hive_test.sh

clean:
	rm -f $(OBJ) $(HIVE_OBJ) $(DEP) $(BIN) $(HIVE_BIN)
