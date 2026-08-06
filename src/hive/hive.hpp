#pragma once
// Hive -- the BeeLang package manager. Shared declarations.
//
// Hive is a separate binary from `bee` on purpose: the interpreter stays a
// self-contained language runtime with no networking, and the package manager
// stays a plain command-line tool. The only contract between them is on disk --
// the layout of `hive_modules/` and of the global library root, which the
// interpreter knows how to search (see Interpreter::resolveModulePath).
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace hive {

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------
// A minimal DOM. Objects keep insertion order so rewriting a manifest doesn't
// shuffle the user's keys around.
struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0;
    std::string text;
    std::vector<Json> items;                            // Array
    std::vector<std::pair<std::string, Json>> members;  // Object

    Json() = default;
    explicit Json(bool b) : type(Type::Bool), boolean(b) {}
    explicit Json(double n) : type(Type::Number), number(n) {}
    explicit Json(std::string s) : type(Type::String), text(std::move(s)) {}
    explicit Json(const char* s) : type(Type::String), text(s) {}

    static Json array() { Json j; j.type = Type::Array; return j; }
    static Json object() { Json j; j.type = Type::Object; return j; }

    bool isNull()   const { return type == Type::Null; }
    bool isBool()   const { return type == Type::Bool; }
    bool isNumber() const { return type == Type::Number; }
    bool isString() const { return type == Type::String; }
    bool isArray()  const { return type == Type::Array; }
    bool isObject() const { return type == Type::Object; }

    // Object lookup. Returns nullptr when the key is absent -- or when this
    // value isn't an object at all, so callers can probe untrusted input
    // without checking the type first.
    const Json* find(const std::string& key) const;
    Json* find(const std::string& key);

    // Typed object lookups with fallbacks, for the same reason.
    std::string str(const std::string& key, const std::string& fallback = "") const;
    double num(const std::string& key, double fallback = 0) const;
    bool flag(const std::string& key, bool fallback = false) const;

    Json& set(const std::string& key, Json value);  // insert or replace
    void erase(const std::string& key);
    void push(Json value);

    // `indent` of 0 prints compact, otherwise pretty-prints with that many
    // spaces per level.
    std::string dump(int indent = 0) const;

    static bool parse(const std::string& src, Json& out, std::string& err);
};

// ---------------------------------------------------------------------------
// SHA-256
// ---------------------------------------------------------------------------
// Lowercase hex digest. Used for archive integrity: every download is checked
// against the hash the registry (or the lockfile) promised.
std::string sha256Hex(const std::string& data);

// ---------------------------------------------------------------------------
// Filesystem / process helpers
// ---------------------------------------------------------------------------
std::string joinPath(const std::string& a, const std::string& b);
std::string parentDir(const std::string& path);
std::string baseName(const std::string& path);
std::string normalizeSlashes(std::string path);  // '\' -> '/'

bool isFile(const std::string& path);
bool isDir(const std::string& path);
bool readFile(const std::string& path, std::string& out);
bool ensureDir(const std::string& path, std::string& err);
bool writeFileMkdirs(const std::string& path, const std::string& data, std::string& err);
bool removeTree(const std::string& path, std::string& err);

// Every regular file under `root`, as paths relative to `root` with '/'
// separators, sorted. `skip` is consulted with each relative path (files and
// directories) and returning true prunes it.
std::vector<std::string> walkFiles(const std::string& root,
                                   const std::function<bool(const std::string&)>& skip);

std::string homeDir();
std::string hiveHome();     // $HIVE_HOME, else <home>/.hive
std::string globalLibDir(); // where `hive install -g` puts packages
std::string cacheDir();     // downloaded archives, keyed by hash
std::string currentDir();

// Walk up from `start` looking for a directory containing `marker`. Returns ""
// when nothing matches before the filesystem root.
std::string findUp(const std::string& start, const std::string& marker);

// ---------------------------------------------------------------------------
// Fetching
// ---------------------------------------------------------------------------
// GET `url` into `out`. Supports http(s) via curl or wget (whichever is on
// PATH), plus file:// URLs and bare local paths -- which makes a directory on
// disk usable as a registry, handy for testing and for private mirrors.
bool httpGet(const std::string& url, std::string& out, std::string& err);
bool looksLikeUrl(const std::string& s);

// ---------------------------------------------------------------------------
// Manifests and versions
// ---------------------------------------------------------------------------
using DepList = std::vector<std::pair<std::string, std::string>>;  // name -> constraint

struct Manifest {
    std::string name;
    std::string version;
    std::string description;
    std::string author;
    std::string license;
    std::string main;         // entry module, relative to the package root
    std::string homepage;
    std::string repository;
    std::vector<std::string> keywords;
    DepList dependencies;
    std::vector<std::string> files;    // optional include list for `hive pack`
    std::vector<std::string> exclude;  // optional prune list for `hive pack`
    Json raw;                          // as read, so unknown keys survive edits

    // `requireName` is false for project manifests, where a name and version
    // are optional (you're describing an app, not something publishable).
    static bool fromJson(const Json& j, bool requireName, Manifest& out, std::string& err);
    std::string entryFile() const;  // main, or "init.bee" when unset
};

bool validPackageName(const std::string& name);
bool validVersion(const std::string& v);
// -1 / 0 / 1, comparing release parts numerically and treating a prerelease
// suffix as older than the plain release (1.0.0-rc1 < 1.0.0).
int compareVersions(const std::string& a, const std::string& b);
// Supported constraints: "" / "*" / "latest", "1.2.3", "=1.2.3", "^1.2.3",
// "~1.2.3", ">=1.2.3", ">1.2.3", "<=1.2.3", "<1.2.3", and comma- or
// space-separated conjunctions of those.
bool versionSatisfies(const std::string& version, const std::string& constraint);
bool validConstraint(const std::string& constraint);

// ---------------------------------------------------------------------------
// The .hive archive format
// ---------------------------------------------------------------------------
// A .hive file is a tiny, dependency-free container:
//
//     HIVE1\n
//     <header-byte-length>\n
//     {"format":1,"manifest":{...},"files":[{"path":..,"size":..,"sha256":..}]}\n
//     <file bytes, concatenated in `files` order>
//
// No compression, so packing and unpacking need no third-party library and the
// header stays readable with `head`. Per-file hashes make a truncated or
// tampered archive a hard error rather than a silently broken install.
struct ArchiveEntry {
    std::string path;  // relative, '/'-separated
    std::string data;
};

struct Archive {
    Json manifestJson;
    Manifest manifest;
    std::vector<ArchiveEntry> entries;
};

bool writeArchive(const std::string& outPath, const Json& manifestJson,
                  const std::vector<ArchiveEntry>& entries, std::string& err);
bool readArchive(const std::string& blob, Archive& out, std::string& err);
// Reject paths that would escape the package directory (absolute, "..", drive
// letters). Exposed because `hive pack` refuses to store them in the first
// place.
bool safeRelativePath(const std::string& path);

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------
// The registry is a set of static files, so it can be hosted on GitHub Pages,
// S3, a plain nginx, or a local directory:
//
//     <base>/packages/<name>.json   per-package metadata (used by install)
//     <base>/index.json             flat catalogue (used by search)
struct VersionInfo {
    std::string version;
    std::string url;
    std::string sha256;
    std::string description;
    DepList dependencies;
    bool yanked = false;
};

struct PackageInfo {
    std::string name;
    std::string description;
    std::string homepage;
    std::vector<VersionInfo> versions;  // sorted ascending
};

class Registry {
public:
    Registry(std::string base, bool offline) : base_(std::move(base)), offline_(offline) {}

    const std::string& base() const { return base_; }
    bool fetchPackage(const std::string& name, PackageInfo& out, std::string& err);
    bool fetchIndex(Json& out, std::string& err);
    // Archive bytes for `v`, from the cache when the hash already matches.
    bool download(const VersionInfo& v, std::string& out, std::string& err);

private:
    std::string base_;
    bool offline_ = false;
    std::string resolveUrl(const std::string& urlOrPath) const;
};

std::string defaultRegistry();

// ---------------------------------------------------------------------------
// Installed-package records
// ---------------------------------------------------------------------------
// One JSON file per installed package, under `<root>/.hive/<name>.json`, so
// `hive list` and `hive uninstall` don't have to trust the package's own
// manifest and know exactly which files Hive created.
struct InstallRecord {
    std::string name;
    std::string version;
    std::string source;  // "registry" | "file"
    std::string url;
    std::string sha256;
    bool direct = false;  // asked for by name, vs pulled in as a dependency
    std::vector<std::string> files;
};

bool readRecord(const std::string& root, const std::string& name, InstallRecord& out);
bool writeRecord(const std::string& root, const InstallRecord& rec, std::string& err);
std::vector<std::string> listInstalled(const std::string& root);

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------
struct Options {
    bool global = false;
    bool offline = false;
    bool force = false;
    bool update = false;  // ignore lockfile pins and take the newest match
    bool save = true;
    bool quiet = false;
    std::string registry;   // overrides config / default
    std::string outFile;    // `hive pack -o`
    std::string dir;        // project root override
};

int cmdInit(const std::vector<std::string>& args, const Options& o);
int cmdInstall(const std::vector<std::string>& args, const Options& o);
int cmdUninstall(const std::vector<std::string>& args, const Options& o);
int cmdList(const std::vector<std::string>& args, const Options& o);
int cmdInfo(const std::vector<std::string>& args, const Options& o);
int cmdSearch(const std::vector<std::string>& args, const Options& o);
int cmdPack(const std::vector<std::string>& args, const Options& o);
int cmdCache(const std::vector<std::string>& args, const Options& o);

// Shared bits of command plumbing.
std::string configuredRegistry(const Options& o);
std::string projectRoot(const Options& o);  // nearest dir with hive.json, else cwd
std::string modulesDir(const Options& o);   // where packages get installed

}  // namespace hive
