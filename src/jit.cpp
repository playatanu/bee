//
// Front end to the LLVM JIT. The backend (jit_llvm.cpp) pulls in ~120MB of
// libLLVM, so it lives in a separate shared object -- libbee_jit.so -- that this
// loads with dlopen the first time a script compiles a function. That keeps
// libLLVM off the `bee` executable's link line: a plain script run, which is
// most of them, never maps the library and starts in ~1ms instead of paying the
// loader on every invocation. It is the same trick beegen uses for libclang
// (see src/beegen/clang.cpp).
//
// The backend calls back into the interpreter (Interpreter::globals,
// Value/Environment accessors); those resolve at load time against the `bee`
// executable, whose symbols are exported by -rdynamic -- exactly as native
// modules resolve Interpreter::callValue.
//
#include "jit.hpp"

#include <cstdlib>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <limits.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace bee {

namespace {

#ifdef _WIN32
const char* kJitLib = "bee_jit.dll";
#elif defined(__APPLE__)
const char* kJitLib = "libbee_jit.dylib";
#else
const char* kJitLib = "libbee_jit.so";
#endif

// Directory of the running executable, with a trailing separator, or "" if it
// cannot be determined. The backend ships alongside `bee`, so this is where we
// look first.
std::string selfDir() {
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, (DWORD)sizeof buf);
    if (n == 0 || n >= sizeof buf) return "";
    std::string p(buf, n);
    auto slash = p.find_last_of("\\/");
    return slash == std::string::npos ? "" : p.substr(0, slash + 1);
#elif defined(__APPLE__)
    char buf[PATH_MAX];
    uint32_t sz = sizeof buf;
    if (_NSGetExecutablePath(buf, &sz) != 0) return "";
    std::string p(buf);
    auto slash = p.find_last_of('/');
    return slash == std::string::npos ? "" : p.substr(0, slash + 1);
#else
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return "";
    buf[n] = '\0';
    std::string p(buf);
    auto slash = p.find_last_of('/');
    return slash == std::string::npos ? "" : p.substr(0, slash + 1);
#endif
}

void* libOpen(const char* path) {
#ifdef _WIN32
    return reinterpret_cast<void*>(LoadLibraryA(path));
#else
    // RTLD_NOW so any interpreter symbol the backend needs is resolved now: if
    // one is missing the dlopen fails and we fall back to the interpreter,
    // rather than crashing on the first compiled call.
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}

void* libSym(void* lib, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), name));
#else
    return dlsym(lib, name);
#endif
}

// BEE_NO_JIT=1 forces everything onto the VM/interpreter. The counterpart of
// BEE_NO_VM, and the only way to attribute a measurement to the native tier
// rather than guess at it -- so it suppresses the loop JIT as well as functions.
bool jitDisabled() {
    static const bool disabled = [] {
        const char* v = std::getenv("BEE_NO_JIT");
        return v && *v && std::string(v) != "0";
    }();
    return disabled;
}

}  // namespace

JitBackend* Jit::backend() {
    if (triedLoad_) return backend_;
    triedLoad_ = true;

    std::vector<std::string> candidates;
    if (const char* env = std::getenv("BEE_JIT_LIB")) if (*env) candidates.push_back(env);
    std::string dir = selfDir();
    if (!dir.empty()) {
        candidates.push_back(dir + kJitLib);                    // next to the binary
        candidates.push_back(dir + "../lib/bee/" + kJitLib);    // FHS: /usr/bin -> /usr/lib/bee
    }
    candidates.push_back(kJitLib);                              // loader search path
#ifndef _WIN32
    candidates.push_back(std::string("/usr/lib/bee/") + kJitLib);
    candidates.push_back(std::string("/usr/local/lib/bee/") + kJitLib);
#endif

    for (auto& path : candidates) {
        lib_ = libOpen(path.c_str());
        if (lib_) break;
    }
    if (!lib_) return nullptr;   // no backend: everything runs on the interpreter/VM

    auto create = (BeeJitCreateFn)libSym(lib_, "bee_jit_create");
    if (create) backend_ = create();
    return backend_;
}

Jit::~Jit() {
    // The interpreter is going away with us; nothing will call the compiled code
    // again, so it is safe to tear down the engine and unmap the library.
    delete backend_;
    backend_ = nullptr;
    if (lib_) {
#ifdef _WIN32
        FreeLibrary(static_cast<HMODULE>(lib_));
#else
        dlclose(lib_);
#endif
        lib_ = nullptr;
    }
}

JitFn Jit::getCompiled(const FunctionStmt* fn, JitSig sig, Interpreter& interp) {
    if (jitDisabled()) return nullptr;

    SigKey key{fn, sig};
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;      // may be a cached failure

    if (!jitCandidate(fn)) { cache_[key] = nullptr; return nullptr; }
    JitBackend* b = backend();
    if (!b) { cache_[key] = nullptr; return nullptr; }

    std::vector<JitCacheEntry> extra;
    JitFn result = b->compile(fn, sig, interp, extra);
    // Cache the callees compiled alongside `fn` so a later direct call reuses
    // this native code instead of recompiling.
    for (auto& e : extra) cache_[SigKey{e.fn, e.sig}] = e.ptr;
    cache_[key] = result;                           // caches negative results too
    return result;
}

const CompiledLoop& Jit::getCompiledLoop(const Stmt* loop, Interpreter& interp) {
    auto it = loopCache_.find(loop);
    if (it != loopCache_.end()) return it->second;

    CompiledLoop cl;   // fn == nullptr sentinel == "cannot compile"
    if (!jitDisabled())
        if (JitBackend* b = backend()) b->compileLoop(loop, interp, cl);

    auto res = loopCache_.emplace(loop, std::move(cl));
    return res.first->second;
}

}  // namespace bee
