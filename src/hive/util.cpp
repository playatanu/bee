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
#define HIVE_VERSION "0.3.0"
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
