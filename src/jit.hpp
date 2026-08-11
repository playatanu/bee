#pragma once
//
// LLVM JIT backend for Bee.
//
// Bee is a dynamically-typed tree-walking interpreter. This backend detects
// functions whose bodies stay inside a *numeric subset* (only numbers/bools,
// arithmetic, comparisons, control flow, and calls to other numeric functions
// -- self-recursion, mutual recursion, and helpers) and compiles them to native
// code with LLVM's ORCv2 JIT, operating on unboxed `double`s. A whole numeric
// call graph is compiled into one module so calls are direct and inlinable.
// Everything outside the subset keeps running on the interpreter.
//
// Like the tree-walker's fast path, a compiled call resolves its target from
// the global binding once, at compile time; reassigning a numeric function to a
// different value at runtime is unsupported (compiled callers keep the original
// target).
//
// The backend (jit_llvm.cpp) is compiled into a separate shared object,
// libbee_jit.so, and reached through the JitBackend interface below. The `bee`
// executable links no LLVM at all; the ~120MB library is mapped only when a
// script first compiles something, via a dlopen in jit.cpp. When it is absent
// (no libbee_jit.so, or BEE_NO_JIT=1) every query fails and the interpreter
// behaves exactly as before. This is the same trick beegen uses for libclang.
//
#include "ast.hpp"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace bee {

class Interpreter;

// What a JIT-compiled version assumes about one argument.
//
// The compiler used to accept numbers and nothing else. It now specialises on
// the types a call actually passes -- so a function is compiled per *signature*,
// and re-entering it checks the arguments still match. That check is the guard;
// a mismatch simply means the interpreter runs instead.
enum class ArgKind : uint8_t {
    Num,        // a number, passed unboxed
    BufF64,     // an f64 buffer, passed as a raw pointer and a length
    Other       // anything else: not compiled
};

// A whole argument list, packed two bits per argument so it can key a cache and
// be compared in one instruction. Functions with more arguments than fit are
// simply not compiled.
using JitSig = uint64_t;
static const size_t kMaxJitArgs = 32;

inline JitSig jitSigWith(JitSig s, size_t i, ArgKind k) {
    return s | ((JitSig)k << (i * 2));
}
inline ArgKind jitSigAt(JitSig s, size_t i) {
    return (ArgKind)((s >> (i * 2)) & 3);
}

// ABI of a JIT-compiled Bee function.
//
//   nums   : one double per argument; only the slots the signature marks Num
//   bufs   : one double* per argument; only the slots marked BufF64
//   bufLens: element count for each of those buffers
//   interp : opaque `Interpreter*`, threaded through for trampolines
//   bail   : out-flag, three-valued.
//             0 => success; the return value is the function's numeric result.
//             1 => native code gave up (e.g. a Bee runtime error such as
//                  division by zero, or an out-of-range index); ignore the
//                  result and re-run in the interpreter. Safe because the
//                  compiled subset has no side effects -- it reads buffers and
//                  computes, and never writes anything the caller can see.
//             2 => the function completed with a nil result (a value-less
//                  `return`, or falling off the end); the return value is
//                  unused and the caller yields nil without re-running.
//
// Returns the function's numeric result as a double.
using JitFn = double (*)(const double* nums, double* const* bufs, const long long* bufLens,
                         void* interp, int* bail);

// ABI of a JIT-compiled *top-level loop*. Same shape as JitFn, but `vars` is an
// in/out array: the numeric globals the loop reads/writes, in `globals` order.
// The native code loads them on entry and, on clean completion, writes the
// final values back before returning. On `bail` the array is left untouched and
// the interpreter re-runs the loop from the (unmodified) globals. The return
// value is unused. This is safe because numeric code has no other side effects.
using JitLoopFn = double (*)(double* vars, int nvars, void* interp, int* bail);

// A compiled top-level loop plus the global-variable layout of its `vars` array.
struct CompiledLoop {
    JitLoopFn fn = nullptr;
    std::vector<std::string> globals;   // vars[i] <-> global named globals[i]
};

// Cheap static pre-filter: is this function even a candidate? (plain function,
// no `this`/`super`, no rest parameter). The authoritative eligibility check
// happens inside codegen, which bails on any unsupported construct. Inline so
// both the front end (jit.cpp) and the backend (jit_llvm.cpp) share one copy.
inline bool jitCandidate(const FunctionStmt* fn) {
    // Sized numeric types (i8..u64, f16/f32/f64) carry wrapping/rounding the
    // JIT doesn't model, so it declines them and the tree-walker runs them.
    return fn->restParam < 0 && fn->paramStart == 0 && !fn->usesSized;
}

// One function compiled into a module, handed back for the front end to cache:
// the requested function, or a callee pulled into the same module with it.
struct JitCacheEntry {
    const FunctionStmt* fn;
    JitSig sig;
    JitFn ptr;
};

// The LLVM backend, implemented in libbee_jit.so and reached through dlopen.
// Behind this interface the `bee` executable links no LLVM: the library is
// mapped only when a script first asks for a compile, not on every startup.
struct JitBackend {
    virtual ~JitBackend() = default;

    // Compile `fn` specialised to `sig`. Returns the native entry point, or
    // nullptr if it is not compilable for those argument types. Every function
    // compiled into the module alongside `fn` (its callees) is appended to
    // `extra` so the front end can cache them for later direct calls.
    virtual JitFn compile(const FunctionStmt* fn, JitSig sig, Interpreter& interp,
                          std::vector<JitCacheEntry>& extra) = 0;

    // Compile a top-level while/for loop, filling `out`. On failure out.fn
    // stays nullptr.
    virtual void compileLoop(const Stmt* loop, Interpreter& interp, CompiledLoop& out) = 0;
};

// The factory the shared object exports (C linkage, resolved by dlsym).
extern "C" JitBackend* bee_jit_create();
using BeeJitCreateFn = JitBackend* (*)();

// Front end to the JIT, embedded in the interpreter. It owns the compile caches
// and lazily dlopen's the LLVM backend the first time a function is hot enough
// to compile. Without a backend every query simply fails and execution stays on
// the interpreter/VM, exactly as an interpreter-only build behaves.
class Jit {
public:
    Jit() = default;
    ~Jit();
    Jit(const Jit&) = delete;
    Jit& operator=(const Jit&) = delete;

    // Compile `fn` specialised to `sig` if eligible, caching the result
    // (including negative results). Returns the native entry point, or nullptr
    // if it could not be compiled for those argument types.
    JitFn getCompiled(const FunctionStmt* fn, JitSig sig, Interpreter& interp);

    // Compile a top-level `while`/`for` loop if it stays in the numeric subset.
    // Returns a CompiledLoop with fn==nullptr if it could not be compiled.
    const CompiledLoop& getCompiledLoop(const Stmt* loop, Interpreter& interp);

private:
    // dlopen the backend on first use. Returns nullptr if unavailable; the
    // outcome (including failure) is remembered so it is attempted only once.
    JitBackend* backend();

    void* lib_ = nullptr;            // dlopen handle for libbee_jit.so
    JitBackend* backend_ = nullptr;
    bool triedLoad_ = false;

    // (fn, signature) -> compiled entry. A null entry means "tried, and this
    // function cannot be compiled for those argument types", which is cached
    // too so a failure is not re-attempted on every call.
    struct SigKey {
        const FunctionStmt* fn;
        JitSig sig;
        bool operator==(const SigKey& o) const { return fn == o.fn && sig == o.sig; }
    };
    struct SigHash {
        size_t operator()(const SigKey& k) const {
            return std::hash<const void*>()(k.fn) ^ (std::hash<JitSig>()(k.sig) << 1);
        }
    };
    std::unordered_map<SigKey, JitFn, SigHash> cache_;
    // loop stmt -> compiled loop (fn==nullptr sentinel == "cannot compile").
    std::unordered_map<const Stmt*, CompiledLoop> loopCache_;
};

} // namespace bee
