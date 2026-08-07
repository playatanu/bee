// Paths, files, and fetching -- the plumbing every command shares.
#include "hive.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifndef HIVE_VERSION
#define HIVE_VERSION "0.3.1"
#endif

namespace fs = std::filesystem;

namespace hive {

// ------------------------------------------------------------------ paths
std::string normalizeSlashes(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

std::string joinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a.back() == '/' || a.back() == '\\') return a + b;
    return a + "/" + b;
}

std::string parentDir(const std::string& path) {
    std::string p = normalizeSlashes(path);
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    size_t slash = p.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return p.substr(0, slash);
}

std::string baseName(const std::string& path) {
    std::string p = normalizeSlashes(path);
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

// ------------------------------------------------------------------ files
bool isFile(const std::string& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

bool isDir(const std::string& path) {
    std::error_code ec;
    return fs::is_directory(path, ec);
}

bool readFile(const std::string& path, std::string& out) {
    if (!isFile(path)) return false;  // never let a directory look like a file
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool ensureDir(const std::string& path, std::string& err) {
    if (isDir(path)) return true;
    std::error_code ec;
    fs::create_directories(path, ec);
    if (ec) {
        err = "cannot create directory '" + path + "': " + ec.message();
        return false;
    }
    return true;
}

bool writeFileMkdirs(const std::string& path, const std::string& data, std::string& err) {
    if (!ensureDir(parentDir(path), err)) return false;
    // Write to a sibling temp file and rename, so an interrupted install can't
    // leave a half-written manifest or lockfile behind.
    std::string tmp = path + ".hive-tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            err = "cannot write '" + path + "'";
            return false;
        }
        f.write(data.data(), (std::streamsize)data.size());
        if (!f) {
            err = "cannot write '" + path + "'";
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(path, ec);
        fs::rename(tmp, path, ec);
    }
    if (ec) {
        err = "cannot replace '" + path + "': " + ec.message();
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

bool removeTree(const std::string& path, std::string& err) {
    std::error_code ec;
    fs::remove_all(path, ec);
    if (ec) {
        err = "cannot remove '" + path + "': " + ec.message();
        return false;
    }
    return true;
}

std::vector<std::string> walkFiles(const std::string& root,
                                   const std::function<bool(const std::string&)>& skip) {
    std::vector<std::string> out;
    if (!isDir(root)) return out;

    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        std::string rel = normalizeSlashes(fs::relative(it->path(), root, ec).generic_string());
        if (ec || rel.empty() || rel == ".") continue;
        if (skip && skip(rel)) {
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(ec)) out.push_back(rel);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string currentDir() {
    std::error_code ec;
    std::string p = fs::current_path(ec).generic_string();
    return ec ? "." : p;
}

std::string findUp(const std::string& start, const std::string& marker) {
    std::error_code ec;
    fs::path dir = fs::absolute(start.empty() ? "." : start, ec);
    if (ec) return "";
    for (;;) {
        if (fs::exists(dir / marker, ec)) return normalizeSlashes(dir.generic_string());
        fs::path up = dir.parent_path();
        if (up == dir || up.empty()) return "";
        dir = up;
    }
}

// ------------------------------------------------------------------ hive dirs
std::string homeDir() {
    if (const char* h = std::getenv("HOME")) if (*h) return normalizeSlashes(h);
#ifdef _WIN32
    const char* up = std::getenv("USERPROFILE");
    if (up && *up) return normalizeSlashes(up);
    const char* drive = std::getenv("HOMEDRIVE");
    const char* rest = std::getenv("HOMEPATH");
    if (drive && rest) return normalizeSlashes(std::string(drive) + rest);
#endif
    return ".";
}

std::string hiveHome() {
    if (const char* h = std::getenv("HIVE_HOME")) if (*h) return normalizeSlashes(h);
    return joinPath(homeDir(), ".hive");
}

std::string globalLibDir() { return joinPath(hiveHome(), "lib"); }
std::string cacheDir()     { return joinPath(hiveHome(), "cache"); }

// ------------------------------------------------------------------ fetching
bool looksLikeUrl(const std::string& s) {
    return s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0 ||
           s.rfind("file://", 0) == 0;
}

namespace {

// Only these characters may reach the shell. Anything else -- quotes, spaces,
// backticks, `$`, `;` -- is rejected outright rather than escaped, so a hostile
// registry can't smuggle a command into the download step.
bool urlIsShellSafe(const std::string& url) {
    static const std::string ok =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
        "-._~:/?#[]@!$&()*+,;=%";
    if (url.empty() || url.size() > 2048) return false;
    for (char c : url) {
        // '$' and '&' and ';' are legal in URLs but dangerous unquoted; we
        // single-quote the argument, which neutralises them. A single quote
        // itself would break out of that quoting, so it stays banned.
        if (ok.find(c) == std::string::npos) return false;
    }
    return url.find('\'') == std::string::npos;
}

std::string quoteArg(const std::string& s) {
#ifdef _WIN32
    return "\"" + s + "\"";  // validated above: no quotes, no spaces
#else
    return "'" + s + "'";
#endif
}

bool haveTool(const std::string& tool) {
#ifdef _WIN32
    std::string probe = "where " + tool + " >nul 2>nul";
#else
    std::string probe = "command -v " + tool + " >/dev/null 2>&1";
#endif
    return std::system(probe.c_str()) == 0;
}

std::string tempPath(const std::string& tag) {
    std::error_code ec;
    fs::path dir = fs::temp_directory_path(ec);
    if (ec) dir = ".";
#ifdef _WIN32
    long pid = (long)_getpid();
#else
    long pid = (long)getpid();
#endif
    static int counter = 0;
    return normalizeSlashes(
        (dir / ("hive-" + tag + "-" + std::to_string(pid) + "-" + std::to_string(counter++))).generic_string());
}

int runQuiet(const std::string& cmd) {
    int status = std::system(cmd.c_str());
#ifndef _WIN32
    if (status != -1 && WIFEXITED(status)) return WEXITSTATUS(status);
#endif
    return status;
}

}  // namespace

// Quote a filesystem path for the shell. quoteArg() above is for URLs, which
// are validated to contain no spaces or quotes first; a real install path has
// no such guarantee ("C:\Users\John Doe\..."), so it gets escaped properly.
static std::string quotePath(const std::string& p) {
#ifdef _WIN32
    // cmd.exe has no escape for a double quote inside a quoted string, but a
    // Windows path can never contain one, so quoting is enough.
    return "\"" + p + "\"";
#else
    std::string out = "'";
    for (char c : p) {
        if (c == '\'') out += "'\\''";   // close, escaped quote, reopen
        else out += c;
    }
    return out + "'";
#endif
}

std::string hostPlatform() {
#if defined(_WIN32)
    const char* os = "windows";
#elif defined(__APPLE__)
    const char* os = "darwin";
#elif defined(__linux__)
    const char* os = "linux";
#elif defined(__FreeBSD__)
    const char* os = "freebsd";
#else
    const char* os = "unknown";
#endif

#if defined(__x86_64__) || defined(_M_X64)
    const char* arch = "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    const char* arch = "aarch64";
#elif defined(__i386__) || defined(_M_IX86)
    const char* arch = "x86";
#elif defined(__arm__)
    const char* arch = "arm";
#else
    const char* arch = "unknown";
#endif
    return std::string(os) + "-" + arch;
}

int runInDir(const std::string& dir, const std::string& cmd, std::string& out) {
    out.clear();
    const std::string log = tempPath("build");
#ifdef _WIN32
    // `cd /d` so a package on another drive still works.
    std::string full = "cd /d " + quotePath(dir) + " && (" + cmd + ") > " + quotePath(log) +
                       " 2>&1";
#else
    std::string full = "cd " + quotePath(dir) + " && { " + cmd + " ; } > " + quotePath(log) +
                       " 2>&1";
#endif
    int status = runQuiet(full);
    readFile(log, out);
    std::error_code ec;
    fs::remove(log, ec);
    return status;
}

bool httpGet(const std::string& url, std::string& out, std::string& err) {
    // A local directory or file works as a registry, which keeps testing (and
    // air-gapped mirrors) free of any HTTP server.
    std::string localPath;
    if (url.rfind("file://", 0) == 0) {
        localPath = url.substr(7);
        // file:///abs/path -> /abs/path
        if (localPath.size() > 2 && localPath[0] == '/' && localPath[2] == ':')
            localPath.erase(0, 1);  // Windows: file:///C:/x
    } else if (!looksLikeUrl(url)) {
        localPath = url;
    }
    if (!localPath.empty()) {
        if (!readFile(localPath, out)) {
            err = "cannot read '" + localPath + "'";
            return false;
        }
        return true;
    }

    if (!urlIsShellSafe(url)) {
        err = "refusing to fetch a URL with unexpected characters: " + url;
        return false;
    }

    const std::string tmp = tempPath("dl");
    const std::string agent = std::string("hive/") + HIVE_VERSION;
    int code = -1;

    if (haveTool("curl")) {
        std::string cmd = "curl -fsSL --proto =http,https --max-time 120 -A " + quoteArg(agent) +
                          " -o " + quoteArg(tmp) + " " + quoteArg(url);
        code = runQuiet(cmd);
    } else if (haveTool("wget")) {
        std::string cmd = "wget -q -U " + quoteArg(agent) + " -O " + quoteArg(tmp) + " " + quoteArg(url);
        code = runQuiet(cmd);
    } else {
        err = "no downloader found -- install curl or wget (or use --offline)";
        return false;
    }

    std::error_code ec;
    if (code != 0) {
        fs::remove(tmp, ec);
        err = "download failed (exit " + std::to_string(code) + "): " + url;
        return false;
    }
    if (!readFile(tmp, out)) {
        fs::remove(tmp, ec);
        err = "download produced no data: " + url;
        return false;
    }
    fs::remove(tmp, ec);
    return true;
}

}  // namespace hive
