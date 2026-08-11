#pragma once
//
// Runtime support for AOT-compiled Bee programs.
//
// `beec` translates a .bee program into a C++ source file that #includes this
// header and calls the helpers below. Each Bee function becomes a native C++
// lambda wrapped as a first-class callable Value (a Builtin), variables become
// reference-counted `Cell`s so closures capture them correctly, and every
// operation is delegated to the existing Bee runtime (Interpreter::applyBinary,
// callValue, getProperty, ...). The result is real machine code -- no bytecode
// or AST is interpreted at run time -- linked against the runtime as a static
// library to produce a standalone executable.
//
#include "interpreter.hpp"
#include "environment.hpp"
#include "value.hpp"
#include "token.hpp"

#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bee {
namespace aot {

// ---- Variables as reference-counted cells --------------------------------
// A Bee variable is a `shared_ptr<Value>`. A nested function is emitted as a
// C++ lambda with a by-copy capture ([=]) of the cells it uses, so it shares
// the very same Value the enclosing scope does -- which is exactly what a
// closure over a mutable variable needs.
using Cell = std::shared_ptr<Value>;
inline Cell cell()            { return std::make_shared<Value>(); }
inline Cell cell(Value v)     { return std::make_shared<Value>(std::move(v)); }
inline Value  load(const Cell& c)              { return *c; }
inline Value  store(const Cell& c, Value v)    { *c = v; return v; }

// ---- Callables ------------------------------------------------------------
using NativeFn = std::function<Value(Interpreter&, std::vector<Value>&)>;

// Wrap a compiled Bee function (a C++ lambda) as a first-class Value -- a real
// Function with a native body, so it can also serve as a class method (bound
// with a receiver) and go through the same call path as any function. The
// generated body binds parameters and reports arity errors itself, so defaults
// and `...rest` work.
inline Value makeFn(std::string name, NativeFn f) {
    auto fn = std::make_shared<Function>();
    fn->name = std::move(name);
    fn->native = std::move(f);
    return Value(fn);
}

// ---- Classes --------------------------------------------------------------
struct MethodDef { const char* name; Value fn; };

// Build a class Value: its name, an optional superclass (nil if none), and its
// methods (each a makeFn() Value). `init` becomes the constructor.
Value makeClass(Interpreter& I, const char* name, const Value& superclass,
                std::initializer_list<MethodDef> methods, int line);

// `object.name = v` on an instance (fields only live on instances).
Value setProp(Interpreter& I, const Value& object, const char* name, const Value& v, int line);

// ---- Globals (top-level bindings + all built-ins) -------------------------
// Free names that aren't a local/enclosing cell resolve here: the standard
// library and every top-level `let`/`fn`/`class` live in the interpreter's
// global scope.
inline void defineGlobal(Interpreter& I, const char* name, Value v) {
    I.globals->define(name, v);
}
inline Value getGlobal(Interpreter& I, const char* name, int line) {
    Value out;
    if (I.globals->tryGetName(name, out)) return out;
    I.error(std::string("undefined variable '") + name + "'", line);
    return Value();
}
inline Value assignGlobal(Interpreter& I, const char* name, Value v, int line) {
    if (!I.globals->assignName(name, v))
        I.error(std::string("undefined variable '") + name + "'", line);
    return v;
}

// The same three, but against a given named environment -- the global scope for
// the main program, or a module's own top-level scope. A module's environment
// chains to the interpreter's globals, so the standard library resolves through
// it too.
inline void defineIn(Environment& env, const char* name, Value v) { env.define(name, v); }
inline Value getIn(Interpreter& I, Environment& env, const char* name, int line) {
    Value out;
    if (env.tryGetName(name, out)) return out;
    I.error(std::string("undefined variable '") + name + "'", line);
    return Value();
}
inline Value assignIn(Interpreter& I, Environment& env, const char* name, Value v, int line) {
    if (!env.assignName(name, v))
        I.error(std::string("undefined variable '") + name + "'", line);
    return v;
}

// ---- Cached global/module slots -------------------------------------------
// A free name resolving to a global (or module top-level) binding lives in a
// std::map whose node address is stable for the whole run -- globals are never
// erased. slotIn resolves it once; generated code caches the Value* in a
// function-local static and then reads/writes it directly, skipping the
// per-access string hash lookup that getIn/assignIn do on the hot path.
inline Value* slotIn(Interpreter& I, Environment& env, const char* name, int line) {
    Value* p = env.findNameSlot(name);
    if (!p) I.error(std::string("undefined variable '") + name + "'", line);  // [[noreturn]]
    return p;
}
inline Value& slotRef(Interpreter& I, Environment& env, const char* name, int line, Value*& cache) {
    if (!cache) cache = slotIn(I, env, name, line);
    return *cache;
}

// Wrap a value into a sized numeric type (i8..u64, f16/f32/f64) at a typed
// store -- annotated let/assignment/parameter/return. Numbers are coerced with
// the shared TypeAnn::coerce so the result is identical to the tree-walker;
// non-numbers pass through (the interpreter would have rejected them earlier).
inline Value coerceNum(const Value& v, TypeAnn::NumTy t) {
    return v.isNumber() ? Value(TypeAnn::coerce(v.asNumber(), t)) : v;
}

// Coerce a range() bound to a double, reporting exactly what the range built-in
// does when called: the interpreter attaches the call line to a built-in's
// RuntimeError via I.error(), so the native counting-loop specialisation of
// `for x in range()` does the same for byte-for-byte identical diagnostics.
inline double rangeNum(Interpreter& I, const Value& v, int line) {
    if (!v.isNumber()) I.error("range: expected a number", line);   // [[noreturn]]
    return v.asNumber();
}

// Create a fresh module namespace whose parent is the interpreter's globals.
Value makeModule(Interpreter& I, const char* name, std::shared_ptr<Environment>& envOut);

// ---- Operators ------------------------------------------------------------
// Bee's numbers are all doubles, so number-on-number arithmetic and comparison
// have no dynamic dispatch to do -- yet routing every `a + b` through the
// out-of-line applyBinary (in the runtime archive) blocks inlining and dominates
// hot numeric loops. This inline fast path handles two-number operands directly,
// mirroring applyBinary's semantics *exactly* (division/modulo-by-zero errors
// via the same I.error, and the cmp-based comparison so NaN orders identically),
// and falls back to the shared runtime for every other case -- strings, lists,
// equality, bitwise/shift, and any non-number operand. It is a pure speed path:
// no new behaviour, and the optimiser often folds the number check away when an
// operand is a known-numeric value (e.g. a specialised range() induction var).
inline Value binary(Interpreter& I, TokenType op, const Value& l, const Value& r, int line) {
    if (l.isNumber() && r.isNumber()) {
        double a = l.asNumber(), b = r.asNumber();
        switch (op) {
            case TokenType::PLUS:  return Value(a + b);
            case TokenType::MINUS: return Value(a - b);
            case TokenType::STAR:  return Value(a * b);
            case TokenType::SLASH: if (b == 0) I.error("division by zero", line); return Value(a / b);
            case TokenType::PERCENT: if (b == 0) I.error("modulo by zero", line); return Value(beeMod(a, b));
            case TokenType::LT: case TokenType::GT:
            case TokenType::LE: case TokenType::GE: {
                int cmp = (a < b) ? -1 : (a > b) ? 1 : 0;   // NaN -> 0, matching applyBinary
                switch (op) {
                    case TokenType::LT: return Value(cmp < 0);
                    case TokenType::GT: return Value(cmp > 0);
                    case TokenType::LE: return Value(cmp <= 0);
                    default:            return Value(cmp >= 0);   // GE
                }
            }
            default: break;   // EQ/NEQ, bitwise, shifts -> the exact shared path
        }
    }
    return I.applyBinary(op, l, r, line);
}
inline Value lnot(const Value& v) { return Value(!v.truthy()); }
Value neg(Interpreter& I, const Value& r, int line);
Value bnot(Interpreter& I, const Value& r, int line);

// ---- Calls, indexing, properties, slices ----------------------------------
// One arg (or list element) with a flag saying whether it is spread (...x).
struct Arg { Value v; bool spread; };

// Flatten a possibly-spread argument/element list into a plain vector.
std::vector<Value> flatten(Interpreter& I, std::initializer_list<Arg> items, int line);

inline Value call(Interpreter& I, const Value& callee, std::vector<Value> args, int line) {
    return I.callValue(callee, args, line);
}
inline Value list(Interpreter& I, std::initializer_list<Arg> items, int line) {
    return Value(std::make_shared<ValueList>(flatten(I, items, line)));
}
inline Value dict(Interpreter& I, std::initializer_list<std::pair<Value, Value>> entries) {
    auto d = std::make_shared<ValueDict>();
    for (auto& kv : entries) (*d)[I.keyString(kv.first)] = kv.second;
    return Value(d);
}
inline Value index(Interpreter& I, const Value& o, const Value& i, int line) {
    return I.indexGet(o, i, line);
}
inline Value indexSet(Interpreter& I, const Value& o, const Value& i, const Value& v, int line) {
    I.indexSet(o, i, v, line);
    return v;
}
inline Value getProp(Interpreter& I, const Value& o, const char* name, int line) {
    return I.getProperty(o, name, line);
}
inline Value slice(Interpreter& I, const Value& o, const Value& a, const Value& b, int line) {
    return I.sliceValue(o, a, b, line);
}

// ---- Iteration (for .. in) -------------------------------------------------
// Normalise an iterable to a list, matching the VM's ITER_PREP: a list is
// iterated live, a string yields its characters, a dict yields its keys.
std::shared_ptr<ValueList> iterate(Interpreter& I, const Value& v, int line);

// ---- Errors ----------------------------------------------------------------
[[noreturn]] void doThrow(Interpreter& I, const Value& v);

// Runs a block on every exit from its scope -- normal, exception, return,
// break or continue -- which is precisely `finally`.
struct Finally {
    std::function<void()> f;
    explicit Finally(std::function<void()> fn) : f(std::move(fn)) {}
    ~Finally() { f(); }
    Finally(const Finally&) = delete;
    Finally& operator=(const Finally&) = delete;
};

// ---- Program driver --------------------------------------------------------
// Sets up an Interpreter (with the whole standard library), holds the GIL for
// the run, executes `body`, joins spawned threads, and reports an uncaught
// error the same way the interpreter's top level does. Returns a process exit
// code.
int run(int argc, char** argv, const char* sourceName,
        const std::function<void(Interpreter&)>& body);

} // namespace aot
} // namespace bee
