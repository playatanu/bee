#pragma once
#include "ast.hpp"
#include "value.hpp"
#include "environment.hpp"
#include "jit.hpp"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <random>

namespace bee {

// Control-flow signals implemented as exceptions.
struct ReturnSignal { Value value; };
struct BreakSignal {};
struct ContinueSignal {};

// A Bee-level `throw` in flight, carrying the thrown value. Caught by `try`.
struct BeeThrow { Value value; };

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

    // Convert any value to its display string.
    std::string stringify(const Value& v);
    std::string reprString(const Value& v); // quoted form for nested display

    // Call any callable value with already-evaluated arguments.
    Value callValue(const Value& callee, std::vector<Value>& args, int line);

    // Structural equality, and the string form of a dict/index key. Public so
    // native (built-in) functions can reuse the interpreter's own semantics.
    bool valuesEqual(const Value& a, const Value& b);
    std::string keyString(const Value& v);

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

    // Optional LLVM JIT for numeric functions (a no-op stub without BEE_JIT).
    Jit jit;

private:
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
    std::vector<std::string> scriptArgs;
    std::shared_ptr<Module> loadModule(const std::string& moduleName, int line);
    std::string resolveModulePath(const std::string& moduleName);

    // Program execution
    void execProgram(const Program& program, std::shared_ptr<Environment> env);

    // Statements
    void execute(Stmt* stmt, std::shared_ptr<Environment>& env);
    void execBlock(const std::vector<StmtPtr>& stmts, std::shared_ptr<Environment> env);
    void execImport(ImportStmt* stmt, std::shared_ptr<Environment>& env);
    void execClass(ClassStmt* stmt, std::shared_ptr<Environment>& env);
    void execTry(TryStmt* stmt, std::shared_ptr<Environment>& env);
    void runCatch(TryStmt* stmt, const Value& err, std::shared_ptr<Environment>& env);

    // Expressions
    Value evaluate(Expr* expr, std::shared_ptr<Environment>& env);
    Value evalBinary(BinaryExpr* e, std::shared_ptr<Environment>& env);
    Value applyBinaryArith(TokenType op, const Value& l, const Value& r, int line); // +,-,*,/ for compound assign
    Value evalCall(CallExpr* e, std::shared_ptr<Environment>& env);
    Value evalGet(GetExpr* e, std::shared_ptr<Environment>& env);
    Value evalIndex(IndexExpr* e, std::shared_ptr<Environment>& env);

    Value callFunction(std::shared_ptr<Function> fn, std::vector<Value>& args, int line);
    std::shared_ptr<Function> bindMethod(std::shared_ptr<Function> method,
                                         std::shared_ptr<Instance> self,
                                         std::shared_ptr<Class> definingClass);

    // Helpers
    Value getProperty(const Value& object, const std::string& name, int line);
    [[noreturn]] void error(const std::string& msg, int line);
};

} // namespace bee
