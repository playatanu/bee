#pragma once
//
// LLVM JIT backend for Bee.
//
// Bee is a dynamically-typed tree-walking interpreter. This backend detects
// functions whose bodies stay inside a *numeric subset* (only numbers/bools,
// arithmetic, comparisons, control flow, and direct self-recursion) and
// compiles them to native code with LLVM's ORCv2 JIT, operating on unboxed
// `double`s. Everything outside the subset keeps running on the interpreter.
//
// The whole thing is gated on BEE_JIT: when LLVM is not available the header
// still compiles, but Jit is an empty stub and getCompiled() always fails,
// so the interpreter behaves exactly as before.
//
#include "ast.hpp"
#include <memory>
#include <unordered_map>

namespace bee {

class Interpreter;

// ABI of a JIT-compiled Bee function.
//
//   args  : pointer to `argc` doubles (the numeric arguments)
//   argc  : number of arguments (always == the function's arity at call time)
//   interp: opaque `Interpreter*`, threaded through for trampolines
//   bail  : out-flag. Set to non-zero if native code gave up (e.g. a Bee
//           runtime error such as division by zero). When set, the caller must
//           ignore the return value and fall back to the interpreter. This is
//           safe precisely because the subset has no observable side effects.
//
// Returns the function's numeric result as a double.
using JitFn = double (*)(const double* args, int argc, void* interp, int* bail);

// Cheap static pre-filter: is this function even a candidate? (plain function,
// no `this`/`super`, no rest parameter). The authoritative eligibility check
// happens inside codegen, which bails on any unsupported construct.
bool jitCandidate(const FunctionStmt* fn);

#ifdef BEE_JIT

class Jit {
public:
    Jit();
    ~Jit();

    // Compile `fn` if eligible, caching the result (including negative results).
    // Returns the native entry point, or nullptr if it could not be compiled.
    JitFn getCompiled(const FunctionStmt* fn, Interpreter& interp);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    // fn -> compiled entry (nullptr sentinel == "tried and cannot compile").
    std::unordered_map<const FunctionStmt*, JitFn> cache_;
    std::unordered_map<const FunctionStmt*, bool>  tried_;
};

#else // !BEE_JIT

// Stub used when the build has no LLVM. Never compiles anything.
class Jit {
public:
    Jit() = default;
    ~Jit() = default;
    JitFn getCompiled(const FunctionStmt*, Interpreter&) { return nullptr; }
};

#endif // BEE_JIT

} // namespace bee
