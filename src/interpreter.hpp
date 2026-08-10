#pragma once
#include "ast.hpp"
#include "value.hpp"
#include "environment.hpp"
#include "jit.hpp"
#include "vm.hpp"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <random>

namespace bee {

// How a statement finished. `return`, `break` and `continue` used to be C++
// exceptions, which cost ~2-4 us each to throw -- 50-100x an entire interpreted
// loop iteration, on paths that are not exceptional at all (every call returns).
// They are ordinary return values now: each statement reports how it left, and
// the enclosing loop or call frame absorbs it. Genuine errors (a Bee `throw`, a
// runtime error) stay exceptions, because those really are rare.
enum class Flow : uint8_t { Normal, Break, Continue, Return };

// A runtime error that already carries its full message *and* its stack trace.
// Native built-ins throw a plain RuntimeError with no location; the interpreter
// catches those at the call site and re-throws them as this, so the location is
// added exactly once no matter how deep the call went.
//
// The parts are kept separate because they have two audiences: what() is what a
// crashing program prints (message + full trace), while brief() is what a Bee
// `catch` binds -- one line, because that value often gets printed as part of a
// larger message.
struct TracedError : RuntimeError {
    std::string message;   // what went wrong, with no prefix or location
    std::string location;  // "file:line", empty when unknown
    std::string trace;     // "  at f()  file:line" rows, innermost first

    explicit TracedError(const std::string& fullText) : RuntimeError(fullText) {}
    TracedError(std::string msg, std::string loc, std::string tr)
        : RuntimeError(compose(msg, tr)),
          message(std::move(msg)), location(std::move(loc)), trace(std::move(tr)) {}

    std::string brief() const {
        if (message.empty()) return what();
        std::string out = "Runtime error: " + message;
        return location.empty() ? out : out + " (" + location + ")";
    }

private:
    static std::string compose(const std::string& msg, const std::string& tr) {
        std::string out = "Runtime error: " + msg;
        return tr.empty() ? out : out + "\n" + tr;
    }
};

// A Bee-level `throw` in flight, carrying the thrown value. Caught by `try`.
// The trace is captured at the throw site, because by the time an uncaught
// throw reaches the top the frames it came through are already gone.
struct BeeThrow {
    Value value;
    std::string trace;
};

// One entry per active Bee call. A frame records where the call was *written*,
// which is what lets a trace show the path taken into the error.
//
// The function is stored as a bare pointer rather than its formatted name: a
// frame is pushed on every call, but the name is only ever read when a trace is
// printed. Building it eagerly cost a string copy per call for nothing. The
// caller holds a reference to the Function for the whole call, so the pointer
// cannot dangle while the frame is live.
struct CallFrame {
    const Function* fn = nullptr;                 // the function being called
    std::shared_ptr<const std::string> callFile;  // file containing the call
    int callLine = 0;
};

// "name", or "Class.name" for a method -- how a function appears in a trace.
std::string functionName(const Function& fn);

// Does `v` satisfy `t`? An undeclared (Any) annotation accepts everything, so
// unannotated code pays nothing. A class annotation accepts instances of that
// class and of anything deriving from it.
bool typeAccepts(const TypeAnn& t, const Value& v);

// The name of a value's type, as a type annotation would spell it -- used to
// say what arrived when a check fails.
std::string typeNameOf(const Value& v);

// Everything entering a Bee call has to do besides binding arguments: check the
// depth limit, record the frame a trace will need, and make errors report the
// callee's file. The VM calls a compiled function directly rather than through
// callFunction, so it needs this as a piece it can hold on its own.
struct CallScope {
    CallScope(Interpreter& I, const std::shared_ptr<Function>& fn, int line);
    ~CallScope();
    CallScope(const CallScope&) = delete;
    CallScope& operator=(const CallScope&) = delete;
private:
    std::shared_ptr<const std::string> savedFile;
};

// Row-major offset for a multi-dimensional buffer index, and the short form a
// buffer prints as. Defined in builtins_buffer.cpp.
size_t bufferOffset(const Buffer& b, const std::vector<Value>& indices, size_t first,
                    const std::string& who);
std::string bufferSummary(const Buffer& b);

// A running (or finished) Bee thread spawned via spawn().
struct ThreadRec {
    std::thread th;
    Value result;
    bool failed = false;
    std::string error;
};

class Interpreter {
public:
    Interpreter();

    // Run a top-level program from `path` (used as the main module).
    void runFile(const std::string& path);

    // Run source that isn't a file on disk -- stdin, or `bee -e`. `name` is what
    // errors call it; `dir` is where `import` starts looking.
    void runSource(const std::string& src, const std::string& name, const std::string& dir);

    // Read-eval-print loop. Each line is a top-level program in its own right,
    // sharing one set of globals, so definitions persist across lines.
    void runRepl();

    // Convert any value to its display string.
    std::string stringify(const Value& v);
    std::string reprString(const Value& v); // quoted form for nested display

    // Call any callable value with already-evaluated arguments.
    Value callValue(const Value& callee, std::vector<Value>& args, int line);

    // Structural equality, and the string form of a dict/index key. Public so
    // native (built-in) functions can reuse the interpreter's own semantics.
    bool valuesEqual(const Value& a, const Value& b);
    std::string keyString(const Value& v);

    // Every binary operator, on two already-evaluated operands. The tree-walker
    // and the bytecode VM share this, so `+` has one definition.
    Value applyBinary(TokenType op, const Value& l, const Value& r, int line);
    // The `+`/`-`/`*`/`/` of a compound assignment, which has its own messages.
    Value applyBinaryArith(TokenType op, const Value& l, const Value& r, int line);
    // `super.name` bound to `self`, given the superclass value.
    Value superMethod(const Value& superV, const Value& self, const std::string& name, int line);

    // Property/index access and assignment, shared with the VM.
    Value getProperty(const Value& object, const std::string& name, int line);
    Value indexGet(const Value& obj, const Value& idx, int line);
    void indexSet(const Value& obj, const Value& idx, const Value& v, int line);
    Value sliceValue(const Value& obj, const Value& start, const Value& end, int line);

    // Raise a runtime error attributed to `line` in the file being executed.
    [[noreturn]] void error(const std::string& msg, int line);

    // Deepest Bee call nesting allowed; see maxCallDepth.
    size_t callDepthLimit() const { return maxCallDepth; }

    // Enforce a declared parameter, return, `let` or assignment type; all
    // no-ops when the thing in question was not annotated.
    void checkParamType(const FunctionStmt& decl, size_t i, const Value& v, int line);
    void checkReturnType(const FunctionStmt& decl, const Value& v, int line);
    void checkDeclared(const TypeAnn& t, const std::string& name, const Value& v, int line);

    // Command-line arguments after the script path (exposed via args()).
    void setScriptArgs(const std::vector<std::string>& a) { scriptArgs = a; }

    // The global interpreter lock (GIL). Held while a thread runs Bee code;
    // blocking built-ins release it so other threads can make progress.
    std::mutex gil;
    void gilRelease() { gil.unlock(); }
    void gilAcquire() { gil.lock(); }

    // Thread registry (used by spawn/join). Accessed only while holding the GIL.
    std::map<double, std::shared_ptr<ThreadRec>> threads;
    double nextThreadId = 1;
    void joinAllThreads();

    // Shared PRNG (seedable via random_seed).
    std::mt19937_64 rng;

    std::shared_ptr<Environment> globals;

    // Optional LLVM JIT for numeric functions. The backend is dlopen'd from
    // libbee_jit.so on first compile; without it every query is a no-op.
    Jit jit;

    // The register VM. Functions it can compile skip the tree-walker entirely;
    // the rest are unaffected.
    Vm vm;

    // Turn a bare message into "Runtime error: <msg>" plus a stack trace ending
    // at `line` of the file currently executing. Public so built-ins that call
    // back into Bee code can report with a location.
    std::string describeError(const std::string& msg, int line) const;

private:
    // ---- Source locations -------------------------------------------------
    // File names are interned: every function and every frame from one file
    // shares a single string, so pushing a frame copies a pointer.
    std::map<std::string, std::shared_ptr<const std::string>> fileNames;
    std::shared_ptr<const std::string> internFile(const std::string& path);

    // "  at f()  file:line" lines, innermost first, for the trace ending at
    // `line` in the file currently executing.
    std::string formatTrace(int line) const;

    // "file:line" for the innermost location, or "" if the file is unknown.
    std::string currentLocation(int line) const;

    // Deepest Bee call nesting allowed. Without a limit, runaway recursion
    // overflows the real C++ stack and the process dies on a signal with no
    // diagnostic at all. Derived from the process's stack limit at startup --
    // see computeMaxCallDepth().
    size_t maxCallDepth = 2000;

    // Module system
    std::map<std::string, std::shared_ptr<Module>> moduleCache; // by resolved path
    std::vector<std::string> searchPaths;
    std::string currentDir;

    // Parsed programs are kept alive here for the interpreter's lifetime, because
    // Function values hold raw `const FunctionStmt*` pointers into these ASTs.
    std::vector<std::unique_ptr<Program>> programStore;

    void defineBuiltins();
    void defineSystemBuiltins();  // file I/O, time, random, env, processes, threads
    void defineExtraBuiltins();   // higher-order collection ops, math, JSON
    void defineBufferBuiltins();  // contiguous typed arrays
    std::vector<std::string> scriptArgs;
    std::shared_ptr<Module> loadModule(const std::string& moduleName, int line);
    // A shared library exporting bee_module_init(); see bee_native.hpp.
    std::shared_ptr<Module> loadNativeModule(const std::string& moduleName,
                                             const std::string& path, int line);
    std::string resolveModulePath(const std::string& moduleName);
    // Loaded native libraries, kept open for the process's lifetime: values the
    // module created hold lambdas whose code lives inside it.
    std::vector<void*> nativeLibraries;

    // Program execution
    Flow execProgram(const Program& program, std::shared_ptr<Environment> env);

    // Statements. Each returns how it finished; see Flow.
    Flow execute(Stmt* stmt, std::shared_ptr<Environment>& env);
    Flow execBlock(const std::vector<StmtPtr>& stmts, std::shared_ptr<Environment> env);

    // The value of the `return` currently propagating. Set when a statement
    // reports Flow::Return, and consumed by the call frame that absorbs it --
    // which happens before any other Bee code runs, so a single slot is enough.
    Value returnValue_;
    // Try to run a top-level numeric loop as native code. Returns true if it did.
    bool tryJitLoop(Stmt* stmt, std::shared_ptr<Environment>& env);
    // If `e` is `x = x + rhs` with x currently a string, return a pointer to x's
    // storage so the concat can grow it in place (else null). Turns the common
    // O(n^2) string-building idiom into O(n).
    Value* selfStringAppend(AssignExpr* e, std::shared_ptr<Environment>& env);
    void execImport(ImportStmt* stmt, std::shared_ptr<Environment>& env);
    void execClass(ClassStmt* stmt, std::shared_ptr<Environment>& env);
    Flow execTry(TryStmt* stmt, std::shared_ptr<Environment>& env);
    Flow runCatch(TryStmt* stmt, const Value& err, std::shared_ptr<Environment>& env);

    // Expressions
    Value evaluate(Expr* expr, std::shared_ptr<Environment>& env);
    Value evalBinary(BinaryExpr* e, std::shared_ptr<Environment>& env);
    Value evalCall(CallExpr* e, std::shared_ptr<Environment>& env);
    Value evalGet(GetExpr* e, std::shared_ptr<Environment>& env);
    Value evalIndex(IndexExpr* e, std::shared_ptr<Environment>& env);
    Value evalSlice(SliceExpr* e, std::shared_ptr<Environment>& env);

    Value callFunction(const std::shared_ptr<Function>& fn, std::vector<Value>& args, int line);
    std::shared_ptr<Function> bindMethod(std::shared_ptr<Function> method,
                                         std::shared_ptr<Instance> self,
                                         std::shared_ptr<Class> definingClass);

};

} // namespace bee
