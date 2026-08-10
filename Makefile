# Build the `bee` interpreter, with an optional LLVM JIT backend.
VERSION  ?= 0.3.2
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pthread
CXXFLAGS += -DBEE_VERSION=\"$(VERSION)\"
LDFLAGS  ?= -pthread
# Native modules are dlopen'd at run time (see src/bee_native.hpp). -rdynamic
# exports the interpreter's own symbols to them, which is what lets a module call
# back into BeeLang (Interpreter::callValue) rather than only be called.
ifeq ($(shell uname -s),Linux)
  LDFLAGS += -ldl -rdynamic
endif
ifeq ($(shell uname -s),Darwin)
  LDFLAGS += -rdynamic
endif
BIN      := bee
# src/jit.cpp is the LLVM-free front end: it dlopen's the backend below. The
# `bee` binary links no LLVM at all, so it starts without mapping the library.
SRC      := src/lexer.cpp src/parser.cpp src/resolver.cpp src/compiler.cpp src/vm.cpp \
            src/interpreter.cpp src/builtins_sys.cpp src/builtins_extra.cpp \
            src/builtins_buffer.cpp src/jit.cpp src/main.cpp

# The `hive` package manager. A separate binary with no LLVM dependency -- it
# shares the repo, not the runtime.
HIVE_BIN := hive
HIVE_SRC := src/hive/json.cpp src/hive/sha256.cpp src/hive/util.cpp src/hive/archive.cpp \
            src/hive/manifest.cpp src/hive/registry.cpp src/hive/commands.cpp src/hive/main.cpp
HIVE_LDFLAGS := -pthread

# `beegen`, the binding generator. It reaches libclang through dlopen, so there
# is no build-time dependency on clang headers or libraries.
GEN_BIN := beegen
GEN_SRC := src/beegen/clang.cpp src/beegen/scan.cpp src/beegen/emit.cpp src/beegen/main.cpp
GEN_LDFLAGS := -pthread
ifeq ($(shell uname -s),Linux)
  GEN_LDFLAGS += -ldl
endif

# ---- optional LLVM JIT (a dlopen'd shared object) ------------------------
# The LLVM backend used to be linked straight into `bee`, which made the loader
# map ~120MB of libLLVM on *every* run -- ~12ms of startup for a JIT most
# scripts never trigger. It now lives in its own shared object, libbee_jit.so,
# that src/jit.cpp dlopen's the first time a script compiles something. `bee`
# itself links no LLVM. The .so calls back into the interpreter, resolved at
# load time against `bee`'s -rdynamic symbols -- the native-module mechanism.
#
# If an llvm-config is on PATH, `all` also builds the backend. Without one, only
# the interpreter is built and JIT queries fall back to the interpreter/VM.
UNAME_S := $(shell uname -s)
JIT_LIB := libbee_jit.so
PIC     := -fPIC
# MSYS2/MinGW (uname reports MINGW64_NT.../MSYS_NT...): a DLL, no -fPIC, and LLVM
# folded in statically so the backend is self-contained.
LLVM_LINK :=
ifeq ($(UNAME_S),Darwin)
  JIT_LIB := libbee_jit.dylib
endif
ifneq (,$(filter MINGW% MSYS%,$(UNAME_S)))
  JIT_LIB := bee_jit.dll
  PIC :=
  LLVM_LINK := --link-static
endif

LLVM_CONFIG ?= $(shell which llvm-config-18 llvm-config-17 llvm-config 2>/dev/null | head -n1)

ifneq ($(LLVM_CONFIG),)
  $(info Building LLVM JIT backend ($(JIT_LIB)) via $(LLVM_CONFIG))
  LLVM_INC := $(shell $(LLVM_CONFIG) --includedir)
  LLVM_LIBS := $(shell $(LLVM_CONFIG) --ldflags) \
               $(shell $(LLVM_CONFIG) $(LLVM_LINK) --libs core orcjit native passes) \
               $(shell $(LLVM_CONFIG) --system-libs)
  JIT_TARGET := $(JIT_LIB)
else
  $(info Building interpreter only -- no llvm-config found. Install e.g. llvm-18-dev to enable the JIT.)
  JIT_TARGET :=
endif

OBJ      := $(SRC:.cpp=.o)
HIVE_OBJ := $(HIVE_SRC:.cpp=.o)
GEN_OBJ  := $(GEN_SRC:.cpp=.o)
DEP      := $(OBJ:.o=.d) $(HIVE_OBJ:.o=.d) $(GEN_OBJ:.o=.d)

.PHONY: all clean run test

# `make bee` / `make hive` / `make beegen` build just one of the three binaries;
# $(JIT_TARGET) is the LLVM backend, empty when no llvm-config was found.
all: $(BIN) $(HIVE_BIN) $(GEN_BIN) $(JIT_TARGET)

$(BIN): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# The LLVM backend, built as a shared object and dlopen'd on first compile.
# -fPIC for a shared library; -fno-rtti to match how LLVM's own libraries are
# built (LLVM disables RTTI); exceptions stay on. The interpreter symbols it
# calls back into are left undefined here and resolved from `bee` at load time.
# Only reachable when LLVM_CONFIG is set (else $(JIT_TARGET) is empty).
$(JIT_LIB): src/jit_llvm.cpp $(wildcard src/*.hpp) src/bee_buffer.h
	$(CXX) $(CXXFLAGS) -DBEE_JIT $(PIC) -fno-rtti -I$(LLVM_INC) -shared -o $@ \
	    src/jit_llvm.cpp $(LLVM_LIBS)

$(HIVE_BIN): $(HIVE_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(HIVE_LDFLAGS)

$(GEN_BIN): $(GEN_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(GEN_LDFLAGS)

# Compile with automatic header-dependency tracking.
src/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# hive gets its own version macro but never the LLVM include path.
src/hive/%.o: src/hive/%.cpp
	$(CXX) $(CXXFLAGS) -DHIVE_VERSION=\"$(VERSION)\" -MMD -MP -c $< -o $@

src/beegen/%.o: src/beegen/%.cpp
	$(CXX) $(CXXFLAGS) -DBEEGEN_VERSION=\"$(VERSION)\" -MMD -MP -c $< -o $@

-include $(DEP)

# `make run FILE=examples/foo.bee`
run: $(BIN) $(JIT_TARGET)
	./$(BIN) $(FILE)

# Language diagnostics, the two execution engines against each other, then hive
# and module resolution, end to end.
test: $(BIN) $(HIVE_BIN) $(GEN_BIN) $(JIT_TARGET)
	bash tests/lang_test.sh
	bash tests/vm_diff_test.sh
	bash tests/perf_guard_test.sh
	bash tests/hive_test.sh
	bash tests/beegen_test.sh

# Timings. `make bench REF=/path/to/other/bee` compares two builds, and
# BENCHARGS passes anything else through (--python, -n 9, a name prefix).
BENCHARGS ?=
bench: $(BIN) $(JIT_TARGET)
	bash bench/run.sh $(if $(REF),--vs $(REF)) $(BENCHARGS)

clean:
	rm -f $(OBJ) $(HIVE_OBJ) $(GEN_OBJ) $(DEP) $(BIN) $(HIVE_BIN) $(GEN_BIN) \
	      $(JIT_LIB) libbee_jit.so libbee_jit.dylib
