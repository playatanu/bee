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
//   bail  : out-flag, three-valued.
//             0 => success; the return value is the function's numeric result.
//             1 => native code gave up (e.g. a Bee runtime error such as
//                  division by zero); ignore the result and re-run in the
//                  interpreter. Safe because the subset has no side effects.
//             2 => the function completed with a nil result (a value-less
//                  `return`, or falling off the end); the return value is
//                  unused and the caller yields nil without re-running.
//
// Returns the function's numeric result as a double.
using JitFn = double (*)(const double* args, int argc, void* interp, int* bail);

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

    // Compile a top-level `while`/`for` loop if it stays in the numeric subset.
    // Returns a CompiledLoop with fn==nullptr if it could not be compiled.
    const CompiledLoop& getCompiledLoop(const Stmt* loop, Interpreter& interp);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    // fn -> compiled entry (nullptr sentinel == "tried and cannot compile").
    std::unordered_map<const FunctionStmt*, JitFn> cache_;
    std::unordered_map<const FunctionStmt*, bool>  tried_;
    // loop stmt -> compiled loop (fn==nullptr sentinel == "cannot compile").
    std::unordered_map<const Stmt*, CompiledLoop> loopCache_;
};

#else // !BEE_JIT

// Stub used when the build has no LLVM. Never compiles anything.
class Jit {
public:
    Jit() = default;
    ~Jit() = default;
    JitFn getCompiled(const FunctionStmt*, Interpreter&) { return nullptr; }
    const CompiledLoop& getCompiledLoop(const Stmt*, Interpreter&) {
        static const CompiledLoop none;
        return none;
    }
};

#endif // BEE_JIT

} // namespace bee
