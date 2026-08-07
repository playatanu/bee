// beegen -- the BeeLang binding generator.
#include "beegen.hpp"

#include <filesystem>
#include <iostream>

#ifndef BEEGEN_VERSION
#define BEEGEN_VERSION "0.3.1"
#endif

namespace fs = std::filesystem;

namespace {

void usage(std::ostream& out, const char* prog) {
    out << "beegen " << BEEGEN_VERSION << " - generate BeeLang bindings from C++ headers\n\n"
        << "usage: " << prog << " <header...> --module <name> [options] [-- clang args]\n\n"
        << "Reads the headers with libclang and writes a native module plus a BeeLang\n"
        << "wrapper, so `import <name>` calls into the real library.\n\n"
        << "options:\n"
        << "  -m, --module <name>    the BeeLang module to generate (required)\n"
        << "  -o, --out-dir <dir>    where to write the output (default: .)\n"
        << "      --namespace <ns>   only bind declarations in this namespace (repeatable)\n"
        << "      --prefix <p>       only bind names starting with this (repeatable)\n"
        << "      --skip <name>      leave this function, class or method out (repeatable)\n"
        << "      --no-classes       bind free functions and enums only\n"
        << "      --no-enums         don't bind enum constants\n"
        << "      --no-manifest      don't write hive.json\n"
        << "  -I <dir>               add an include directory (passed to clang)\n"
        << "      --std <std>        C++ standard to parse with (default: c++17)\n"
        << "      --bee-src <dir>    path to bee's src/, used in the build script\n"
        << "      --libclang <path>  a specific libclang shared library\n"
        << "  -q, --quiet            print only warnings and errors\n"
        << "  -v, --version          print the version and exit\n"
        << "  -h, --help             print this help and exit\n\n"
        << "Everything after `--` is passed to clang verbatim, for the awkward cases:\n"
        << "  " << prog << " raylib.h -m raylib -- -I/usr/local/include -DPLATFORM_DESKTOP\n\n"
        << "Then build and use it:\n"
        << "  ./build.sh && bee -e 'import raylib; print(raylib.__module)'\n";
}

// Guess where bee's headers live, so build.sh works without being told. Looks
// for src/bee_native.hpp beside the running binary and in the usual places.
std::string guessBeeSrc(const char* argv0) {
    std::error_code ec;
    std::vector<fs::path> candidates;
    fs::path self = fs::weakly_canonical(fs::path(argv0), ec);
    if (!ec) {
        candidates.push_back(self.parent_path() / "src");            // repo checkout
        candidates.push_back(self.parent_path().parent_path() / "src");
        candidates.push_back(self.parent_path().parent_path() / "include" / "bee");
    }
    candidates.push_back("/usr/local/include/bee");
    candidates.push_back("/usr/include/bee");
    for (auto& dir : candidates)
        if (fs::exists(dir / "bee_native.hpp", ec)) return dir.string();
    return "";
}

}  // namespace

int main(int argc, char** argv) {
    const char* prog = argc ? argv[0] : "beegen";
    beegen::Options opts;
    std::vector<std::string> clangExtra;
    std::string standard = "c++17";
    bool afterDashDash = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto value = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "beegen: " << flag << " needs a value\n";
                std::exit(64);
            }
            return argv[++i];
        };

        if (afterDashDash)                    { clangExtra.push_back(a); continue; }
        if (a == "--")                        { afterDashDash = true; continue; }
        if (a == "-h" || a == "--help")       { usage(std::cout, prog); return 0; }
        if (a == "-v" || a == "--version")    { std::cout << "beegen " << BEEGEN_VERSION << "\n"; return 0; }
        if (a == "-m" || a == "--module")     { opts.module = value("--module"); continue; }
        if (a == "-o" || a == "--out-dir")    { opts.outDir = value("--out-dir"); continue; }
        if (a == "--namespace")               { opts.namespaces.push_back(value("--namespace")); continue; }
        if (a == "--prefix")                  { opts.prefixes.push_back(value("--prefix")); continue; }
        if (a == "--skip")                    { opts.skip.push_back(value("--skip")); continue; }
        if (a == "--no-classes")              { opts.bindClasses = false; continue; }
        if (a == "--no-enums")                { opts.bindEnums = false; continue; }
        if (a == "--no-manifest")             { opts.writeManifest = false; continue; }
        if (a == "--std")                     { standard = value("--std"); continue; }
        if (a == "--bee-src")                 { opts.beeSrc = value("--bee-src"); continue; }
        if (a == "--libclang")                { opts.libclangPath = value("--libclang"); continue; }
        if (a == "-q" || a == "--quiet")      { opts.quiet = true; continue; }
        if (a.rfind("-I", 0) == 0) {
            opts.clangArgs.push_back(a.size() > 2 ? a : "-I" + value("-I"));
            continue;
        }
        if (a.size() > 1 && a[0] == '-') {
            std::cerr << "beegen: unknown option '" << a << "'\n"
                      << "try '" << prog << " --help' for more information.\n";
            return 64;
        }
        opts.headers.push_back(a);
    }

    if (opts.headers.empty() || opts.module.empty()) {
        usage(std::cerr, prog);
        return 64;  // EX_USAGE
    }
    for (auto& h : opts.headers) {
        if (!fs::exists(h)) {
            std::cerr << "beegen: no such header '" << h << "'\n";
            return 66;  // EX_NOINPUT
        }
    }
    if (opts.beeSrc.empty()) opts.beeSrc = guessBeeSrc(prog);

    // Parse as C++ regardless of the extension: a .h from a C library still has
    // to be read the way the generated module will compile it.
    std::vector<std::string> clangArgs = {"-x", "c++", "-std=" + standard};
    for (auto& a : opts.clangArgs) clangArgs.push_back(a);
    for (auto& a : clangExtra) {
        clangArgs.push_back(a);
        if (a.rfind("-I", 0) == 0) opts.clangArgs.push_back(a);  // build.sh needs these too
    }

    beegen::Clang clang;
    std::string err;
    if (!clang.load(opts.libclangPath, err)) {
        std::cerr << "beegen: " << err << "\n";
        return 69;  // EX_UNAVAILABLE
    }

    // One translation unit covering every header, so types declared in one and
    // used in another resolve.
    std::string combined;
    for (auto& h : opts.headers) combined += "#include \"" + fs::absolute(h).string() + "\"\n";
    const std::string unitPath = (fs::temp_directory_path() / ("beegen-unit-" + opts.module + ".cpp")).string();
    if (!beegen::writeFile(unitPath, combined, err)) {
        std::cerr << "beegen: " << err << "\n";
        return 73;  // EX_CANTCREAT
    }

    std::vector<std::string> diagnostics;
    if (!clang.parse(unitPath, clangArgs, diagnostics, err)) {
        std::cerr << "beegen: " << err << "\n";
        for (auto& d : diagnostics) std::cerr << "  " << d << "\n";
        return 65;  // EX_DATAERR
    }
    // Errors don't stop generation -- a header that half-parses still yields
    // usable bindings -- but they are never hidden, because they usually mean a
    // missing -I and a silently smaller API.
    if (!diagnostics.empty()) {
        std::cerr << "beegen: clang reported " << diagnostics.size()
                  << " error(s); bindings may be incomplete\n";
        size_t shown = 0;
        for (auto& d : diagnostics) {
            if (shown++ == 8) {
                std::cerr << "  ... " << (diagnostics.size() - 8) << " more\n";
                break;
            }
            std::cerr << "  " << d << "\n";
        }
    }

    beegen::Api api;
    if (!beegen::scan(clang, opts, api, err)) {
        std::cerr << "beegen: " << err << "\n";
        return 70;  // EX_SOFTWARE
    }
    std::error_code ec;
    fs::remove(unitPath, ec);

    size_t methodCount = 0, fieldCount = 0;
    for (auto& c : api.classes) {
        methodCount += c.methods.size();
        fieldCount += c.fields.size();
    }
    if (api.functions.empty() && api.classes.empty() && api.enums.empty()) {
        std::cerr << "beegen: nothing to bind. Check the header path, your -I flags, and\n"
                  << "        any --namespace / --prefix filters.\n";
        if (!api.skipped.empty()) {
            std::cerr << "        " << api.skipped.size() << " declaration(s) were skipped:\n";
            for (size_t i = 0; i < api.skipped.size() && i < 10; ++i)
                std::cerr << "          " << api.skipped[i].what << " -- "
                          << api.skipped[i].reason << "\n";
        }
        return 65;
    }

    std::vector<std::string> written;
    if (!beegen::emit(opts, api, written, err)) {
        std::cerr << "beegen: " << err << "\n";
        return 73;
    }

    if (!opts.quiet) {
        std::cout << "beegen: " << api.functions.size() << " function(s), " << api.classes.size()
                  << " class(es) (" << methodCount << " method(s), " << fieldCount
                  << " field(s)), " << api.enums.size() << " enum(s)\n";
        for (auto& w : written) std::cout << "  wrote " << w << "\n";
    }

    // Always report the gaps: a binding that silently omits half a library is
    // worse than one that says what it left out.
    if (!api.skipped.empty()) {
        std::cout << "\nskipped " << api.skipped.size() << " declaration(s):\n";
        for (auto& s : api.skipped) std::cout << "  " << s.what << " -- " << s.reason << "\n";
    }

    if (!opts.quiet) {
        std::cout << "\nnext:\n"
                  << "  cd " << opts.outDir << " && ./build.sh\n"
                  << "  bee -e 'import " << opts.module << "; print(" << opts.module
                  << ".__module)'\n";
        if (opts.beeSrc.empty())
            std::cout << "\nnote: bee's src/ wasn't found, so build.sh needs BEE_SRC set:\n"
                      << "      BEE_SRC=/path/to/beelang/src ./build.sh\n";
    }
    return 0;
}
