#include "interpreter.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "resolver.hpp"
#include "bee_native.hpp"

#include <iostream>
#include <fstream>
#include <sstream>
#include <cmath>
#include <iomanip>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <deque>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/resource.h>
#endif

namespace bee {

// ------------------------------------------------------------------
// File-scope helpers
// ------------------------------------------------------------------

static std::string numToStr(double d) {
    if (std::isnan(d)) return "nan";
    if (std::isinf(d)) return d < 0 ? "-inf" : "inf";
    if (d == 0) d = 0; // normalise -0
    double r = std::floor(d);
    if (d == r && std::fabs(d) < 1e15) {
        long long i = (long long)d;
        return std::to_string(i);
    }
    std::ostringstream os;
    os << std::setprecision(15) << d;
    return os.str();
}

static std::string dirOf(const std::string& path) {
    size_t p = path.find_last_of("/\\");
    if (p == std::string::npos) return ".";
    return path.substr(0, p);
}

static bool isRegularFile(const std::string& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

static bool isDirectory(const std::string& path) {
    std::error_code ec;
    return std::filesystem::is_directory(path, ec);
}

static bool readFileContents(const std::string& path, std::string& out) {
    if (!isRegularFile(path)) return false; // a directory must not read as empty
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// The extension a native module uses on this platform. Native modules are
// looked up like any other module, so `import sqlite` finds sqlite.so next to
// the script or inside an installed package.
static const char* nativeExtension() {
#ifdef _WIN32
    return ".dll";
#elif defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}

static bool isNativePath(const std::string& path) {
    const std::string ext = nativeExtension();
    return path.size() > ext.size() &&
           path.compare(path.size() - ext.size(), ext.size(), ext) == 0;
}

// ---- Installed packages (see the `hive` package manager) -------------------
// `hive` owns these directories; the interpreter only reads them. The layout is
// the whole contract: <root>/<package>/ holding the package's files and its
// hive.json, whose "main" names the entry module.

static std::string envValue(const char* name) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : std::string();
}

// $HIVE_HOME/lib, else ~/.hive/lib -- where `hive install -g` puts packages.
static std::string hiveGlobalLib() {
    std::string home = envValue("HIVE_HOME");
    if (home.empty()) {
        home = envValue("HOME");
#ifdef _WIN32
        if (home.empty()) home = envValue("USERPROFILE");
#endif
        if (home.empty()) return "";
        home += "/.hive";
    }
    return home + "/lib";
}

// BEE_PATH is a PATH-style list of extra module roots.
static std::vector<std::string> beePathRoots() {
    std::vector<std::string> roots;
    const std::string list = envValue("BEE_PATH");
#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif
    size_t start = 0;
    while (start <= list.size()) {
        size_t at = list.find(sep, start);
        std::string part = list.substr(start, at == std::string::npos ? std::string::npos : at - start);
        if (!part.empty()) roots.push_back(part);
        if (at == std::string::npos) break;
        start = at + 1;
    }
    return roots;
}

// The "main" entry from a package's hive.json. Read with a targeted scan rather
// than a full JSON parse: a malformed or exotic manifest just falls back to the
// default entry names instead of failing the import.
static std::string packageMainEntry(const std::string& pkgDir) {
    std::string text;
    if (!readFileContents(pkgDir + "/hive.json", text)) return "";
    const std::string key = "\"main\"";
    size_t k = text.find(key);
    if (k == std::string::npos) return "";
    size_t colon = text.find(':', k + key.size());
    if (colon == std::string::npos) return "";
    size_t open = text.find('"', colon + 1);
    if (open == std::string::npos) return "";
    std::string out;
    for (size_t i = open + 1; i < text.size(); ++i) {
        char c = text[i];
        if (c == '\\' && i + 1 < text.size()) { out += text[++i]; continue; }
        if (c == '"') return out;
        if (c == '\n') break;
        out += c;
    }
    return "";
}

// Entry module of an installed package directory, or "" if it has none.
static std::string packageEntryPath(const std::string& pkgDir, const std::string& lastName) {
    std::vector<std::string> candidates;
    std::string main = packageMainEntry(pkgDir);
    // Keep a manifest from pointing outside its own package.
    if (!main.empty() && main.find("..") == std::string::npos && main[0] != '/')
        candidates.push_back(main);
    candidates.push_back("init.bee");
    candidates.push_back("init.be");
    candidates.push_back(lastName + ".bee");
    candidates.push_back(lastName + ".be");
    candidates.push_back(std::string(lastName) + nativeExtension());
    candidates.push_back("main.bee");

    for (auto& c : candidates) {
        std::string full = pkgDir + "/" + c;
        if (isRegularFile(full)) return full;
    }
    return "";
}

static double numArg(const Value& v, const std::string& who) {
    if (!v.isNumber()) throw RuntimeError(who + ": expected a number");
    return v.asNumber();
}

static Value nativeMethod(const std::string& name, int arity,
                          std::function<Value(Interpreter&, std::vector<Value>&)> f) {
    auto b = std::make_shared<Builtin>();
    b->name = name;
    b->arity = arity;
    b->fn = std::move(f);
    return Value(b);
}

// ------------------------------------------------------------------
// Construction / errors
// ------------------------------------------------------------------

// A Bee call costs roughly 2.2 KB of C++ stack (an Environment, the evaluate()
// recursion for the body, and callFunction's own locals). Rather than hard-code
// a depth, scale it to the stack this process actually has and keep a wide
// margin, so `ulimit -s` genuinely buys deeper recursion.
static size_t computeMaxCallDepth() {
    if (const char* v = std::getenv("BEE_MAX_DEPTH")) {
        long n = std::strtol(v, nullptr, 10);
        if (n > 0) return (size_t)n;   // explicit wins: the user knows their stack
    }

    size_t stackBytes = 8u * 1024 * 1024;   // assumed when we can't ask
#ifndef _WIN32
    struct rlimit rl;
    if (getrlimit(RLIMIT_STACK, &rl) == 0) {
        if (rl.rlim_cur == RLIM_INFINITY) stackBytes = 64u * 1024 * 1024;
        else if (rl.rlim_cur > 0) stackBytes = (size_t)rl.rlim_cur;
    }
#endif
    const size_t perFrame = 2560;                       // measured, rounded up
    size_t depth = (stackBytes / 10 * 7) / perFrame;     // spend 70% of it
    if (depth < 256) depth = 256;
    if (depth > 200000) depth = 200000;
    return depth;
}

Interpreter::Interpreter() {
    maxCallDepth = computeMaxCallDepth();
    globals = std::make_shared<Environment>();
    rng.seed(std::random_device{}());
    defineBuiltins();
    defineSystemBuiltins();
    defineExtraBuiltins();
    defineBufferBuiltins();
}

void Interpreter::joinAllThreads() {
    std::vector<std::shared_ptr<ThreadRec>> recs;
    for (auto& kv : threads) recs.push_back(kv.second);
    threads.clear();
    gilRelease();
    for (auto& r : recs)
        if (r->th.joinable()) r->th.join();
    gilAcquire();
}

// ------------------------------------------------------------------
// Source locations and stack traces
// ------------------------------------------------------------------
// Which file is executing, and how we got here. Both are per-thread: spawned
// threads run Bee code under the GIL but have their own call chain, so keeping
// these thread-local stops one thread's frames appearing in another's trace.
static thread_local std::shared_ptr<const std::string> tlsCurrentFile;
static thread_local std::vector<CallFrame> tlsCallStack;

// ---- environment and argument recycling -----------------------------------
// A call used to allocate its frame (a shared_ptr control block plus the slots
// vector) and throw it away on return. Almost every frame dies at the end of the
// call that made it -- only a closure keeps one alive -- so frames are recycled
// through a small free list instead, and the slots vector keeps its capacity.
// Both pools are thread-local: Bee code only runs while holding the GIL, but
// keeping them per-thread means no assumption about that is baked in here.
static thread_local std::vector<std::shared_ptr<Environment>> tlsEnvPool;
static thread_local std::vector<std::vector<Value>> tlsArgPool;
static const size_t kPoolLimit = 64;

static std::shared_ptr<Environment> acquireEnv(const std::shared_ptr<Environment>& parent,
                                               int slotCount) {
    if (!tlsEnvPool.empty()) {
        auto e = std::move(tlsEnvPool.back());
        tlsEnvPool.pop_back();
        e->parent = parent;
        e->slots.assign((size_t)slotCount, Value());   // reuses the existing capacity
        return e;
    }
    return std::make_shared<Environment>(parent, slotCount);
}

// Give a frame back, but only if nothing else still points at it -- a closure
// created during the call holds a reference, and that frame has to keep living.
static void recycleEnv(std::shared_ptr<Environment>& e) {
    if (!e || e.use_count() != 1 || tlsEnvPool.size() >= kPoolLimit) return;
    e->parent.reset();                    // don't pin the parent chain alive
    for (auto& v : e->slots) v = Value();  // drop the values' references now
    if (!e->values.empty()) e->values.clear();
    tlsEnvPool.push_back(std::move(e));
}

void Interpreter::aotSetCurrentFile(const std::string& path) {
    tlsCurrentFile = internFile(path);
}

// Swap in a file (and restore the previous one) for the duration of a scope.
namespace {
struct FileScope {
    std::shared_ptr<const std::string> saved;
    explicit FileScope(std::shared_ptr<const std::string> file) : saved(tlsCurrentFile) {
        if (file) tlsCurrentFile = std::move(file);
    }
    ~FileScope() { tlsCurrentFile = std::move(saved); }
    FileScope(const FileScope&) = delete;
    FileScope& operator=(const FileScope&) = delete;
};
}

// ---- gradual types --------------------------------------------------------

std::string typeNameOf(const Value& v) {
    if (v.isNil())      return "nil";
    if (v.isBool())     return "bool";
    if (v.isNumber())   return "num";
    if (v.isString())   return "str";
    if (v.isList())     return "list";
    if (v.isDict())     return "dict";
    if (v.isBuffer())   return "buffer";
    if (v.isInstance()) return v.asInstance()->klass->name;
    if (v.isClass())    return "class";
    if (v.isModule())   return "module";
    return "fn";   // functions and built-ins are both callable values
}

bool typeAccepts(const TypeAnn& t, const Value& v) {
    switch (t.kind) {
        case TypeAnn::Kind::Any:    return true;
        case TypeAnn::Kind::Num:    return v.isNumber();
        case TypeAnn::Kind::Str:    return v.isString();
        case TypeAnn::Kind::Bool:   return v.isBool();
        case TypeAnn::Kind::List:   return v.isList();
        case TypeAnn::Kind::Dict:   return v.isDict();
        case TypeAnn::Kind::Buffer: return v.isBuffer();
        case TypeAnn::Kind::Nil:    return v.isNil();
        case TypeAnn::Kind::Fn:     return v.isFunction() || v.isBuiltin();
        case TypeAnn::Kind::Class: {
            if (!v.isInstance()) return false;
            // A derived instance satisfies a base class annotation.
            for (Class* k = v.asInstance()->klass.get(); k; k = k->superclass.get())
                if (k->name == t.className) return true;
            return false;
        }
    }
    return true;
}

void Interpreter::checkParamType(const FunctionStmt& decl, size_t i, const Value& v, int line) {
    if (i >= decl.paramTypes.size()) return;
    const TypeAnn& t = decl.paramTypes[i];
    if (!t.declared() || typeAccepts(t, v)) return;
    error("parameter '" + decl.params[i] + "' of '" +
          (decl.name.empty() ? std::string("<anonymous>") : decl.name) + "' is declared " +
          t.name() + " but got " + typeNameOf(v), line);
}

void Interpreter::checkDeclared(const TypeAnn& t, const std::string& name, const Value& v,
                                int line) {
    if (!t.declared() || typeAccepts(t, v)) return;
    error("'" + name + "' is declared " + t.name() + " but got " + typeNameOf(v), line);
}

void Interpreter::checkReturnType(const FunctionStmt& decl, const Value& v, int line) {
    if (!decl.returnType.declared() || typeAccepts(decl.returnType, v)) return;
    error("'" + (decl.name.empty() ? std::string("<anonymous>") : decl.name) +
          "' is declared to return " + decl.returnType.name() + " but returned " +
          typeNameOf(v), line);
}

// Wrap a number into a sized numeric annotation (i8..u64, f16/f32/f64) -- the
// shared coercion every engine agrees on. A no-op for `num`/non-sized types or
// a non-number (which checkDeclared/checkParamType would already have rejected).
Value Interpreter::coerceToType(const TypeAnn& t, const Value& v) {
    if (t.isSizedNum() && v.isNumber()) return Value(TypeAnn::coerce(v.asNumber(), t.num));
    return v;
}

// ---- shapes ---------------------------------------------------------------
// Shapes live for the process's lifetime: there is one per distinct sequence of
// field names, which the program text bounds, and instances point at them
// without owning them. A deque is used because it never moves what it holds, so
// a Shape* stays valid as more shapes appear.
namespace {
std::deque<Shape>& shapeArena() {
    static std::deque<Shape> arena;
    return arena;
}
}

Shape* emptyShape() {
    static Shape* root = [] {
        shapeArena().emplace_back();
        return &shapeArena().back();
    }();
    return root;
}

Shape* Shape::with(const std::string& n) {
    auto it = transitions.find(n);
    if (it != transitions.end()) return it->second;   // this chain was walked before

    shapeArena().emplace_back();
    Shape* next = &shapeArena().back();
    next->index = index;
    next->names = names;
    next->index[n] = (int)next->names.size();
    next->names.push_back(n);
    transitions[n] = next;
    return next;
}

CallScope::CallScope(Interpreter& I, const std::shared_ptr<Function>& fn, int line)
    : savedFile(tlsCurrentFile) {
    if (tlsCallStack.size() >= I.callDepthLimit())
        I.error("call stack overflow in '" + functionName(*fn) + "' (deeper than " +
                std::to_string(I.callDepthLimit()) + " nested calls) -- unbounded recursion?\n"
                "       if the depth is intentional, raise it with BEE_MAX_DEPTH "
                "(and the stack with 'ulimit -s')", line);
    tlsCallStack.push_back({fn.get(), tlsCurrentFile, line});
    if (fn->file) tlsCurrentFile = fn->file;
}

CallScope::~CallScope() {
    tlsCallStack.pop_back();
    tlsCurrentFile = std::move(savedFile);
}

std::shared_ptr<const std::string> Interpreter::internFile(const std::string& path) {
    auto it = fileNames.find(path);
    if (it != fileNames.end()) return it->second;
    // Module paths are built from the importing file's directory, which is "."
    // for a script run from its own folder -- "./util.bee" reads as noise.
    std::string display = path;
    while (display.rfind("./", 0) == 0) display.erase(0, 2);
    return fileNames.emplace(path, std::make_shared<const std::string>(display)).first->second;
}

std::string functionName(const Function& fn) {
    const FunctionStmt* decl = fn.decl;
    std::string name = !fn.name.empty() ? fn.name
                     : (decl && !decl->name.empty() ? decl->name : "<anonymous>");
    // Qualify methods with their class: two classes can both have `draw`, and a
    // trace saying which one is running is the whole point.
    if (fn.definingClass && !fn.definingClass->name.empty())
        name = fn.definingClass->name + "." + name;
    return name;
}

std::string Interpreter::formatTrace(int line) const {
    auto place = [](const std::shared_ptr<const std::string>& f, int ln) {
        if (!f) return std::string("<unknown>");
        return ln > 0 ? *f + ":" + std::to_string(ln) : *f;
    };

    // Walk outwards. The innermost row is where the error happened; each row
    // above it sits at the call site recorded by the frame below.
    struct Row { std::string name, where; };
    std::vector<Row> rows;
    auto file = tlsCurrentFile;
    int ln = line;
    for (size_t i = tlsCallStack.size(); i-- > 0;) {
        const Function* f = tlsCallStack[i].fn;
        rows.push_back({(f ? functionName(*f) : std::string("<fn>")) + "()", place(file, ln)});
        file = tlsCallStack[i].callFile;
        ln = tlsCallStack[i].callLine;
    }
    // The outermost frame is a script's top level -- or a thread's, whose
    // spawn site is in another stack we can no longer point at.
    rows.push_back({file ? "<main>" : "<thread>", place(file, ln)});

    size_t width = 0;
    for (auto& r : rows) width = std::max(width, r.name.size());

    // Runaway recursion would otherwise print thousands of identical lines.
    const size_t kHead = 12, kTail = 3;
    std::string out;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows.size() > kHead + kTail + 1 && i == kHead) {
            out += "  ... " + std::to_string(rows.size() - kHead - kTail) + " more frames ...\n";
            i = rows.size() - kTail - 1;   // the loop's ++i lands on the tail
            continue;
        }
        out += "  at " + rows[i].name + std::string(width - rows[i].name.size(), ' ') +
               "  " + rows[i].where + "\n";
    }
    if (!out.empty()) out.pop_back();
    return out;
}

std::string Interpreter::currentLocation(int line) const {
    if (!tlsCurrentFile) return "";
    return line > 0 ? *tlsCurrentFile + ":" + std::to_string(line) : *tlsCurrentFile;
}

std::string Interpreter::describeError(const std::string& msg, int line) const {
    return "Runtime error: " + msg + "\n" + formatTrace(line);
}

void Interpreter::error(const std::string& msg, int line) {
    throw TracedError(msg, currentLocation(line), formatTrace(line));
}

std::string Interpreter::keyString(const Value& v) {
    return v.isString() ? v.asString() : stringify(v);
}

// ------------------------------------------------------------------
// Display
// ------------------------------------------------------------------

std::string Interpreter::stringify(const Value& v) {
    if (v.isNil())    return "nil";
    if (v.isBool())   return v.asBool() ? "true" : "false";
    if (v.isNumber()) return numToStr(v.asNumber());
    if (v.isString()) return v.asString();
    if (v.isList()) {
        auto l = v.asList();
        std::string s = "[";
        for (size_t i = 0; i < l->size(); ++i) {
            if (i) s += ", ";
            s += reprString((*l)[i]);
        }
        return s + "]";
    }
    if (v.isDict()) {
        auto d = v.asDict();
        std::string s = "{";
        bool first = true;
        for (auto& kv : *d) {
            if (!first) s += ", ";
            first = false;
            s += reprString(Value(kv.first));
            s += ": ";
            s += reprString(kv.second);
        }
        return s + "}";
    }
    if (v.isFunction()) {
        auto f = v.asFunction();
        std::string n = !f->name.empty() ? f->name : (f->decl ? f->decl->name : "");
        return "<fn " + n + ">";
    }
    if (v.isBuiltin()) return "<builtin " + v.asBuiltin()->name + ">";
    if (v.isBuffer()) return bufferSummary(*v.asBuffer());
    if (v.isClass())   return "<class " + v.asClass()->name + ">";
    if (v.isInstance()) {
        auto inst = v.asInstance();
        auto m = inst->klass->findMethod("str");
        if (m) {
            auto bound = bindMethod(m, inst, inst->klass);
            std::vector<Value> noargs;
            Value r = callFunction(bound, noargs, 0);
            if (r.isString()) return r.asString();
        }
        return "<" + inst->klass->name + " instance>";
    }
    if (v.isModule()) return "<module " + v.asModule()->name + ">";
    return "?";
}

std::string Interpreter::reprString(const Value& v) {
    if (v.isString()) {
        std::string s = "\"";
        for (char c : v.asString()) {
            switch (c) {
                case '\\': s += "\\\\"; break;
                case '"':  s += "\\\""; break;
                case '\n': s += "\\n"; break;
                case '\t': s += "\\t"; break;
                case '\r': s += "\\r"; break;
                default:   s += c;
            }
        }
        return s + "\"";
    }
    return stringify(v);
}

// ------------------------------------------------------------------
// Equality
// ------------------------------------------------------------------

bool Interpreter::valuesEqual(const Value& a, const Value& b) {
    if (a.isNil() && b.isNil())       return true;
    if (a.isBool() && b.isBool())     return a.asBool() == b.asBool();
    if (a.isNumber() && b.isNumber()) return a.asNumber() == b.asNumber();
    if (a.isString() && b.isString()) return a.asString() == b.asString();
    if (a.isList() && b.isList()) {
        auto x = a.asList(), y = b.asList();
        if (x == y) return true;
        if (x->size() != y->size()) return false;
        for (size_t i = 0; i < x->size(); ++i)
            if (!valuesEqual((*x)[i], (*y)[i])) return false;
        return true;
    }
    if (a.isDict() && b.isDict()) {
        auto x = a.asDict(), y = b.asDict();
        if (x == y) return true;
        if (x->size() != y->size()) return false;
        for (auto& kv : *x) {
            auto it = y->find(kv.first);
            if (it == y->end() || !valuesEqual(kv.second, it->second)) return false;
        }
        return true;
    }
    if (a.isBuffer() && b.isBuffer()) {
        auto x = a.asBuffer(), y = b.asBuffer();
        if (x == y) return true;
        return x->dtype == y->dtype && x->shape == y->shape && x->bytes == y->bytes;
    }
    if (a.isInstance() && b.isInstance()) return a.asInstance() == b.asInstance();
    if (a.isClass() && b.isClass())       return a.asClass() == b.asClass();
    if (a.isFunction() && b.isFunction()) return a.asFunction() == b.asFunction();
    if (a.isBuiltin() && b.isBuiltin())   return a.asBuiltin() == b.asBuiltin();
    if (a.isModule() && b.isModule())     return a.asModule() == b.asModule();
    return false;
}

// ------------------------------------------------------------------
// Program / statement execution
// ------------------------------------------------------------------

void Interpreter::runFile(const std::string& path) {
    std::string src;
    if (!readFileContents(path, src))
        throw RuntimeError("cannot open file '" + path + "'");
    runSource(src, path, dirOf(path));
}

void Interpreter::runSource(const std::string& src, const std::string& name,
                            const std::string& dir) {
    const std::string& path = name;
    currentDir = dir;
    searchPaths.clear();
    searchPaths.push_back(currentDir);
    searchPaths.push_back(currentDir + "/lib");

    auto file = internFile(path);
    tlsCurrentFile = file;

    auto program = std::make_unique<Program>();
    try {
        Lexer lx(src);
        auto toks = lx.tokenize();
        Parser ps(std::move(toks));
        *program = ps.parse();
    } catch (const LexError& e) {
        throw TracedError("Lex error: " + e.message + "\n  at " + path + ":" + std::to_string(e.line));
    } catch (const ParseError& e) {
        throw TracedError("Parse error: " + e.message + "\n  at " + path + ":" + std::to_string(e.line));
    }
    Resolver().resolve(*program);
    Program* prog = program.get();
    programStore.push_back(std::move(program));

    // Hold the GIL for the whole main-thread run; spawned threads and blocking
    // built-ins hand it back and forth. `guard` guarantees we join outstanding
    // threads and release the lock however we leave this function.
    gil.lock();
    struct Guard {
        Interpreter* it;
        ~Guard() { it->joinAllThreads(); it->gil.unlock(); }
    } guard{ this };

    try {
        // A top-level `return` just ends the program; a stray break/continue is
        // an error, because there is no loop here to absorb it.
        Flow f = execProgram(*prog, globals);
        if (f == Flow::Break)
            throw TracedError("Runtime error: 'break' used outside of a loop");
        if (f == Flow::Continue)
            throw TracedError("Runtime error: 'continue' used outside of a loop");
    } catch (BeeThrow& t) {
        // The trace was captured where the throw happened; those frames are
        // already unwound by the time we get here.
        std::string msg = "Uncaught: " + stringify(t.value);
        throw TracedError(t.trace.empty() ? msg : msg + "\n" + t.trace);
    }
}

// ------------------------------------------------------------------
// REPL
// ------------------------------------------------------------------
void Interpreter::runRepl() {
    currentDir = ".";
    searchPaths.clear();
    searchPaths.push_back(currentDir);
    searchPaths.push_back(currentDir + "/lib");
    tlsCurrentFile = internFile("<repl>");

    gil.lock();
    struct Guard {
        Interpreter* it;
        ~Guard() { it->joinAllThreads(); it->gil.unlock(); }
    } guard{ this };

    std::string pending;   // accumulates across lines while input is incomplete
    while (true) {
        std::cout << (pending.empty() ? ">>> " : "... ") << std::flush;
        std::string line;
        if (!std::getline(std::cin, line)) {   // Ctrl-D
            std::cout << "\n";
            break;
        }
        if (pending.empty()) {
            if (line == "exit" || line == "quit" || line == ":q") break;
            if (line.find_first_not_of(" \t") == std::string::npos) continue;
        }
        pending += line;
        pending += "\n";

        auto program = std::make_unique<Program>();
        try {
            Lexer lx(pending);
            auto toks = lx.tokenize();
            Parser ps(std::move(toks));
            *program = ps.parse();
        } catch (const ParseError& e) {
            // Ran out of input rather than hit something wrong: the user is
            // mid-block, so prompt for more instead of complaining.
            if (e.atEnd) continue;
            std::cout << "Parse error: " << e.message << "\n";
            pending.clear();
            continue;
        } catch (const LexError& e) {
            // An unterminated string or f-string is the same situation.
            if (e.message.rfind("unterminated", 0) == 0) continue;
            std::cout << "Lex error: " << e.message << "\n";
            pending.clear();
            continue;
        }
        pending.clear();

        try {
            Resolver().resolve(*program);
            Program* prog = program.get();
            programStore.push_back(std::move(program));

            auto env = globals;
            for (auto& stmt : *prog) {
                // Echo the value of a bare expression -- that's what makes a
                // REPL worth using -- but stay quiet for statements and nil.
                if (stmt->kind == Stmt::Kind::Expression) {
                    Value v = evaluate(static_cast<ExprStmt*>(stmt.get())->expr.get(), env);
                    if (!v.isNil()) std::cout << reprString(v) << "\n";
                    continue;
                }
                Flow f = execute(stmt.get(), env);
                if (f == Flow::Return)
                    std::cout << "Runtime error: 'return' outside of a function\n";
                else if (f == Flow::Break)
                    std::cout << "Runtime error: 'break' outside of a loop\n";
                else if (f == Flow::Continue)
                    std::cout << "Runtime error: 'continue' outside of a loop\n";
                if (f != Flow::Normal) break;
            }
        } catch (BeeThrow& t) {
            std::cout << "Uncaught: " << stringify(t.value) << "\n";
            if (!t.trace.empty()) std::cout << t.trace << "\n";
        } catch (const std::exception& e) {
            std::cout << e.what() << "\n";
        }
        // A failed line must not leave frames behind for the next one.
        tlsCallStack.clear();
        tlsCurrentFile = internFile("<repl>");
    }
}

Flow Interpreter::execProgram(const Program& program, std::shared_ptr<Environment> env) {
    for (auto& s : program) {
        Flow f = execute(s.get(), env);
        if (f != Flow::Normal) return f;
    }
    return Flow::Normal;
}

Flow Interpreter::execBlock(const std::vector<StmtPtr>& stmts, std::shared_ptr<Environment> env) {
    for (auto& s : stmts) {
        Flow f = execute(s.get(), env);
        if (f != Flow::Normal) return f;   // stop here; the caller decides what it means
    }
    return Flow::Normal;
}

// Below this many iterations, interpreting a flat loop is cheaper than paying
// the one-time (~3 ms) native compilation, so we don't JIT it.
static const long kJitLoopMinTrips = 40000;

static bool litNum(Expr* e, double& out) {
    if (e && e->kind == Expr::Kind::Literal) {
        auto* l = static_cast<LiteralExpr*>(e);
        if (l->value.isNumber()) { out = l->value.asNumber(); return true; }
    }
    return false;
}

// Exact trip count of a simple counting `for (let i = c; i </<= c; i += c)`, or
// -1 if the bounds aren't compile-time literals. Used only as a cost heuristic,
// so a wrong/absent estimate affects speed, never correctness.
static long estimateForTrips(ForStmt* s) {
    if (!s->init || !s->condition || !s->increment) return -1;
    if (s->init->kind != Stmt::Kind::Let) return -1;
    auto* let = static_cast<LetStmt*>(s->init.get());
    if (let->isDestructure || !let->initializer) return -1;
    double lo; if (!litNum(let->initializer.get(), lo)) return -1;

    if (s->condition->kind != Expr::Kind::Binary) return -1;
    auto* c = static_cast<BinaryExpr*>(s->condition.get());
    if (c->op != TokenType::LT && c->op != TokenType::LE) return -1;
    if (c->left->kind != Expr::Kind::Variable) return -1;
    auto* cv = static_cast<VariableExpr*>(c->left.get());
    if (cv->global != (let->slot < 0) || cv->slot != let->slot) return -1;
    double hi; if (!litNum(c->right.get(), hi)) return -1;

    if (s->increment->kind != Expr::Kind::Assign) return -1;
    auto* a = static_cast<AssignExpr*>(s->increment.get());
    if (a->slot != let->slot || !a->value || a->value->kind != Expr::Kind::Binary) return -1;
    auto* bin = static_cast<BinaryExpr*>(a->value.get());
    double step; if (bin->op != TokenType::PLUS || !litNum(bin->right.get(), step)) return -1;
    if (step <= 0) return -1;

    double span = (c->op == TokenType::LT) ? (hi - lo) : (hi - lo + 1);
    if (span <= 0) return 0;
    return (long)((span + step - 1) / step);
}

// Does this statement (a loop body) contain a nested loop? A nested loop makes
// the total work large, so it's worth compiling even if the outer trip is low.
static bool stmtContainsLoop(Stmt* s) {
    if (!s) return false;
    switch (s->kind) {
        case Stmt::Kind::While: case Stmt::Kind::For: case Stmt::Kind::ForIn:
            return true;
        case Stmt::Kind::Block:
            for (auto& st : static_cast<BlockStmt*>(s)->statements)
                if (stmtContainsLoop(st.get())) return true;
            return false;
        case Stmt::Kind::If: {
            auto* i = static_cast<IfStmt*>(s);
            return stmtContainsLoop(i->thenBranch.get()) || stmtContainsLoop(i->elseBranch.get());
        }
        default: return false;
    }
}

bool Interpreter::tryJitLoop(Stmt* stmt, std::shared_ptr<Environment>& env) {
    // Only top-level loops (whose outer variables are named globals) qualify;
    // inside functions, loop variables are slots and the function-level JIT
    // already applies.
    if (env.get() != globals.get()) return false;

    // Warmup guard: a small flat loop finishes faster interpreted than the ~3 ms
    // it takes to compile. Nested/large/unanalyzable loops still compile.
    if (stmt->kind == Stmt::Kind::For) {
        auto* fs = static_cast<ForStmt*>(stmt);
        long trips = estimateForTrips(fs);
        if (trips >= 0 && trips < kJitLoopMinTrips && !stmtContainsLoop(fs->body.get()))
            return false;
    }

    const CompiledLoop& cl = jit.getCompiledLoop(stmt, *this);
    if (!cl.fn) return false;

    // Gather the current values of the numeric globals the loop touches. If any
    // isn't a number right now, interpret instead.
    size_t n = cl.globals.size();
    std::vector<double> vars(n ? n : 1);
    std::vector<Value*> slots(n);
    for (size_t i = 0; i < n; ++i) {
        Value* p = globals->findNameSlot(cl.globals[i]);
        if (!p || !p->isNumber()) return false;
        slots[i] = p;
        vars[i] = p->asNumber();
    }

    int bail = 0;
    cl.fn(vars.data(), (int)n, this, &bail);
    if (bail) return false;   // native code gave up: re-run from the original state

    for (size_t i = 0; i < n; ++i) *slots[i] = Value(vars[i]);
    return true;
}

Value* Interpreter::selfStringAppend(AssignExpr* e, std::shared_ptr<Environment>& env) {
    if (!e->value || e->value->kind != Expr::Kind::Binary) return nullptr;
    auto* bin = static_cast<BinaryExpr*>(e->value.get());
    if (bin->op != TokenType::PLUS || bin->left->kind != Expr::Kind::Variable) return nullptr;
    auto* lv = static_cast<VariableExpr*>(bin->left.get());

    Value* slot = nullptr;
    if (e->global) {
        if (!lv->global || lv->name != e->name) return nullptr;
        slot = env->findNameSlot(e->name);
    } else {
        if (lv->global || lv->depth != e->depth || lv->slot != e->slot) return nullptr;
        slot = &env->ancestor(e->depth)->slots[(size_t)e->slot];
    }
    if (!slot || !slot->isString()) return nullptr;
    return slot;
}

Flow Interpreter::execute(Stmt* stmt, std::shared_ptr<Environment>& env) {
    switch (stmt->kind) {
        case Stmt::Kind::Expression: {
            auto* s = static_cast<ExprStmt*>(stmt);
            evaluate(s->expr.get(), env);
            break;
        }
        case Stmt::Kind::Let: {
            auto* s = static_cast<LetStmt*>(stmt);
            Value v = s->initializer ? evaluate(s->initializer.get(), env) : Value();
            checkDeclared(s->type, s->name, v, s->line);
            v = coerceToType(s->type, v);   // `let x: i8 = ...` wraps into range
            if (!s->isDestructure) {
                if (s->global) env->define(s->name, v);
                else env->slots[(size_t)s->slot] = v;
            } else if (s->destructureDict) {
                if (!v.isDict()) error("dict destructuring requires a dict", s->line);
                auto d = v.asDict();
                for (size_t i = 0; i < s->names.size(); ++i) {
                    auto it = d->find(s->names[i]);
                    Value val = it != d->end() ? it->second : Value();
                    if (s->global) env->define(s->names[i], val);
                    else env->slots[(size_t)s->nameSlots[i]] = val;
                }
            } else {
                if (!v.isList()) error("list destructuring requires a list", s->line);
                auto l = v.asList();
                for (size_t i = 0; i < s->names.size(); ++i) {
                    Value val = i < l->size() ? (*l)[i] : Value();
                    if (s->global) env->define(s->names[i], val);
                    else env->slots[(size_t)s->nameSlots[i]] = val;
                }
            }
            break;
        }
        case Stmt::Kind::Block: {
            auto* s = static_cast<BlockStmt*>(stmt);
            if (s->transparent)
                return execBlock(s->statements, env);
            auto child = std::make_shared<Environment>(env, s->slotCount);
            return execBlock(s->statements, child);
        }
        case Stmt::Kind::If: {
            auto* s = static_cast<IfStmt*>(stmt);
            if (evaluate(s->condition.get(), env).truthy())
                return execute(s->thenBranch.get(), env);
            if (s->elseBranch)
                return execute(s->elseBranch.get(), env);
            break;
        }
        case Stmt::Kind::While: {
            auto* s = static_cast<WhileStmt*>(stmt);
            if (tryJitLoop(stmt, env)) break;
            while (evaluate(s->condition.get(), env).truthy()) {
                Flow f = execute(s->body.get(), env);
                if (f == Flow::Break) break;
                if (f == Flow::Return) return f;   // Continue: re-test the condition
            }
            break;
        }
        case Stmt::Kind::For: {
            auto* s = static_cast<ForStmt*>(stmt);
            if (tryJitLoop(stmt, env)) break;
            // A merged loop scope lives in the enclosing frame: no allocation.
            auto loopEnv = s->ownScope ? std::make_shared<Environment>(env, s->slotCount) : env;
            if (s->init) execute(s->init.get(), loopEnv);
            while (s->condition ? evaluate(s->condition.get(), loopEnv).truthy() : true) {
                Flow f = execute(s->body.get(), loopEnv);
                if (f == Flow::Break) break;
                if (f == Flow::Return) return f;   // Continue: fall through to the increment
                if (s->increment) evaluate(s->increment.get(), loopEnv);
            }
            break;
        }
        case Stmt::Kind::ForIn: {
            auto* s = static_cast<ForInStmt*>(stmt);
            Value iter = evaluate(s->iterable.get(), env);
            auto loopEnv = s->ownScope ? std::make_shared<Environment>(env, s->slotCount) : env;

            // Returns Normal to keep iterating, Break to stop, Return to unwind.
            auto runOne = [&](const Value& item) -> Flow {
                loopEnv->slots[(size_t)s->varSlot] = item;
                Flow f = execute(s->body.get(), loopEnv);
                return (f == Flow::Continue) ? Flow::Normal : f;
            };

            if (iter.isList()) {
                auto l = iter.asList();
                for (size_t i = 0; i < l->size(); ++i) {
                    Flow f = runOne((*l)[i]);
                    if (f == Flow::Return) return f;
                    if (f == Flow::Break) break;
                }
            } else if (iter.isString()) {
                const std::string& str = iter.asString();
                for (char c : str) {
                    Flow f = runOne(Value(std::string(1, c)));
                    if (f == Flow::Return) return f;
                    if (f == Flow::Break) break;
                }
            } else if (iter.isDict()) {
                auto d = iter.asDict();
                for (auto& kv : *d) {
                    Flow f = runOne(Value(kv.first));
                    if (f == Flow::Return) return f;
                    if (f == Flow::Break) break;
                }
            } else {
                error("value is not iterable", s->line);
            }
            break;
        }
        case Stmt::Kind::Function: {
            auto* s = static_cast<FunctionStmt*>(stmt);
            auto fn = std::make_shared<Function>();
            fn->decl = s;
            fn->closure = env;
            fn->name = s->name;
            fn->file = tlsCurrentFile;
            if (s->nameGlobal) env->define(s->name, Value(fn));
            else env->slots[(size_t)s->nameSlot] = Value(fn);
            break;
        }
        case Stmt::Kind::Return: {
            auto* s = static_cast<ReturnStmt*>(stmt);
            returnValue_ = s->value ? evaluate(s->value.get(), env) : Value();
            return Flow::Return;
        }
        case Stmt::Kind::Class:
            execClass(static_cast<ClassStmt*>(stmt), env);
            break;
        case Stmt::Kind::Import:
            execImport(static_cast<ImportStmt*>(stmt), env);
            break;
        case Stmt::Kind::Match: {
            auto* s = static_cast<MatchStmt*>(stmt);
            Value subj = evaluate(s->subject.get(), env);
            for (auto& c : s->cases) {
                for (auto& v : c.values) {
                    if (valuesEqual(subj, evaluate(v.get(), env)))
                        return execute(c.body.get(), env);
                }
            }
            if (s->hasDefault) return execute(s->defaultBody.get(), env);
            break;
        }
        case Stmt::Kind::Try:
            return execTry(static_cast<TryStmt*>(stmt), env);
        case Stmt::Kind::Throw: {
            auto* s = static_cast<ThrowStmt*>(stmt);
            Value thrown = evaluate(s->value.get(), env);
            throw BeeThrow{ thrown, formatTrace(s->line) };
        }
        case Stmt::Kind::Break:
            return Flow::Break;
        case Stmt::Kind::Continue:
            return Flow::Continue;
    }
    return Flow::Normal;
}

Flow Interpreter::runCatch(TryStmt* s, const Value& err, std::shared_ptr<Environment>& env) {
    auto catchEnv = s->catchOwnScope ? std::make_shared<Environment>(env, s->catchScopeSlots) : env;
    if (!s->catchName.empty())
        catchEnv->slots[(size_t)s->catchSlot] = err;
    return execute(s->catchBody.get(), catchEnv);
}

Flow Interpreter::execTry(TryStmt* s, std::shared_ptr<Environment>& env) {
    // `finally` must run on every exit path: normal completion, a handled or
    // rethrown Bee exception, or a return/break/continue leaving the block.
    Flow f = Flow::Normal;
    try {
        try {
            f = execute(s->body.get(), env);
        } catch (BeeThrow& t) {
            if (!s->hasCatch) throw;
            f = runCatch(s, t.value, env);
        } catch (TracedError& e) {
            if (!s->hasCatch) throw;
            // One line: a handler usually prints this inside a message of its
            // own, and a stack trace embedded there would be noise.
            f = runCatch(s, Value(e.brief()), env);
        } catch (RuntimeError& e) {
            if (!s->hasCatch) throw;
            f = runCatch(s, Value(std::string(e.what())), env); // a built-in's own message
        }
    } catch (...) {
        if (s->hasFinally) execute(s->finallyBody.get(), env);
        throw;
    }
    if (s->hasFinally) {
        // The body's pending `return` value has to survive the finally block,
        // which runs arbitrary code -- including calls that set returnValue_.
        Value pending;
        if (f == Flow::Return) pending = std::move(returnValue_);
        Flow ff = execute(s->finallyBody.get(), env);
        // A jump out of `finally` wins over one out of the body, matching the
        // old behaviour where a signal thrown from finally replaced the one in
        // flight.
        if (ff != Flow::Normal) return ff;
        if (f == Flow::Return) returnValue_ = std::move(pending);
    }
    return f;
}

void Interpreter::execClass(ClassStmt* c, std::shared_ptr<Environment>& env) {
    std::shared_ptr<Class> superclass;
    if (!c->superclassName.empty()) {
        Value sv;
        if (c->superGlobal) {
            if (!env->tryGetName(c->superclassName, sv))
                error("superclass '" + c->superclassName + "' is not defined", c->line);
        } else {
            sv = env->getAt(c->superDepth, c->superSlot);
        }
        if (!sv.isClass())
            error("superclass '" + c->superclassName + "' is not a class", c->line);
        superclass = sv.asClass();
    }

    auto klass = std::make_shared<Class>();
    klass->name = c->name;
    klass->superclass = superclass;

    // Methods close over the class's defining environment. `this` and (when a
    // superclass exists) `super` are reserved as hidden frame slots, populated
    // per call -- so `super` always refers to *this* class's superclass,
    // regardless of the runtime type of the receiver.
    for (auto& m : c->methods) {
        auto fn = std::make_shared<Function>();
        fn->decl = m.get();
        fn->closure = env;
        fn->name = m->name;
        fn->file = tlsCurrentFile;
        fn->isInitializer = (m->name == "init");
        fn->definingClass = klass;
        klass->methods[m->name] = fn;
    }

    if (c->nameGlobal) env->define(c->name, Value(klass));
    else env->slots[(size_t)c->nameSlot] = Value(klass);
}

void Interpreter::execImport(ImportStmt* s, std::shared_ptr<Environment>& env) {
    auto mod = loadModule(s->moduleName, s->line);

    if (s->isFrom) {
        if (s->importAll) {
            for (auto& kv : mod->env->values) {
                if (!kv.first.empty() && kv.first[0] == '_') continue; // skip privates
                env->define(kv.first, kv.second);
            }
        } else {
            for (auto& [name, alias] : s->names) {
                Value v;
                if (!mod->env->tryGetName(name, v))
                    error("module '" + s->moduleName + "' has no member '" + name + "'", s->line);
                env->define(alias, v);
            }
        }
    } else {
        env->define(s->bindName, Value(mod));
    }
}

// ------------------------------------------------------------------
// Modules
// ------------------------------------------------------------------

std::string Interpreter::resolveModulePath(const std::string& moduleName) {
    // Roots are searched in order: the importing file's own directory first, so
    // local code always wins over an installed package of the same name.
    std::vector<std::string> roots;
    auto addRoot = [&](const std::string& r) {
        if (r.empty()) return;
        for (auto& have : roots)
            if (have == r) return;
        roots.push_back(r);
    };

    addRoot(currentDir);
    for (auto& sp : searchPaths) addRoot(sp);

    // `hive_modules` in the importing directory and every directory above it, so
    // an installed package finds the project's other packages -- including the
    // ones it depends on -- without each package carrying its own copy.
    {
        std::error_code ec;
        std::filesystem::path dir = std::filesystem::absolute(currentDir.empty() ? "." : currentDir, ec);
        if (!ec) {
            for (;;) {
                addRoot((dir / "hive_modules").generic_string());
                std::filesystem::path up = dir.parent_path();
                if (up.empty() || up == dir) break;
                dir = up;
            }
        }
    }

    for (auto& p : beePathRoots()) addRoot(p);
    addRoot(hiveGlobalLib());
    addRoot(".");

    // For `import a.b`, moduleName is "a/b" -- only the last component can name
    // a package's entry module.
    size_t slash = moduleName.find_last_of('/');
    const std::string lastName = (slash == std::string::npos) ? moduleName : moduleName.substr(slash + 1);

    for (auto& root : roots) {
        const std::string base = root + "/" + moduleName;
        if (isRegularFile(base + ".bee")) return base + ".bee";
        if (isRegularFile(base + ".be")) return base + ".be";
        // A native module (see bee_native.hpp) imports like any other.
        if (isRegularFile(base + nativeExtension())) return base + nativeExtension();
        if (isRegularFile(base)) return base;
        // A directory is an installed package: import its entry module.
        if (isDirectory(base)) {
            std::string entry = packageEntryPath(base, lastName);
            if (!entry.empty()) return entry;
        }
    }
    return "";
}

std::shared_ptr<Module> Interpreter::loadNativeModule(const std::string& moduleName,
                                                      const std::string& path, int line) {
    // dlopen wants a path it recognises as a path, not a bare name it would
    // search the system library directories for.
    std::string target = path;
    if (target.find('/') == std::string::npos && target.find('\\') == std::string::npos)
        target = "./" + target;

#ifdef _WIN32
    HMODULE lib = LoadLibraryA(target.c_str());
    if (!lib)
        error("cannot load native module '" + moduleName + "' (" + target + "): error " +
              std::to_string((long)GetLastError()), line);
    auto symbol = [&](const char* name) -> void* {
        return (void*)GetProcAddress(lib, name);
    };
#else
    void* lib = dlopen(target.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!lib)
        error("cannot load native module '" + moduleName + "': " + std::string(dlerror()), line);
    auto symbol = [&](const char* name) -> void* { return dlsym(lib, name); };
#endif

    auto abi = (const char* (*)())symbol("bee_native_abi");
    auto init = (int (*)(NativeModule*))symbol("bee_module_init");
    if (!abi || !init)
        error("'" + path + "' is not a bee native module (it exports no bee_module_init)", line);

    const char* moduleAbi = abi();
    if (!moduleAbi || std::string(moduleAbi) != BEE_NATIVE_ABI)
        error("native module '" + moduleName + "' was built for ABI '" +
              std::string(moduleAbi ? moduleAbi : "?") + "', this bee expects '" +
              BEE_NATIVE_ABI + "' -- rebuild it", line);

    nativeLibraries.push_back((void*)lib);

    auto modEnv = std::make_shared<Environment>(globals);
    auto mod = std::make_shared<Module>();
    mod->name = moduleName;
    mod->path = path;
    mod->env = modEnv;
    moduleCache[path] = mod;   // cache before init, matching .bee module loading

    NativeModule handle(*this, modEnv);
    int rc = 0;
    try {
        rc = init(&handle);
    } catch (const std::exception& e) {
        error("native module '" + moduleName + "' failed to initialise: " + e.what(), line);
    }
    if (rc != 0)
        error("native module '" + moduleName + "' failed to initialise (code " +
              std::to_string(rc) + ")", line);
    return mod;
}

std::shared_ptr<Module> Interpreter::loadModule(const std::string& moduleName, int line) {
    std::string path = resolveModulePath(moduleName);
    if (path.empty()) {
        std::string hint;
        // A bare name could well be a package nobody has installed yet.
        if (moduleName.find('/') == std::string::npos)
            hint = " (try 'hive install " + moduleName + "')";
        error("cannot find module '" + moduleName + "'" + hint, line);
    }

    auto cached = moduleCache.find(path);
    if (cached != moduleCache.end()) return cached->second;

    if (isNativePath(path)) return loadNativeModule(moduleName, path, line);

    std::string src;
    if (!readFileContents(path, src))
        error("cannot read module '" + moduleName + "'", line);

    auto program = std::make_unique<Program>();
    try {
        Lexer lx(src);
        auto toks = lx.tokenize();
        Parser ps(std::move(toks));
        *program = ps.parse();
    } catch (const LexError& e) {
        // Report the module's own file and line, then how we came to import it.
        throw TracedError("Lex error: " + e.message + "\n  at " + *internFile(path) + ":" +
                          std::to_string(e.line) + "\n" + formatTrace(line));
    } catch (const ParseError& e) {
        throw TracedError("Parse error: " + e.message + "\n  at " + *internFile(path) + ":" +
                          std::to_string(e.line) + "\n" + formatTrace(line));
    } catch (const std::exception& e) {
        error("while loading module '" + moduleName + "': " + e.what(), line);
    }
    Resolver().resolve(*program);

    auto modEnv = std::make_shared<Environment>(globals);
    auto mod = std::make_shared<Module>();
    mod->name = moduleName;
    mod->path = path;
    mod->env = modEnv;
    moduleCache[path] = mod; // cache early so cyclic imports see a partial module

    Program* prog = program.get();
    programStore.push_back(std::move(program));

    std::string savedDir = currentDir;
    currentDir = dirOf(path);
    FileScope scope(internFile(path));   // errors in the module name its file
    try {
        execProgram(*prog, modEnv);
    } catch (...) {
        currentDir = savedDir;
        throw;
    }
    currentDir = savedDir;
    return mod;
}

// ------------------------------------------------------------------
// Expression evaluation
// ------------------------------------------------------------------

Value Interpreter::evaluate(Expr* expr, std::shared_ptr<Environment>& env) {
    switch (expr->kind) {
        case Expr::Kind::Literal:
            return static_cast<LiteralExpr*>(expr)->value;

        case Expr::Kind::ListLit: {
            auto* e = static_cast<ListLitExpr*>(expr);
            auto list = std::make_shared<ValueList>();
            list->reserve(e->elements.size());
            for (size_t i = 0; i < e->elements.size(); ++i) {
                Value v = evaluate(e->elements[i].get(), env);
                if (i < e->spread.size() && e->spread[i]) {
                    if (!v.isList()) error("spread element (...) must be a list", e->line);
                    for (auto& x : *v.asList()) list->push_back(x);
                } else {
                    list->push_back(v);
                }
            }
            return Value(list);
        }

        case Expr::Kind::DictLit: {
            auto* e = static_cast<DictLitExpr*>(expr);
            auto dict = std::make_shared<ValueDict>();
            for (auto& entry : e->entries) {
                Value k = evaluate(entry.first.get(), env);
                Value v = evaluate(entry.second.get(), env);
                (*dict)[keyString(k)] = v;
            }
            return Value(dict);
        }

        case Expr::Kind::Variable: {
            auto* e = static_cast<VariableExpr*>(expr);
            if (!e->global) return env->getAt(e->depth, e->slot);
            // Inline cache: re-resolve only when the base environment changes.
            Environment* base = env.get();
            if (e->cacheEnv != base) {
                Value* slot = base->findNameSlot(e->name);
                if (!slot) error("undefined variable '" + e->name + "'", e->line);
                e->cacheEnv = base;
                e->cacheSlot = slot;
            }
            return *e->cacheSlot;
        }

        case Expr::Kind::Assign: {
            auto* e = static_cast<AssignExpr*>(expr);

            // Fast path: `x = x + rhs` (x a string). Capture x's buffer, then
            // append rhs. If x's string is uniquely owned once its own slot is
            // released, grow it in place; otherwise (an alias like `let a = x`
            // exists) fall back to allocating a fresh string, preserving value
            // semantics. This is O(1) amortized instead of O(n) per iteration.
            if (Value* slot = selfStringAppend(e, env)) {
                auto* bin = static_cast<BinaryExpr*>(e->value.get());
                auto sp = std::get<std::shared_ptr<std::string>>(slot->data); // old value
                std::string add;
                { Value rv = evaluate(bin->right.get(), env); add = stringify(rv); }
                *slot = Value();                        // drop the slot's own reference
                if (sp.use_count() == 1) { *sp += add; slot->data = sp; }   // unique: in place
                else slot->data = std::make_shared<std::string>(*sp + add); // shared: copy
                return *slot;
            }

            Value v = e->value ? evaluate(e->value.get(), env) : Value();
            // An annotation binds the name for its whole life, not just its
            // initialiser, so an assignment has to satisfy it too.
            checkDeclared(e->declaredType, e->name, v, e->line);
            v = coerceToType(e->declaredType, v);   // sized types wrap on every write
            if (!e->global) {
                env->setAt(e->depth, e->slot, v);
            } else {
                Environment* base = env.get();
                if (e->cacheEnv != base) {
                    Value* slot = base->findNameSlot(e->name);
                    if (!slot)
                        error("cannot assign to undefined variable '" + e->name + "'", e->line);
                    e->cacheEnv = base;
                    e->cacheSlot = slot;
                }
                *e->cacheSlot = v;
            }
            return v;
        }

        case Expr::Kind::Binary:
            return evalBinary(static_cast<BinaryExpr*>(expr), env);

        case Expr::Kind::Logical: {
            auto* e = static_cast<LogicalExpr*>(expr);
            Value l = evaluate(e->left.get(), env);
            if (e->op == TokenType::OR)
                return l.truthy() ? l : evaluate(e->right.get(), env);
            return l.truthy() ? evaluate(e->right.get(), env) : l;
        }

        case Expr::Kind::Unary: {
            auto* e = static_cast<UnaryExpr*>(expr);
            Value r = evaluate(e->right.get(), env);
            if (e->op == TokenType::MINUS) {
                if (!r.isNumber()) error("operand of unary '-' must be a number", e->line);
                return Value(-r.asNumber());
            }
            if (e->op == TokenType::BIT_NOT) {
                if (!r.isNumber()) error("operand of unary '~' must be a number", e->line);
                return Value((double)(~(long long)r.asNumber()));
            }
            return Value(!r.truthy()); // NOT
        }

        case Expr::Kind::Call:
            return evalCall(static_cast<CallExpr*>(expr), env);

        case Expr::Kind::Get:
            return evalGet(static_cast<GetExpr*>(expr), env);

        case Expr::Kind::Set: {
            auto* e = static_cast<SetExpr*>(expr);
            Value obj = evaluate(e->object.get(), env);
            if (!obj.isInstance())
                error("only instances have fields", e->line);
            auto inst = obj.asInstance();
            Value rhs = evaluate(e->value.get(), env);
            Value newVal;
            if (e->op == TokenType::ASSIGN) {
                newVal = rhs;
            } else {
                Value* cur = inst->field(e->name);
                if (!cur)
                    error("compound assignment to undefined field '" + e->name + "'", e->line);
                newVal = applyBinaryArith(e->op, *cur, rhs, e->line);
            }
            inst->setField(e->name, newVal);
            return newVal;
        }

        case Expr::Kind::Index:
            return evalIndex(static_cast<IndexExpr*>(expr), env);
        case Expr::Kind::Slice:
            return evalSlice(static_cast<SliceExpr*>(expr), env);

        case Expr::Kind::IndexSet: {
            auto* e = static_cast<IndexSetExpr*>(expr);
            Value obj = evaluate(e->object.get(), env);
            Value idx = evaluate(e->index.get(), env);
            Value rhs = evaluate(e->value.get(), env);
            Value newVal = (e->op == TokenType::ASSIGN)
                ? rhs
                : applyBinaryArith(e->op, indexGet(obj, idx, e->line), rhs, e->line);
            indexSet(obj, idx, newVal, e->line);
            return newVal;
        }

        case Expr::Kind::This: {
            auto* e = static_cast<ThisExpr*>(expr);
            if (e->depth < 0)
                error("'this' used outside of a method", expr->line);
            return env->getAt(e->depth, 0); // `this` is slot 0 of the method frame
        }

        case Expr::Kind::Super: {
            auto* e = static_cast<SuperExpr*>(expr);
            if (e->depth < 0)
                error("'super' used outside of a subclass method", e->line);
            return superMethod(env->getAt(e->depth, 1),   // superclass is slot 1
                               env->getAt(e->depth, 0),   // `this` is slot 0
                               e->method, e->line);
        }

        case Expr::Kind::Grouping:
            return evaluate(static_cast<GroupingExpr*>(expr)->inner.get(), env);

        case Expr::Kind::Ternary: {
            auto* e = static_cast<TernaryExpr*>(expr);
            return evaluate(e->cond.get(), env).truthy()
                ? evaluate(e->thenBranch.get(), env)
                : evaluate(e->elseBranch.get(), env);
        }

        case Expr::Kind::Function: {
            auto* e = static_cast<FunctionExpr*>(expr);
            auto fn = std::make_shared<Function>();
            fn->decl = e->fn.get();
            fn->closure = env;
            fn->name = ""; // anonymous
            fn->file = tlsCurrentFile;
            return Value(fn);
        }

        case Expr::Kind::ListComp: {
            auto* e = static_cast<ListCompExpr*>(expr);
            Value iter = evaluate(e->iterable.get(), env);
            auto out = std::make_shared<ValueList>();
            auto compEnv = e->ownScope ? std::make_shared<Environment>(env, e->slotCount) : env;
            auto one = [&](const Value& item) {
                compEnv->slots[(size_t)e->varSlot] = item;
                if (!e->cond || evaluate(e->cond.get(), compEnv).truthy())
                    out->push_back(evaluate(e->elem.get(), compEnv));
            };
            if (iter.isList()) {
                for (auto& x : *iter.asList()) one(x);
            } else if (iter.isString()) {
                for (char c : iter.asString()) one(Value(std::string(1, c)));
            } else if (iter.isDict()) {
                for (auto& kv : *iter.asDict()) one(Value(kv.first));
            } else {
                error("value is not iterable", e->line);
            }
            return Value(out);
        }
    }
    error("unknown expression", expr->line);
}

Value Interpreter::applyBinaryArith(TokenType op, const Value& l, const Value& r, int line) {
    switch (op) {
        case TokenType::PLUS:
            if (l.isNumber() && r.isNumber()) return Value(l.asNumber() + r.asNumber());
            if (l.isList() && r.isList()) {
                auto out = std::make_shared<ValueList>(*l.asList());
                auto rr = r.asList();
                out->insert(out->end(), rr->begin(), rr->end());
                return Value(out);
            }
            if (l.isString() || r.isString()) return Value(stringify(l) + stringify(r));
            error("operands of '+' must be numbers, strings, or lists", line);
        case TokenType::MINUS:
            if (l.isNumber() && r.isNumber()) return Value(l.asNumber() - r.asNumber());
            error("operands of '-' must be numbers", line);
        case TokenType::STAR:
            if (l.isNumber() && r.isNumber()) return Value(l.asNumber() * r.asNumber());
            error("operands of '*' must be numbers", line);
        case TokenType::SLASH:
            if (!(l.isNumber() && r.isNumber())) error("operands of '/' must be numbers", line);
            if (r.asNumber() == 0) error("division by zero", line);
            return Value(l.asNumber() / r.asNumber());
        default:
            error("unsupported compound operator", line);
    }
}

// The whole binary-operator semantics, on two already-evaluated operands. The
// tree-walker and the bytecode VM both go through here, so there is exactly one
// definition of what `+` means.
Value Interpreter::applyBinary(TokenType op, const Value& l, const Value& r, int line) {

    auto needNums = [&]() {
        if (!(l.isNumber() && r.isNumber()))
            error("operands must be numbers", line);
    };

    switch (op) {
        case TokenType::PLUS:
            if (l.isNumber() && r.isNumber()) return Value(l.asNumber() + r.asNumber());
            if (l.isList() && r.isList()) {
                auto out = std::make_shared<ValueList>(*l.asList());
                auto rr = r.asList();
                out->insert(out->end(), rr->begin(), rr->end());
                return Value(out);
            }
            if (l.isString() || r.isString())
                return Value(stringify(l) + stringify(r));
            error("operands of '+' must be numbers, strings, or lists", line);

        case TokenType::MINUS:
            needNums();
            return Value(l.asNumber() - r.asNumber());

        case TokenType::STAR:
            if (l.isNumber() && r.isNumber()) return Value(l.asNumber() * r.asNumber());
            if (l.isString() && r.isNumber()) {
                std::string out;
                long long n = (long long)r.asNumber();
                for (long long i = 0; i < n; ++i) out += l.asString();
                return Value(out);
            }
            if (l.isList() && r.isNumber()) {
                auto out = std::make_shared<ValueList>();
                long long n = (long long)r.asNumber();
                auto src = l.asList();
                for (long long i = 0; i < n; ++i)
                    out->insert(out->end(), src->begin(), src->end());
                return Value(out);
            }
            error("operands of '*' must be numbers (or string/list * number)", line);

        case TokenType::SLASH:
            needNums();
            if (r.asNumber() == 0) error("division by zero", line);
            return Value(l.asNumber() / r.asNumber());

        case TokenType::PERCENT:
            needNums();
            if (r.asNumber() == 0) error("modulo by zero", line);
            return Value(beeMod(l.asNumber(), r.asNumber()));

        case TokenType::LT: case TokenType::GT:
        case TokenType::LE: case TokenType::GE: {
            int cmp;
            if (l.isNumber() && r.isNumber()) {
                double a = l.asNumber(), b = r.asNumber();
                cmp = (a < b) ? -1 : (a > b) ? 1 : 0;
            } else if (l.isString() && r.isString()) {
                int c = l.asString().compare(r.asString());
                cmp = (c < 0) ? -1 : (c > 0) ? 1 : 0;
            } else {
                error("comparison operands must both be numbers or both strings", line);
            }
            switch (op) {
                case TokenType::LT: return Value(cmp < 0);
                case TokenType::GT: return Value(cmp > 0);
                case TokenType::LE: return Value(cmp <= 0);
                default:            return Value(cmp >= 0); // GE
            }
        }

        case TokenType::EQ:  return Value(valuesEqual(l, r));
        case TokenType::NEQ: return Value(!valuesEqual(l, r));

        case TokenType::BIT_AND: case TokenType::BIT_OR: case TokenType::BIT_XOR:
        case TokenType::SHL: case TokenType::SHR: {
            needNums();
            long long a = (long long)l.asNumber();
            long long b = (long long)r.asNumber();
            switch (op) {
                case TokenType::BIT_AND: return Value((double)(a & b));
                case TokenType::BIT_OR:  return Value((double)(a | b));
                case TokenType::BIT_XOR: return Value((double)(a ^ b));
                case TokenType::SHL:     return Value((double)(a << b));
                default:                 return Value((double)(a >> b)); // SHR
            }
        }

        default:
            error("unknown binary operator", line);
    }
}

Value Interpreter::evalBinary(BinaryExpr* e, std::shared_ptr<Environment>& env) {
    Value l = evaluate(e->left.get(), env);
    Value r = evaluate(e->right.get(), env);
    return applyBinary(e->op, l, r, e->line);
}

Value Interpreter::evalCall(CallExpr* e, std::shared_ptr<Environment>& env) {
    Value callee = evaluate(e->callee.get(), env);

    // The argument vector is recycled rather than allocated per call. The pool
    // is a stack, so a nested call inside an argument expression gets its own.
    std::vector<Value> args;
    if (!tlsArgPool.empty()) { args = std::move(tlsArgPool.back()); tlsArgPool.pop_back(); }
    struct ArgRecycler {
        std::vector<Value>* p;
        ~ArgRecycler() {
            if (tlsArgPool.size() < kPoolLimit) { p->clear(); tlsArgPool.push_back(std::move(*p)); }
        }
    } argRecycler{ &args };

    args.reserve(e->args.size());
    for (size_t i = 0; i < e->args.size(); ++i) {
        Value v = evaluate(e->args[i].get(), env);
        if (i < e->spread.size() && e->spread[i]) {
            if (!v.isList()) error("spread argument (...) must be a list", e->line);
            for (auto& x : v.listRef()) args.push_back(x);
        } else {
            args.push_back(std::move(v));
        }
    }
    return callValue(callee, args, e->line);
}

Value Interpreter::evalGet(GetExpr* e, std::shared_ptr<Environment>& env) {
    Value obj = evaluate(e->object.get(), env);
    return getProperty(obj, e->name, e->line);
}

// obj[start:end], with either bound possibly nil meaning "open". Shared by the
// tree-walker and the VM.
Value Interpreter::sliceValue(const Value& obj, const Value& start, const Value& end, int line) {
    const bool isList = obj.isList();
    if (!isList && !obj.isString())
        error("only lists and strings can be sliced", line);

    const long long n = isList ? (long long)obj.listRef().size()
                               : (long long)obj.asString().size();

    auto bound = [&](const Value& v, long long fallback) {
        if (v.isNil()) return fallback;
        if (!v.isNumber()) error("slice bounds must be numbers", line);
        long long i = (long long)v.asNumber();
        if (i < 0) i += n;
        if (i < 0) i = 0;
        if (i > n) i = n;
        return i;
    };

    long long from = bound(start, 0);
    long long to = bound(end, n);
    if (to < from) to = from;   // an inverted range is empty, not an error

    if (isList) {
        const ValueList& src = obj.listRef();
        auto out = std::make_shared<ValueList>(src.begin() + (long)from, src.begin() + (long)to);
        return Value(out);
    }
    return Value(obj.asString().substr((size_t)from, (size_t)(to - from)));
}

Value Interpreter::evalSlice(SliceExpr* e, std::shared_ptr<Environment>& env) {
    Value obj = evaluate(e->object.get(), env);
    const bool isList = obj.isList();
    if (!isList && !obj.isString())
        error("only lists and strings can be sliced", e->line);

    const long long n = isList ? (long long)obj.asList()->size()
                               : (long long)obj.asString().size();

    // A bound may be negative (from the end) and may point outside the value;
    // like most languages, slicing clamps instead of failing, so a[0:1000] is
    // simply everything.
    auto bound = [&](Expr* expr, long long fallback) {
        if (!expr) return fallback;
        Value v = evaluate(expr, env);
        if (!v.isNumber()) error("slice bounds must be numbers", e->line);
        long long i = (long long)v.asNumber();
        if (i < 0) i += n;
        if (i < 0) i = 0;
        if (i > n) i = n;
        return i;
    };

    long long from = bound(e->start.get(), 0);
    long long to = bound(e->end.get(), n);
    if (to < from) to = from;   // an inverted range is empty, not an error

    if (isList) {
        auto src = obj.asList();
        auto out = std::make_shared<ValueList>(src->begin() + (long)from, src->begin() + (long)to);
        return Value(out);
    }
    return Value(obj.asString().substr((size_t)from, (size_t)(to - from)));
}

// `super.name`, bound to the receiver. Shared by the tree-walker and the VM.
Value Interpreter::superMethod(const Value& superV, const Value& self,
                               const std::string& name, int line) {
    auto superClass = superV.asClass();
    auto m = superClass->findMethod(name);
    if (!m) error("undefined method '" + name + "' on superclass", line);
    return Value(bindMethod(m, self.asInstance(), superClass));
}

// Reading obj[idx]. Shared by the tree-walker and the VM.
Value Interpreter::indexGet(const Value& obj, const Value& idx, int line) {

    if (obj.isList()) {
        const ValueList& lst = obj.listRef();   // borrowed: no refcount traffic
        if (!idx.isNumber()) error("list index must be a number", line);
        long long i = (long long)idx.asNumber();
        if (i < 0) i += (long long)lst.size();
        if (i < 0 || i >= (long long)lst.size())
            error("list index out of range", line);
        return lst[(size_t)i];
    }
    if (obj.isString()) {
        const std::string& s = obj.asString();
        if (!idx.isNumber()) error("string index must be a number", line);
        long long i = (long long)idx.asNumber();
        if (i < 0) i += (long long)s.size();
        if (i < 0 || i >= (long long)s.size())
            error("string index out of range", line);
        return Value(std::string(1, s[(size_t)i]));
    }
    if (obj.isBuffer()) {
        // Flat indexing, so b[0] works whatever the shape. Use at(b, i, j) for
        // multi-dimensional access.
        auto buf = obj.asBuffer();
        if (!idx.isNumber()) error("buffer index must be a number", line);
        long long i = (long long)idx.asNumber();
        if (i < 0) i += (long long)buf->count();
        if (i < 0 || (size_t)i >= buf->count())
            error("buffer index out of range (" + std::to_string(buf->count()) + " element(s))",
                  line);
        return Value(buf->get((size_t)i));
    }
    if (obj.isDict()) {
        const ValueDict& d = obj.dictRef();
        auto it = d.find(keyString(idx));
        return it != d.end() ? it->second : Value();
    }
    error("cannot index this type", line);
}

// Writing obj[idx] = v. Shared by the tree-walker and the VM; compound forms
// (`+=`) are a read, an arithmetic op, and then this.
void Interpreter::indexSet(const Value& obj, const Value& idx, const Value& v, int line) {
    if (obj.isList()) {
        ValueList& lst = obj.listRef();   // borrowed: no refcount traffic
        if (!idx.isNumber()) error("list index must be a number", line);
        long long i = (long long)idx.asNumber();
        if (i < 0) i += (long long)lst.size();
        if (i < 0 || i >= (long long)lst.size())
            error("list index out of range", line);
        lst[(size_t)i] = v;
        return;
    }
    if (obj.isDict()) {
        obj.dictRef()[keyString(idx)] = v;
        return;
    }
    if (obj.isBuffer()) {
        auto buf = obj.asBuffer();
        if (!idx.isNumber()) error("buffer index must be a number", line);
        long long i = (long long)idx.asNumber();
        if (i < 0) i += (long long)buf->count();
        if (i < 0 || (size_t)i >= buf->count())
            error("buffer index out of range (" + std::to_string(buf->count()) +
                  " element(s))", line);
        if (!v.isNumber()) error("a buffer holds numbers only", line);
        buf->set((size_t)i, v.asNumber());
        return;
    }
    error("cannot assign by index to this type", line);
}

Value Interpreter::evalIndex(IndexExpr* e, std::shared_ptr<Environment>& env) {
    Value obj = evaluate(e->object.get(), env);
    Value idx = evaluate(e->index.get(), env);
    return indexGet(obj, idx, e->line);
}

// ------------------------------------------------------------------
// Calling
// ------------------------------------------------------------------

Value Interpreter::callValue(const Value& callee, std::vector<Value>& args, int line) {
    if (callee.isBuiltin()) {
        auto b = callee.asBuiltin();
        if (b->arity >= 0 && (int)args.size() != b->arity)
            error("'" + b->name + "' expects " + std::to_string(b->arity) +
                  " argument(s) but got " + std::to_string(args.size()), line);
        // A built-in throws a bare RuntimeError with no idea where it was
        // called from. Attach the location here -- and let an already-traced
        // error from Bee code called *back* into (map, sort, spawn) pass
        // through untouched, so it keeps its own deeper trace.
        try {
            return b->fn(*this, args);
        } catch (TracedError&) {
            throw;
        // A Bee-level throw travels through built-ins that call back into Bee
        // code (map, sort, spawn); it must pass untouched. Control flow no
        // longer appears here at all -- callFunction absorbs it before the
        // built-in sees anything.
        } catch (BeeThrow&) {
            throw;
        } catch (RuntimeError& e) {
            error(e.what(), line);
        } catch (const std::exception& e) {
            // A bound C++ library throwing its own exception type (cv::Exception,
            // std::bad_alloc, Ort::Exception) would otherwise unwind past the
            // interpreter and abort the process.
            error(std::string("native error: ") + e.what(), line);
        } catch (...) {
            error("native code threw an exception that is not a std::exception", line);
        }
    }
    if (callee.isFunction())
        return callFunction(callee.asFunction(), args, line);
    if (callee.isClass()) {
        auto klass = callee.asClass();
        auto inst = std::make_shared<Instance>();
        inst->klass = klass;
        auto init = klass->findMethod("init");
        if (init) {
            auto bound = bindMethod(init, inst, klass);
            callFunction(bound, args, line);
        } else if (!args.empty()) {
            error("class '" + klass->name + "' has no constructor but got arguments", line);
        }
        return Value(inst);
    }
    error("value is not callable", line);
}

Value Interpreter::callFunction(const std::shared_ptr<Function>& fn, std::vector<Value>& args,
                                int line) {
    // An AOT-compiled (native) function: run its machine code directly. A bound
    // method receives its receiver as the first argument; the compiled body
    // expects that layout. The scope enforces the recursion-depth limit and
    // records a trace frame, exactly as an interpreted call would.
    if (fn->native) {
        CallScope scope(*this, fn, line);
        if (fn->boundThis) {
            std::vector<Value> a;
            a.reserve(args.size() + 1);
            a.push_back(Value(fn->boundThis));
            for (auto& x : args) a.push_back(x);
            return fn->native(*this, a);
        }
        return fn->native(*this, args);
    }

    const FunctionStmt* decl = fn->decl;
    size_t np = decl->params.size();
    size_t provided = args.size();
    int rest = decl->restParam;
    // The name is only needed to report an error, so it is formatted on the
    // error paths rather than on every call.
    if (rest < 0 && provided > np)
        error("function '" + functionName(*fn) + "' expects at most " + std::to_string(np) +
              " argument(s) but got " + std::to_string(provided), line);

    // Fast path: if this is a plain (non-method) function called with exactly
    // its numeric arguments, try the LLVM-compiled version. A `bail` means the
    // native code hit something it can't handle (e.g. division by zero); we
    // then fall through to the interpreter, which is safe because the JIT
    // subset has no side effects.
    if (!fn->boundThis && !fn->definingClass && rest < 0 && provided == np && np <= kMaxJitArgs &&
        !decl->usesSized) {   // sized types need wrapping the JIT doesn't do
        // The argument types *are* the guard. Each call classifies what it is
        // actually passing; native code exists per signature, so a call with
        // different types simply finds none and runs interpreted. Numbers pass
        // unboxed; an f64 buffer passes as a raw pointer and a count, which is
        // what lets a numeric kernel over one compile at all.
        JitSig sig = 0;
        bool compilable = true;
        for (size_t i = 0; i < np && compilable; ++i) {
            if (args[i].isNumber()) sig = jitSigWith(sig, i, ArgKind::Num);
            else if (args[i].isBuffer() && args[i].bufRef().dtype == DType::F64)
                sig = jitSigWith(sig, i, ArgKind::BufF64);
            else compilable = false;
        }
        if (compilable) {
            if (JitFn nf = jit.getCompiled(decl, sig, *this)) {
                std::vector<double> ds(np ? np : 1);
                std::vector<double*> bufs(np ? np : 1, nullptr);
                std::vector<long long> lens(np ? np : 1, 0);
                for (size_t i = 0; i < np; ++i) {
                    if (jitSigAt(sig, i) == ArgKind::BufF64) {
                        Buffer& b = args[i].bufRef();
                        bufs[i] = (double*)b.bytes.data();
                        lens[i] = (long long)b.count();
                    } else {
                        ds[i] = args[i].asNumber();
                    }
                }
                int bail = 0;
                double r = nf(ds.data(), bufs.data(), lens.data(), this, &bail);
                // Native code does not know about declared types, so the result
                // is checked here -- notably a `-> num` function that fell off
                // its end, which completes natively with a nil result.
                if (bail == 0) { checkReturnType(*decl, Value(r), line); return Value(r); }
                if (bail == 2) { checkReturnType(*decl, Value(), line); return Value(); }
                // bail == 1: native code gave up; fall through to the interpreter
            }
        }
    }

    // Past this point we are inside the callee, so record a frame: the trace
    // needs the call site, and the depth check needs the count. Arity errors
    // above deliberately stay attributed to the caller.
    if (tlsCallStack.size() >= maxCallDepth)
        error("call stack overflow in '" + functionName(*fn) + "' (deeper than " +
              std::to_string(maxCallDepth) + " nested calls) -- unbounded recursion?\n"
              "       if the depth is intentional, raise it with BEE_MAX_DEPTH "
              "(and the stack with 'ulimit -s')", line);

    tlsCallStack.push_back({fn.get(), tlsCurrentFile, line});
    struct FrameGuard {
        ~FrameGuard() { tlsCallStack.pop_back(); }
    } frameGuard;
    FileScope fileScope(fn->file);   // errors inside report the callee's file

    // Second fast path: a body the register VM could compile runs as bytecode,
    // with its frame in registers rather than an Environment. Exact arity only --
    // defaults and rest parameters stay on the tree-walker.
    if (rest < 0 && provided == np && !decl->usesSized) {
        if (Chunk* ch = vm.chunkFor(decl)) {
            Value r = vm.run(*this, *ch, fn, args, line);
            if (fn->isInitializer && fn->boundThis) return Value(fn->boundThis);
            return r;
        }
    }

    // Frame layout (fixed by the resolver): [this?][super?][params...][locals...]
    auto frame = acquireEnv(fn->closure, decl->frameSlots);
    struct EnvRecycler {                       // runs before `frame` is destroyed
        std::shared_ptr<Environment>* p;
        ~EnvRecycler() { recycleEnv(*p); }
    } envRecycler{ &frame };
    if (fn->boundThis)
        frame->slots[0] = Value(fn->boundThis);
    if (fn->definingClass && fn->definingClass->superclass)
        frame->slots[1] = Value(fn->definingClass->superclass);
    int base = decl->paramStart;
    for (size_t i = 0; i < np; ++i) {
        if ((int)i == rest) {
            auto restList = std::make_shared<ValueList>();
            for (size_t j = i; j < provided; ++j) restList->push_back(args[j]);
            frame->slots[(size_t)base + i] = Value(restList);
        } else if (i < provided) {
            checkParamType(*decl, i, args[i], line);
            frame->slots[(size_t)base + i] =
                i < decl->paramTypes.size() ? coerceToType(decl->paramTypes[i], args[i]) : args[i];
        } else if (i < decl->defaults.size() && decl->defaults[i]) {
            Value d = evaluate(decl->defaults[i].get(), frame);
            checkParamType(*decl, i, d, line);   // a default has to satisfy it too
            if (i < decl->paramTypes.size()) d = coerceToType(decl->paramTypes[i], d);
            frame->slots[(size_t)base + i] = std::move(d);
        } else {
            error("function '" + functionName(*fn) + "' missing required argument '" +
                  decl->params[i] + "'", line);
        }
    }

    for (auto& s : decl->body) {
        Flow f = execute(s.get(), frame);
        if (f == Flow::Normal) continue;
        if (f == Flow::Return) {
            // Take the value now: anything that runs next could set it again.
            Value rv = std::move(returnValue_);
            returnValue_ = Value();
            if (fn->isInitializer && fn->boundThis) return Value(fn->boundThis);
            checkReturnType(*decl, rv, s->line);
            return coerceToType(decl->returnType, rv);   // `-> i8` wraps the result
        }
        // A break/continue that no loop in this function absorbed.
        error(std::string(f == Flow::Break ? "'break'" : "'continue'") +
              " used outside of a loop", s->line);
    }
    if (fn->isInitializer && fn->boundThis) return Value(fn->boundThis);
    checkReturnType(*decl, Value(), line);   // fell off the end: the value is nil
    return Value();
}

std::shared_ptr<Function> Interpreter::bindMethod(std::shared_ptr<Function> method,
                                                  std::shared_ptr<Instance> self,
                                                  std::shared_ptr<Class> definingClass) {
    // `this`/`super` are populated as frame slots at call time, so binding just
    // records the receiver. The method keeps its own definingClass (which drives
    // `super`), so we only fill it in as a fallback.
    auto bound = std::make_shared<Function>(*method);
    bound->boundThis = self;
    if (!bound->definingClass) bound->definingClass = definingClass;
    return bound;
}

// ------------------------------------------------------------------
// Property access (fields, methods, built-in type methods)
// ------------------------------------------------------------------

Value Interpreter::getProperty(const Value& object, const std::string& name, int line) {
    if (object.isInstance()) {
        auto inst = object.asInstance();
        if (const Value* f = inst->field(name)) return *f;
        auto m = inst->klass->findMethod(name);
        if (m) return Value(bindMethod(m, inst, inst->klass));
        error("undefined property '" + name + "'", line);
    }

    if (object.isModule()) {
        Value v;
        if (!object.asModule()->env->tryGetName(name, v))
            error("module '" + object.asModule()->name + "' has no member '" + name + "'", line);
        return v;
    }

    if (object.isString()) {
        const Value self = object;
        if (name == "len" || name == "length")
            return nativeMethod(name, 0, [self](Interpreter&, std::vector<Value>&) {
                return Value((double)self.asString().size());
            });
        if (name == "upper")
            return nativeMethod(name, 0, [self](Interpreter&, std::vector<Value>&) {
                std::string s = self.asString();
                for (auto& c : s) c = (char)std::toupper((unsigned char)c);
                return Value(s);
            });
        if (name == "lower")
            return nativeMethod(name, 0, [self](Interpreter&, std::vector<Value>&) {
                std::string s = self.asString();
                for (auto& c : s) c = (char)std::tolower((unsigned char)c);
                return Value(s);
            });
        if (name == "trim")
            return nativeMethod(name, 0, [self](Interpreter&, std::vector<Value>&) {
                const std::string& s = self.asString();
                size_t a = 0, b = s.size();
                while (a < b && std::isspace((unsigned char)s[a])) a++;
                while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
                return Value(s.substr(a, b - a));
            });
        if (name == "contains")
            return nativeMethod(name, 1, [self](Interpreter&, std::vector<Value>& a) {
                return Value(self.asString().find(a[0].asString()) != std::string::npos);
            });
        if (name == "starts_with")
            return nativeMethod(name, 1, [self](Interpreter&, std::vector<Value>& a) {
                const std::string& s = self.asString();
                const std::string& p = a[0].asString();
                return Value(s.size() >= p.size() && s.compare(0, p.size(), p) == 0);
            });
        if (name == "ends_with")
            return nativeMethod(name, 1, [self](Interpreter&, std::vector<Value>& a) {
                const std::string& s = self.asString();
                const std::string& p = a[0].asString();
                return Value(s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0);
            });
        if (name == "split")
            return nativeMethod(name, 1, [self](Interpreter&, std::vector<Value>& a) {
                const std::string& s = self.asString();
                std::string sep = a[0].asString();
                auto out = std::make_shared<ValueList>();
                if (sep.empty()) {
                    for (char c : s) out->push_back(Value(std::string(1, c)));
                } else {
                    size_t pos, prev = 0;
                    while ((pos = s.find(sep, prev)) != std::string::npos) {
                        out->push_back(Value(s.substr(prev, pos - prev)));
                        prev = pos + sep.size();
                    }
                    out->push_back(Value(s.substr(prev)));
                }
                return Value(out);
            });
        if (name == "replace")
            return nativeMethod(name, 2, [self](Interpreter&, std::vector<Value>& a) {
                const std::string& s = self.asString();
                std::string from = a[0].asString(), to = a[1].asString();
                if (from.empty()) return Value(s);
                std::string res;
                size_t pos, prev = 0;
                while ((pos = s.find(from, prev)) != std::string::npos) {
                    res += s.substr(prev, pos - prev);
                    res += to;
                    prev = pos + from.size();
                }
                res += s.substr(prev);
                return Value(res);
            });
        if (name == "substr")
            return nativeMethod(name, 2, [self](Interpreter&, std::vector<Value>& a) {
                const std::string& s = self.asString();
                long long start = (long long)numArg(a[0], "substr");
                long long len = (long long)numArg(a[1], "substr");
                if (start < 0) start += (long long)s.size();
                if (start < 0) start = 0;
                if (start > (long long)s.size()) start = (long long)s.size();
                if (len < 0) len = 0;
                return Value(s.substr((size_t)start, (size_t)len));
            });
        if (name == "to_num")
            return nativeMethod(name, 0, [self](Interpreter&, std::vector<Value>&) -> Value {
                try { return Value(std::stod(self.asString())); }
                catch (...) { throw RuntimeError("to_num: cannot parse '" + self.asString() + "'"); }
            });
        error("string has no property '" + name + "'", line);
    }

    if (object.isList()) {
        const Value self = object;
        if (name == "len" || name == "length")
            return nativeMethod(name, 0, [self](Interpreter&, std::vector<Value>&) {
                return Value((double)self.asList()->size());
            });
        if (name == "push" || name == "append")
            return nativeMethod(name, 1, [self](Interpreter&, std::vector<Value>& a) {
                self.asList()->push_back(a[0]);
                return self;
            });
        if (name == "pop")
            return nativeMethod(name, 0, [self](Interpreter&, std::vector<Value>&) -> Value {
                auto l = self.asList();
                if (l->empty()) throw RuntimeError("pop: list is empty");
                Value v = l->back();
                l->pop_back();
                return v;
            });
        if (name == "contains" || name == "includes")
            return nativeMethod(name, 1, [self](Interpreter& I, std::vector<Value>& a) {
                for (auto& x : *self.asList())
                    if (I.valuesEqual(x, a[0])) return Value(true);
                return Value(false);
            });
        if (name == "index_of")
            return nativeMethod(name, 1, [self](Interpreter& I, std::vector<Value>& a) {
                auto l = self.asList();
                for (size_t i = 0; i < l->size(); ++i)
                    if (I.valuesEqual((*l)[i], a[0])) return Value((double)i);
                return Value(-1.0);
            });
        if (name == "insert")
            return nativeMethod(name, 2, [self](Interpreter&, std::vector<Value>& a) -> Value {
                auto l = self.asList();
                long long i = (long long)numArg(a[0], "insert");
                if (i < 0) i += (long long)l->size();
                if (i < 0 || i > (long long)l->size()) throw RuntimeError("insert: index out of range");
                l->insert(l->begin() + i, a[1]);
                return self;
            });
        if (name == "remove_at")
            return nativeMethod(name, 1, [self](Interpreter&, std::vector<Value>& a) -> Value {
                auto l = self.asList();
                long long i = (long long)numArg(a[0], "remove_at");
                if (i < 0) i += (long long)l->size();
                if (i < 0 || i >= (long long)l->size()) throw RuntimeError("remove_at: index out of range");
                Value v = (*l)[(size_t)i];
                l->erase(l->begin() + i);
                return v;
            });
        if (name == "join")
            return nativeMethod(name, -1, [self](Interpreter& I, std::vector<Value>& a) -> Value {
                if (a.size() > 1) throw RuntimeError("join: expects () or (sep)");
                std::string sep = a.empty() ? "" : (a[0].isString() ? a[0].asString() : I.stringify(a[0]));
                std::string out;
                auto l = self.asList();
                for (size_t i = 0; i < l->size(); ++i) {
                    if (i) out += sep;
                    out += I.stringify((*l)[i]);
                }
                return Value(out);
            });
        error("list has no property '" + name + "'", line);
    }

    if (object.isDict()) {
        const Value self = object;
        if (name == "keys")
            return nativeMethod(name, 0, [self](Interpreter&, std::vector<Value>&) {
                auto out = std::make_shared<ValueList>();
                for (auto& kv : *self.asDict()) out->push_back(Value(kv.first));
                return Value(out);
            });
        if (name == "values")
            return nativeMethod(name, 0, [self](Interpreter&, std::vector<Value>&) {
                auto out = std::make_shared<ValueList>();
                for (auto& kv : *self.asDict()) out->push_back(kv.second);
                return Value(out);
            });
        if (name == "has")
            return nativeMethod(name, 1, [self](Interpreter& I, std::vector<Value>& a) {
                return Value(self.asDict()->count(I.keyString(a[0])) > 0);
            });
        if (name == "remove")
            return nativeMethod(name, 1, [self](Interpreter& I, std::vector<Value>& a) {
                self.asDict()->erase(I.keyString(a[0]));
                return self;
            });
        if (name == "len" || name == "length")
            return nativeMethod(name, 0, [self](Interpreter&, std::vector<Value>&) {
                return Value((double)self.asDict()->size());
            });
        if (name == "get")
            return nativeMethod(name, -1, [self](Interpreter& I, std::vector<Value>& a) -> Value {
                if (a.empty() || a.size() > 2) throw RuntimeError("get: expects (key) or (key, default)");
                auto d = self.asDict();
                auto it = d->find(I.keyString(a[0]));
                if (it != d->end()) return it->second;
                return a.size() == 2 ? a[1] : Value();
            });
        // Fall back to treating `.name` as a key lookup.
        auto d = object.asDict();
        auto it = d->find(name);
        if (it != d->end()) return it->second;
        error("dict has no key or method '" + name + "'", line);
    }

    error("value of this type has no property '" + name + "'", line);
}

// ------------------------------------------------------------------
// Built-in global functions
// ------------------------------------------------------------------

void Interpreter::defineBuiltins() {
    auto def = [&](const std::string& n, int arity,
                   std::function<Value(Interpreter&, std::vector<Value>&)> f) {
        auto b = std::make_shared<Builtin>();
        b->name = n;
        b->arity = arity;
        b->fn = std::move(f);
        globals->define(n, Value(b));
    };

    def("print", -1, [](Interpreter& I, std::vector<Value>& a) {
        for (size_t i = 0; i < a.size(); ++i) {
            if (i) std::cout << ' ';
            std::cout << I.stringify(a[i]);
        }
        std::cout << '\n';
        return Value();
    });

    def("write", -1, [](Interpreter& I, std::vector<Value>& a) {
        for (size_t i = 0; i < a.size(); ++i) {
            if (i) std::cout << ' ';
            std::cout << I.stringify(a[i]);
        }
        return Value();
    });

    def("len", 1, [](Interpreter&, std::vector<Value>& a) -> Value {
        const Value& v = a[0];
        if (v.isString()) return Value((double)v.asString().size());
        if (v.isList())   return Value((double)v.asList()->size());
        if (v.isDict())   return Value((double)v.asDict()->size());
        if (v.isBuffer()) return Value((double)v.asBuffer()->count());
        throw RuntimeError("len: expected a string, list, dict, or buffer");
    });

    def("type", 1, [](Interpreter&, std::vector<Value>& a) -> Value {
        const Value& v = a[0];
        if (v.isNil())     return Value(std::string("nil"));
        if (v.isBool())    return Value(std::string("bool"));
        if (v.isNumber())  return Value(std::string("number"));
        if (v.isBuffer())  return Value(std::string("buffer"));
        if (v.isString())  return Value(std::string("string"));
        if (v.isList())    return Value(std::string("list"));
        if (v.isDict())    return Value(std::string("dict"));
        if (v.isFunction() || v.isBuiltin()) return Value(std::string("function"));
        if (v.isClass())   return Value(std::string("class"));
        if (v.isInstance())return Value(v.asInstance()->klass->name);
        if (v.isModule())  return Value(std::string("module"));
        return Value(std::string("unknown"));
    });

    def("str", 1, [](Interpreter& I, std::vector<Value>& a) {
        return Value(I.stringify(a[0]));
    });

    def("repr", 1, [](Interpreter& I, std::vector<Value>& a) {
        return Value(I.reprString(a[0]));
    });

    def("num", 1, [](Interpreter&, std::vector<Value>& a) -> Value {
        const Value& v = a[0];
        if (v.isNumber()) return v;
        if (v.isBool())   return Value(v.asBool() ? 1.0 : 0.0);
        if (v.isString()) {
            try { return Value(std::stod(v.asString())); }
            catch (...) { throw RuntimeError("num: cannot parse '" + v.asString() + "'"); }
        }
        throw RuntimeError("num: cannot convert this type");
    });

    def("int", 1, [](Interpreter&, std::vector<Value>& a) -> Value {
        const Value& v = a[0];
        if (v.isNumber()) return Value(std::trunc(v.asNumber()));
        if (v.isString()) {
            try { return Value(std::trunc(std::stod(v.asString()))); }
            catch (...) { throw RuntimeError("int: cannot parse '" + v.asString() + "'"); }
        }
        throw RuntimeError("int: cannot convert this type");
    });

    def("bool", 1, [](Interpreter&, std::vector<Value>& a) {
        return Value(a[0].truthy());
    });

    def("abs", 1, [](Interpreter&, std::vector<Value>& a) {
        return Value(std::fabs(numArg(a[0], "abs")));
    });
    def("floor", 1, [](Interpreter&, std::vector<Value>& a) {
        return Value(std::floor(numArg(a[0], "floor")));
    });
    def("ceil", 1, [](Interpreter&, std::vector<Value>& a) {
        return Value(std::ceil(numArg(a[0], "ceil")));
    });
    def("round", 1, [](Interpreter&, std::vector<Value>& a) {
        return Value(std::round(numArg(a[0], "round")));
    });
    def("sqrt", 1, [](Interpreter&, std::vector<Value>& a) {
        return Value(std::sqrt(numArg(a[0], "sqrt")));
    });
    def("pow", 2, [](Interpreter&, std::vector<Value>& a) {
        return Value(std::pow(numArg(a[0], "pow"), numArg(a[1], "pow")));
    });

    def("min", -1, [](Interpreter&, std::vector<Value>& a) -> Value {
        std::vector<Value> items = (a.size() == 1 && a[0].isList()) ? *a[0].asList() : a;
        if (items.empty()) throw RuntimeError("min: needs at least one value");
        double best = numArg(items[0], "min");
        for (size_t i = 1; i < items.size(); ++i)
            best = std::min(best, numArg(items[i], "min"));
        return Value(best);
    });
    def("max", -1, [](Interpreter&, std::vector<Value>& a) -> Value {
        std::vector<Value> items = (a.size() == 1 && a[0].isList()) ? *a[0].asList() : a;
        if (items.empty()) throw RuntimeError("max: needs at least one value");
        double best = numArg(items[0], "max");
        for (size_t i = 1; i < items.size(); ++i)
            best = std::max(best, numArg(items[i], "max"));
        return Value(best);
    });

    def("range", -1, [](Interpreter&, std::vector<Value>& a) -> Value {
        double start = 0, stop = 0, step = 1;
        if (a.size() == 1) {
            stop = numArg(a[0], "range");
        } else if (a.size() == 2) {
            start = numArg(a[0], "range");
            stop = numArg(a[1], "range");
        } else if (a.size() == 3) {
            start = numArg(a[0], "range");
            stop = numArg(a[1], "range");
            step = numArg(a[2], "range");
        } else {
            throw RuntimeError("range: expects 1 to 3 arguments");
        }
        if (step == 0) throw RuntimeError("range: step cannot be zero");
        auto out = std::make_shared<ValueList>();
        if (step > 0) for (double x = start; x < stop; x += step) out->push_back(Value(x));
        else          for (double x = start; x > stop; x += step) out->push_back(Value(x));
        return Value(out);
    });

    def("push", 2, [](Interpreter&, std::vector<Value>& a) -> Value {
        if (!a[0].isList()) throw RuntimeError("push: first argument must be a list");
        a[0].asList()->push_back(a[1]);
        return a[0];
    });
    def("pop", 1, [](Interpreter&, std::vector<Value>& a) -> Value {
        if (!a[0].isList()) throw RuntimeError("pop: argument must be a list");
        auto l = a[0].asList();
        if (l->empty()) throw RuntimeError("pop: list is empty");
        Value v = l->back();
        l->pop_back();
        return v;
    });
    def("keys", 1, [](Interpreter&, std::vector<Value>& a) -> Value {
        if (!a[0].isDict()) throw RuntimeError("keys: argument must be a dict");
        auto out = std::make_shared<ValueList>();
        for (auto& kv : *a[0].asDict()) out->push_back(Value(kv.first));
        return Value(out);
    });
    def("values", 1, [](Interpreter&, std::vector<Value>& a) -> Value {
        if (!a[0].isDict()) throw RuntimeError("values: argument must be a dict");
        auto out = std::make_shared<ValueList>();
        for (auto& kv : *a[0].asDict()) out->push_back(kv.second);
        return Value(out);
    });

    def("ord", 1, [](Interpreter&, std::vector<Value>& a) -> Value {
        if (!a[0].isString() || a[0].asString().empty())
            throw RuntimeError("ord: expected a non-empty string");
        return Value((double)(unsigned char)a[0].asString()[0]);
    });
    def("chr", 1, [](Interpreter&, std::vector<Value>& a) {
        int n = (int)numArg(a[0], "chr");
        return Value(std::string(1, (char)n));
    });

    def("input", -1, [](Interpreter& I, std::vector<Value>& a) -> Value {
        if (!a.empty()) std::cout << I.stringify(a[0]);
        std::string line;
        if (!std::getline(std::cin, line)) return Value();
        return Value(line);
    });

    def("assert", -1, [](Interpreter& I, std::vector<Value>& a) -> Value {
        if (a.empty()) throw RuntimeError("assert: needs a condition");
        if (!a[0].truthy()) {
            std::string msg = a.size() > 1 ? I.stringify(a[1]) : "assertion failed";
            throw RuntimeError("assertion failed: " + msg);
        }
        return Value();
    });
}

} // namespace bee
