//
// beec -- the Bee ahead-of-time compiler.
//
// Translates a .bee program to C++ (see aot_codegen.cpp), then invokes the
// system C++ compiler to link it against the Bee runtime into a standalone
// native executable. The produced binary contains machine code for the whole
// program and embeds the runtime, so it runs with no `bee` interpreter present.
//
#include "lexer.hpp"
#include "parser.hpp"
#include "resolver.hpp"
#include "aot_codegen.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <sys/stat.h>
#ifndef _WIN32
#include <limits.h>
#include <unistd.h>
#endif
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef BEE_VERSION
#define BEE_VERSION "0.3.3"
#endif

// Where the AOT headers live and where the runtime archive is. Baked in by the
// Makefile; overridable at run time via the environment for a relocated install.
#ifndef BEE_AOT_INCDIR
#define BEE_AOT_INCDIR "."
#endif
#ifndef BEE_AOT_RUNTIME_LIB
#define BEE_AOT_RUNTIME_LIB "libbee_runtime.a"
#endif
#ifndef BEE_AOT_CXX
#define BEE_AOT_CXX "c++"
#endif

namespace {

std::string envOr(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : std::string(fallback);
}

// Single-quote a path for the shell, escaping embedded single quotes.
std::string shq(const std::string& s) {
    std::string o = "'";
    for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; }
    o += "'";
    return o;
}

std::string baseName(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    std::string b = (slash == std::string::npos) ? path : path.substr(slash + 1);
    size_t dot = b.find_last_of('.');
    return (dot == std::string::npos) ? b : b.substr(0, dot);
}

std::string readFile(const std::string& path, bool& ok) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { ok = false; return ""; }
    std::ostringstream ss; ss << f.rdbuf(); ok = true;
    return ss.str();
}

std::string dirOf(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? std::string(".") : path.substr(0, slash);
}

bool fileExists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && !S_ISDIR(st.st_mode);
}
bool dirExists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string homeDir() {
    const char* h = std::getenv("HOME");
    return (h && *h) ? std::string(h) : std::string(".");
}
std::string hiveHome() {
    const char* h = std::getenv("HIVE_HOME");
    return (h && *h) ? std::string(h) : (homeDir() + "/.hive");
}

// Best-effort absolute path, so we can walk up the tree cleanly.
std::string realDir(const std::string& d) {
#ifdef _WIN32
    char buf[PATH_MAX];
    if (::_fullpath(buf, d.c_str(), PATH_MAX)) return std::string(buf);
#else
    char buf[PATH_MAX];
    if (::realpath(d.c_str(), buf)) return std::string(buf);
#endif
    return d;
}

// Minimal extraction of a top-level JSON string field (e.g. hive.json "main").
std::string jsonStringField(const std::string& j, const std::string& key) {
    std::string pat = "\"" + key + "\"";
    size_t k = j.find(pat);
    if (k == std::string::npos) return "";
    size_t colon = j.find(':', k + pat.size());
    if (colon == std::string::npos) return "";
    size_t q = j.find('"', colon + 1);
    if (q == std::string::npos) return "";
    std::string out;
    for (size_t i = q + 1; i < j.size(); ++i) {
        char c = j[i];
        if (c == '\\' && i + 1 < j.size()) { out += j[i + 1]; ++i; continue; }
        if (c == '"') break;
        out += c;
    }
    return out;
}

// Every `hive_modules` directory from `importerDir` up to the filesystem root --
// which is how an installed package resolves its own dependencies from the one
// flat directory, and how a project finds a package installed at its root.
std::vector<std::string> hiveModuleRoots(const std::string& importerDir) {
    std::vector<std::string> roots;
    std::string d = realDir(importerDir);
    for (;;) {
        roots.push_back(d + "/hive_modules");
        if (d == "/" || d.empty()) break;
        size_t slash = d.find_last_of("/\\");   // handle both separators (Windows too)
        if (slash == std::string::npos) break;   // e.g. a Windows drive root "C:"
        d = (slash == 0) ? "/" : d.substr(0, slash);
    }
    return roots;
}

// The entry module of a package directory: its hive.json "main", else the usual
// fallbacks.
std::string packageEntry(const std::string& pkgDir, const std::string& lastName) {
    std::string manifest = pkgDir + "/hive.json";
    if (fileExists(manifest)) {
        bool ok = false;
        std::string j = readFile(manifest, ok);
        if (ok) {
            std::string main = jsonStringField(j, "main");
            if (!main.empty() && fileExists(pkgDir + "/" + main)) return pkgDir + "/" + main;
        }
    }
    const std::string fallbacks[] = { "/init.bee", "/init.be", "/" + lastName + ".bee", "/main.bee" };
    for (const auto& f : fallbacks) if (fileExists(pkgDir + f)) return pkgDir + f;
    return "";
}

// Resolve a module name (dotted -> '/'-joined) to a file, matching the
// interpreter's search order: the importing file's directory, its lib/ folder,
// every hive_modules/ up the tree, $BEE_PATH, then the global ~/.hive/lib. At
// each root a plain file wins; otherwise a directory is treated as a package.
std::string resolveModule(const std::string& name, const std::string& dir) {
    std::vector<std::string> roots;
    roots.push_back(dir);
    roots.push_back(dir + "/lib");
    for (auto& r : hiveModuleRoots(dir)) roots.push_back(r);
    if (const char* bp = std::getenv("BEE_PATH")) {
        std::string s = bp;
        size_t p = 0;
        while (p <= s.size()) {
            size_t c = s.find(':', p);
            std::string e = s.substr(p, c == std::string::npos ? std::string::npos : c - p);
            if (!e.empty()) roots.push_back(e);
            if (c == std::string::npos) break;
            p = c + 1;
        }
    }
    roots.push_back(hiveHome() + "/lib");

    std::string lastName = name;
    size_t sl = name.find_last_of('/');
    if (sl != std::string::npos) lastName = name.substr(sl + 1);

    for (const auto& R : roots) {
        std::string base = R + "/" + name;
        if (fileExists(base + ".bee")) return base + ".bee";
        if (fileExists(base + ".be"))  return base + ".be";
        if (fileExists(base))          return base;                 // extensionless file
        if (dirExists(base)) {                                       // a package directory
            std::string e = packageEntry(base, lastName);
            if (!e.empty()) return e;
        }
    }
    return "";
}

bool parseFile(const std::string& path, bee::Program& out, std::string& err) {
    bool ok = false;
    std::string src = readFile(path, ok);
    if (!ok) { err = "cannot read '" + path + "'"; return false; }
    try {
        bee::Lexer lx(src);
        bee::Parser ps(lx.tokenize());
        out = ps.parse();
        bee::Resolver().resolve(out);
    } catch (const bee::LexError& e) {
        err = "Lex error in " + path + ":" + std::to_string(e.line) + ": " + e.message; return false;
    } catch (const bee::ParseError& e) {
        err = "Parse error in " + path + ":" + std::to_string(e.line) + ": " + e.message; return false;
    } catch (const std::exception& e) {
        err = std::string(e.what()); return false;
    }
    return true;
}

// Top-level `import` module names in a program.
std::vector<std::string> topLevelImports(const bee::Program& program) {
    std::vector<std::string> out;
    for (auto& s : program)
        if (s->kind == bee::Stmt::Kind::Import)
            out.push_back(static_cast<bee::ImportStmt*>(s.get())->moduleName);
    return out;
}

// Public (non-underscore) top-level bindings, for `from M import *`.
std::vector<std::string> publicNames(const bee::Program& program) {
    std::vector<std::string> out;
    auto pub = [&](const std::string& n) { if (!n.empty() && n[0] != '_') out.push_back(n); };
    for (auto& s : program) {
        switch (s->kind) {
            case bee::Stmt::Kind::Let: {
                auto* l = static_cast<bee::LetStmt*>(s.get());
                if (l->isDestructure) for (auto& n : l->names) pub(n);
                else pub(l->name);
                break;
            }
            case bee::Stmt::Kind::Function: pub(static_cast<bee::FunctionStmt*>(s.get())->name); break;
            case bee::Stmt::Kind::Class:    pub(static_cast<bee::ClassStmt*>(s.get())->name); break;
            default: break;
        }
    }
    return out;
}

struct ModCtx {
    std::vector<std::unique_ptr<bee::Program>>& owned;
    std::vector<bee::AotModule>& modules;
    std::map<std::string, int>& seen;
};

// Resolve, parse, and recursively collect an imported module (and its imports).
bool collectModule(const std::string& name, const std::string& importerDir,
                   ModCtx& ctx, std::string& err) {
    if (ctx.seen.count(name)) return true;   // already collected (also breaks cycles)
    std::string path = resolveModule(name, importerDir);
    if (path.empty()) {
        err = "cannot resolve module '" + name + "' (searched " + importerDir + ", its lib/ "
              "folder, every hive_modules/ up the tree, $BEE_PATH, and ~/.hive/lib; note the AOT "
              "compiler cannot compile native .so modules)";
        return false;
    }
    auto prog = std::make_unique<bee::Program>();
    if (!parseFile(path, *prog, err)) return false;

    int idx = (int)ctx.modules.size();
    ctx.seen[name] = idx;                    // register before recursing (cycles)
    ctx.owned.push_back(std::move(prog));
    const bee::Program* p = ctx.owned.back().get();

    bee::AotModule mod;
    mod.id = "m" + std::to_string(idx);
    mod.name = name;
    mod.program = p;
    mod.publicNames = publicNames(*p);
    ctx.modules.push_back(std::move(mod));

    std::string mdir = dirOf(path);
    for (const auto& dep : topLevelImports(*p))
        if (!collectModule(dep, mdir, ctx, err)) return false;
    return true;
}

void usage(const char* prog) {
    std::cout <<
        "beec " BEE_VERSION " - the Bee ahead-of-time compiler\n\n"
        "usage: " << prog << " <input.bee> [options]\n\n"
        "options:\n"
        "  -o <file>      output executable name (default: input name without extension)\n"
        "  --emit-cpp     also keep the generated C++ next to the output\n"
        "  -O <level>     optimisation level passed to the C++ compiler (default: 2)\n"
        "  -v, --version  print the version and exit\n"
        "  -h, --help     print this help and exit\n\n"
        "environment:\n"
        "  BEE_CXX               C++ compiler to use (default: " BEE_AOT_CXX ")\n"
        "  BEE_AOT_INCDIR        directory holding bee_aot.hpp and the runtime headers\n"
        "  BEE_AOT_RUNTIME_LIB   path to libbee_runtime.a\n";
}

} // namespace

int main(int argc, char** argv) {
    std::string input, output;
    std::string optLevel = "2";
    bool emitCpp = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
        if (a == "-v" || a == "--version") { std::cout << "beec " << BEE_VERSION << "\n"; return 0; }
        if (a == "--emit-cpp") { emitCpp = true; continue; }
        if (a == "-o") { if (++i >= argc) { std::cerr << "beec: -o needs a filename\n"; return 64; } output = argv[i]; continue; }
        if (a == "-O") { if (++i >= argc) { std::cerr << "beec: -O needs a level\n"; return 64; } optLevel = argv[i]; continue; }
        if (!a.empty() && a[0] == '-') { std::cerr << "beec: unknown option '" << a << "'\n"; return 64; }
        if (input.empty()) input = a;
        else { std::cerr << "beec: unexpected extra argument '" << a << "'\n"; return 64; }
    }

    if (input.empty()) { usage(argv[0]); return 64; }
    if (output.empty()) output = baseName(input);

    bool ok = false;
    std::string src = readFile(input, ok);
    if (!ok) { std::cerr << "beec: cannot read '" << input << "'\n"; return 66; }

    // Front end: reuse the interpreter's lexer, parser and resolver.
    bee::Program program;
    try {
        bee::Lexer lx(src);
        bee::Parser ps(lx.tokenize());
        program = ps.parse();
        bee::Resolver().resolve(program);
    } catch (const bee::LexError& e) {
        std::cerr << "Lex error: " << e.message << "\n  at " << input << ":" << e.line << "\n";
        return 65;
    } catch (const bee::ParseError& e) {
        std::cerr << "Parse error: " << e.message << "\n  at " << input << ":" << e.line << "\n";
        return 65;
    } catch (const std::exception& e) {
        std::cerr << "beec: " << e.what() << "\n";
        return 65;
    }

    // Resolve and parse imported modules (recursively) so they compile into the
    // same binary.
    std::vector<std::unique_ptr<bee::Program>> ownedModules;
    std::vector<bee::AotModule> modules;
    std::map<std::string, int> seen;
    ModCtx ctx{ ownedModules, modules, seen };
    {
        std::string merr;
        for (const auto& dep : topLevelImports(program))
            if (!collectModule(dep, dirOf(input), ctx, merr)) {
                std::cerr << "beec: " << merr << "\n";
                return 1;
            }
    }

    // Code generation.
    std::vector<bee::AotError> errs;
    std::string cpp = bee::aotGenerate(program, input, modules, errs);
    if (!errs.empty()) {
        std::cerr << "beec: cannot compile '" << input << "': "
                  << errs.size() << " unsupported construct(s):\n";
        for (auto& e : errs)
            std::cerr << "  " << input << ":" << e.line << ": " << e.msg << "\n";
        std::cerr << "\nThese features aren't in the AOT compiler yet. Run the program with the\n"
                     "`bee` interpreter instead, which supports the whole language.\n";
        return 1;
    }

    // Write the generated C++.
    std::string cppPath = output + ".beec.cpp";
    {
        std::ofstream f(cppPath, std::ios::binary);
        if (!f) { std::cerr << "beec: cannot write '" << cppPath << "'\n"; return 73; }
        f << cpp;
    }

    // Invoke the C++ compiler to link against the runtime.
    std::string cxx     = envOr("BEE_CXX", BEE_AOT_CXX);
    std::string incdir  = envOr("BEE_AOT_INCDIR", BEE_AOT_INCDIR);
    std::string runtime = envOr("BEE_AOT_RUNTIME_LIB", BEE_AOT_RUNTIME_LIB);

    std::string cmd = cxx + " -std=c++17 -O" + optLevel + " -pthread "
                    + "-I" + shq(incdir) + " "
                    + shq(cppPath) + " " + shq(runtime) + " "
                    + "-o " + shq(output)
#ifdef __linux__
                    + " -ldl -rdynamic"
#endif
                    ;

    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        std::cerr << "beec: the C++ compiler failed (kept the generated source at "
                  << cppPath << ")\n";
        return 1;
    }

    if (!emitCpp) std::remove(cppPath.c_str());
    std::cerr << "beec: wrote " << output << "\n";
    return 0;
}
