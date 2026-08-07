// The `hive` subcommands.
#include "hive.hpp"

#include <algorithm>
#include <deque>
#include <iostream>
#include <map>
#include <set>
#include <sstream>

namespace hive {

// ---------------------------------------------------------------------------
// Shared plumbing
// ---------------------------------------------------------------------------
static const char* kManifestName = "hive.json";
static const char* kLockName = "hive.lock";
static const char* kModulesName = "hive_modules";

static void info(const Options& o, const std::string& msg) {
    if (!o.quiet) std::cout << msg << "\n";
}

static int fail(const std::string& msg) {
    std::cerr << "hive: " << msg << "\n";
    return 1;
}

std::string configuredRegistry(const Options& o) {
    if (!o.registry.empty()) return o.registry;
    // ~/.hive/config.json -- {"registry": "https://..."}
    std::string body;
    if (readFile(joinPath(hiveHome(), "config.json"), body)) {
        Json cfg;
        std::string err;
        if (Json::parse(body, cfg, err)) {
            std::string r = cfg.str("registry");
            if (!r.empty()) return r;
        }
    }
    return defaultRegistry();
}

std::string projectRoot(const Options& o) {
    if (!o.dir.empty()) return o.dir;
    // Commands work from anywhere inside a project, like git.
    std::string found = findUp(currentDir(), kManifestName);
    if (!found.empty()) return found;
    found = findUp(currentDir(), kModulesName);
    return found.empty() ? currentDir() : found;
}

std::string modulesDir(const Options& o) {
    return o.global ? globalLibDir() : joinPath(projectRoot(o), kModulesName);
}

// Read the project manifest, if there is one. Missing is not an error: you can
// install into a bare directory the way `pip install` works without a project.
static bool readProjectManifest(const std::string& root, Json& out, bool& exists, std::string& err) {
    exists = false;
    std::string body;
    if (!readFile(joinPath(root, kManifestName), body)) return true;
    exists = true;
    std::string jsonErr;
    if (!Json::parse(body, out, jsonErr) || !out.isObject()) {
        err = std::string(kManifestName) + " is not valid JSON: " + jsonErr;
        return false;
    }
    Manifest m;
    if (!Manifest::fromJson(out, /*requireName=*/false, m, err)) {
        err = std::string(kManifestName) + ": " + err;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Lockfile
// ---------------------------------------------------------------------------
struct LockEntry {
    std::string version;
    std::string url;
    std::string sha256;
    DepList dependencies;
};

static std::map<std::string, LockEntry> readLock(const std::string& root) {
    std::map<std::string, LockEntry> out;
    std::string body;
    if (!readFile(joinPath(root, kLockName), body)) return out;
    Json j;
    std::string err;
    if (!Json::parse(body, j, err)) return out;
    const Json* pkgs = j.find("packages");
    if (!pkgs || !pkgs->isObject()) return out;
    for (auto& m : pkgs->members) {
        if (!m.second.isObject()) continue;
        LockEntry e;
        e.version = m.second.str("version");
        e.url = m.second.str("url");
        e.sha256 = m.second.str("sha256");
        if (const Json* deps = m.second.find("dependencies"))
            for (auto& d : deps->members)
                if (d.second.isString()) e.dependencies.emplace_back(d.first, d.second.text);
        if (!e.version.empty()) out[m.first] = std::move(e);
    }
    return out;
}

static bool writeLock(const std::string& root, const std::map<std::string, LockEntry>& entries,
                      std::string& err) {
    Json j = Json::object();
    j.set("lockVersion", Json((double)1));
    Json pkgs = Json::object();
    for (auto& [name, e] : entries) {
        Json p = Json::object();
        p.set("version", Json(e.version));
        if (!e.url.empty()) p.set("url", Json(e.url));
        if (!e.sha256.empty()) p.set("sha256", Json(e.sha256));
        Json deps = Json::object();
        for (auto& d : e.dependencies) deps.set(d.first, Json(d.second));
        p.set("dependencies", std::move(deps));
        pkgs.set(name, std::move(p));
    }
    j.set("packages", std::move(pkgs));
    return writeFileMkdirs(joinPath(root, kLockName), j.dump(2) + "\n", err);
}

// ---------------------------------------------------------------------------
// Unpacking
// ---------------------------------------------------------------------------
// Install an already-verified archive into `<root>/<name>`, replacing whatever
// version was there. Refuses to clobber a directory Hive didn't create, so a
// hand-written module can't be silently deleted by a name collision.
static bool unpackInto(const std::string& root, const Archive& ar, const std::string& source,
                       const std::string& url, const std::string& sha, bool direct,
                       const Options& o, std::string& err) {
    const std::string name = ar.manifest.name;
    const std::string target = joinPath(root, name);

    InstallRecord old;
    const bool known = readRecord(root, name, old);
    if (isDir(target) && !known && !o.force) {
        err = "'" + target + "' already exists and was not installed by hive\n"
              "       move it aside, or pass --force to overwrite it";
        return false;
    }
    if (isDir(target) && !removeTree(target, err)) return false;

    InstallRecord rec;
    rec.name = name;
    rec.version = ar.manifest.version;
    rec.source = source;
    rec.url = url;
    rec.sha256 = sha;
    // A package explicitly asked for stays "direct" even when a later install
    // pulls it in as someone else's dependency.
    rec.direct = direct || (known && old.direct);

    for (auto& e : ar.entries) {
        const std::string dest = joinPath(target, e.path);
        if (!writeFileMkdirs(dest, e.data, err)) {
            std::string ignored;
            removeTree(target, ignored);
            return false;
        }
        rec.files.push_back(e.path);
    }

    // The archive's manifest is the source of truth for the interpreter's entry
    // lookup, so make sure it lands on disk even if it wasn't packed as a file.
    bool hasManifest = std::find(rec.files.begin(), rec.files.end(), kManifestName) != rec.files.end();
    if (!hasManifest) {
        if (!writeFileMkdirs(joinPath(target, kManifestName), ar.manifestJson.dump(2) + "\n", err))
            return false;
        rec.files.push_back(kManifestName);
    }

    return writeRecord(root, rec, err);
}

// ---------------------------------------------------------------------------
// Dependency resolution
// ---------------------------------------------------------------------------
struct Request {
    std::string name;
    std::string constraint;
    std::string requestedBy;  // "" for a top-level request
    bool direct = false;
};

struct Resolution {
    std::map<std::string, VersionInfo> chosen;
    std::set<std::string> direct;
};

static std::string describeConstraints(const std::vector<std::pair<std::string, std::string>>& cs) {
    std::string out;
    for (auto& [constraint, who] : cs) {
        out += "\n       " + (constraint.empty() ? "*" : constraint);
        out += who.empty() ? " (requested)" : " (from " + who + ")";
    }
    return out;
}

// Highest non-yanked version satisfying every accumulated constraint, favouring
// the locked version when it still qualifies so installs stay reproducible.
static const VersionInfo* pickVersion(const PackageInfo& pkg,
                                      const std::vector<std::pair<std::string, std::string>>& constraints,
                                      const std::string& pinned) {
    const VersionInfo* best = nullptr;
    for (auto it = pkg.versions.rbegin(); it != pkg.versions.rend(); ++it) {
        if (it->yanked) continue;
        bool ok = true;
        for (auto& [c, who] : constraints) {
            (void)who;
            if (!versionSatisfies(it->version, c)) { ok = false; break; }
        }
        if (!ok) continue;
        if (!pinned.empty() && it->version == pinned) return &*it;
        if (!best) best = &*it;
    }
    return best;
}

static bool resolveAll(Registry& reg, const std::vector<Request>& roots,
                       const std::map<std::string, LockEntry>& lock, bool ignoreLock,
                       const std::map<std::string, VersionInfo>& preResolved,
                       Resolution& out, std::string& err) {
    std::map<std::string, PackageInfo> metadata;
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> constraints;
    std::deque<Request> work(roots.begin(), roots.end());

    // Locally-installed archives are already pinned; their dependencies still
    // need resolving, which the caller queues as roots.
    out.chosen = preResolved;

    int steps = 0;
    while (!work.empty()) {
        if (++steps > 10000) {
            err = "dependency resolution did not settle -- is there a cycle of conflicting constraints?";
            return false;
        }
        Request req = work.front();
        work.pop_front();

        if (!validPackageName(req.name)) {
            err = "'" + req.name + "' is not a valid package name";
            return false;
        }
        if (!validConstraint(req.constraint)) {
            err = "invalid version constraint '" + req.constraint + "' for '" + req.name + "'";
            return false;
        }
        if (req.direct) out.direct.insert(req.name);
        constraints[req.name].emplace_back(req.constraint, req.requestedBy);

        auto have = out.chosen.find(req.name);
        if (have != out.chosen.end() && versionSatisfies(have->second.version, req.constraint))
            continue;

        // A locally-installed archive is fixed: we can't swap it for another
        // version to satisfy a constraint.
        if (have != out.chosen.end() && preResolved.count(req.name)) {
            err = "'" + req.name + "@" + have->second.version + "' was installed from a file but "
                  "does not satisfy:" + describeConstraints(constraints[req.name]);
            return false;
        }

        // A lockfile entry that still satisfies every constraint is used as-is.
        // That's what makes an install reproducible -- and what lets it work
        // offline, since nothing has to be looked up.
        auto pin = lock.find(req.name);
        if (!ignoreLock && pin != lock.end() && !pin->second.url.empty()) {
            bool fits = true;
            for (auto& [c, who] : constraints[req.name]) {
                (void)who;
                if (!versionSatisfies(pin->second.version, c)) { fits = false; break; }
            }
            if (fits) {
                VersionInfo vi;
                vi.version = pin->second.version;
                vi.url = pin->second.url;
                vi.sha256 = pin->second.sha256;
                vi.dependencies = pin->second.dependencies;
                if (have != out.chosen.end() && have->second.version == vi.version) continue;
                out.chosen[req.name] = vi;
                for (auto& [depName, depConstraint] : vi.dependencies)
                    work.push_back({depName, depConstraint, req.name + "@" + vi.version, false});
                continue;
            }
        }

        auto meta = metadata.find(req.name);
        if (meta == metadata.end()) {
            PackageInfo pkg;
            if (!reg.fetchPackage(req.name, pkg, err)) return false;
            meta = metadata.emplace(req.name, std::move(pkg)).first;
        }

        std::string pinned;
        if (pin != lock.end()) pinned = pin->second.version;

        const VersionInfo* pick = pickVersion(meta->second, constraints[req.name], pinned);
        if (!pick) {
            err = "no version of '" + req.name + "' satisfies:" + describeConstraints(constraints[req.name]);
            return false;
        }
        if (have != out.chosen.end() && have->second.version == pick->version) continue;

        out.chosen[req.name] = *pick;
        for (auto& [depName, depConstraint] : pick->dependencies)
            work.push_back({depName, depConstraint, req.name + "@" + pick->version, false});
    }

    // Re-resolution can strand a dependency that only the discarded version
    // needed; drop anything no longer reachable from the direct requests.
    std::set<std::string> reachable;
    std::deque<std::string> queue(out.direct.begin(), out.direct.end());
    while (!queue.empty()) {
        std::string name = queue.front();
        queue.pop_front();
        if (!reachable.insert(name).second) continue;
        auto it = out.chosen.find(name);
        if (it == out.chosen.end()) continue;
        for (auto& [depName, depConstraint] : it->second.dependencies) {
            (void)depConstraint;
            queue.push_back(depName);
        }
    }
    for (auto it = out.chosen.begin(); it != out.chosen.end();)
        it = reachable.count(it->first) ? std::next(it) : out.chosen.erase(it);

    return true;
}

// ---------------------------------------------------------------------------
// hive init
// ---------------------------------------------------------------------------
int cmdInit(const std::vector<std::string>& args, const Options& o) {
    if (args.size() > 1) return fail("usage: hive init [directory]");
    const std::string dir = args.empty() ? currentDir() : args[0];
    const std::string path = joinPath(dir, kManifestName);
    if (isFile(path) && !o.force)
        return fail(std::string(kManifestName) + " already exists (use --force to overwrite)");

    std::string name = baseName(dir);
    // Make the directory name usable as a package name where we can.
    std::string cleaned;
    for (char c : name) {
        char lower = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        if ((lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9') || lower == '-' || lower == '_')
            cleaned += lower;
        else
            cleaned += '-';
    }
    if (!validPackageName(cleaned)) cleaned = "my-package";

    Json j = Json::object();
    j.set("name", Json(cleaned));
    j.set("version", Json("0.1.0"));
    j.set("description", Json(""));
    j.set("license", Json("MIT"));
    j.set("main", Json("init.bee"));
    j.set("dependencies", Json::object());

    std::string err;
    if (!writeFileMkdirs(path, j.dump(2) + "\n", err)) return fail(err);
    info(o, "wrote " + path);

    const std::string entry = joinPath(dir, "init.bee");
    if (!isFile(entry)) {
        const std::string starter =
            "# " + cleaned + " -- entry module.\n"
            "# Anything defined here is what `import " + cleaned + "` exposes;\n"
            "# names starting with '_' stay private to the package.\n\n"
            "fn hello() {\n"
            "    return \"hello from " + cleaned + "\"\n"
            "}\n";
        if (!writeFileMkdirs(entry, starter, err)) return fail(err);
        info(o, "wrote " + entry);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// hive install
// ---------------------------------------------------------------------------
// Split "name@1.2.3" / "name@^1.2" into its parts.
static Request parseSpec(const std::string& spec) {
    Request r;
    size_t at = spec.find('@');
    if (at == std::string::npos) {
        r.name = spec;
    } else {
        r.name = spec.substr(0, at);
        r.constraint = spec.substr(at + 1);
    }
    r.direct = true;
    return r;
}

static bool isArchiveSpec(const std::string& spec) {
    if (spec.size() > 4 && spec.compare(spec.size() - 4, 4, ".pkg") == 0) return true;
    // A path that exists on disk is a file install even without the extension.
    return isFile(spec);
}

// What we'd write into hive.json for a newly requested package.
static std::string saveConstraint(const Request& req, const std::string& installedVersion) {
    if (!req.constraint.empty()) return req.constraint;
    if (installedVersion.empty()) return "*";
    return "^" + installedVersion;
}

int cmdInstall(const std::vector<std::string>& args, const Options& o) {
    const std::string root = modulesDir(o);
    const std::string project = projectRoot(o);

    Json manifestJson = Json::object();
    bool hasManifest = false;
    std::string err;
    if (!o.global && !readProjectManifest(project, manifestJson, hasManifest, err)) return fail(err);

    std::vector<Request> roots;
    std::vector<std::string> archiveSpecs;
    for (auto& a : args) {
        if (isArchiveSpec(a)) archiveSpecs.push_back(a);
        else roots.push_back(parseSpec(a));
    }

    // No arguments: install what the project manifest asks for.
    if (args.empty()) {
        if (o.global) return fail("hive install -g needs at least one package name");
        if (!hasManifest)
            return fail("no " + std::string(kManifestName) + " here -- run 'hive init', or name a package to install");
        Manifest pm;
        if (!Manifest::fromJson(manifestJson, false, pm, err)) return fail(err);
        if (pm.dependencies.empty()) {
            info(o, "nothing to install -- " + std::string(kManifestName) + " has no dependencies");
            return 0;
        }
        for (auto& [name, constraint] : pm.dependencies)
            roots.push_back({name, constraint, "", true});
    }

    // Note: `root` is created lazily by the first unpack, so a failed install
    // doesn't leave an empty hive_modules/ behind.

    // ---- local .pkg packages -------------------------------------------
    std::map<std::string, VersionInfo> fromFiles;
    std::map<std::string, Archive> fileArchives;
    std::vector<Request> fileDeps;
    for (auto& spec : archiveSpecs) {
        std::string blob;
        if (!readFile(spec, blob)) return fail("cannot read '" + spec + "'");
        Archive ar;
        if (!readArchive(blob, ar, err)) return fail(std::string(spec) + ": " + err);

        const std::string name = ar.manifest.name;
        VersionInfo vi;
        vi.version = ar.manifest.version;
        vi.sha256 = sha256Hex(blob);
        vi.dependencies = ar.manifest.dependencies;
        fromFiles[name] = vi;
        fileArchives[name] = std::move(ar);

        // The archive itself is pinned, but whatever it needs still comes from
        // the registry.
        for (auto& [depName, depConstraint] : vi.dependencies)
            fileDeps.push_back({depName, depConstraint, name + "@" + vi.version, false});
    }

    // ---- resolve ---------------------------------------------------------
    Registry reg(configuredRegistry(o), o.offline);
    auto lock = o.global ? std::map<std::string, LockEntry>() : readLock(project);

    std::vector<Request> allRoots = roots;
    for (auto& [name, vi] : fromFiles) {
        (void)vi;
        allRoots.push_back({name, "", "", true});  // pinned; recorded as direct
    }
    allRoots.insert(allRoots.end(), fileDeps.begin(), fileDeps.end());

    Resolution res;
    if (!resolveAll(reg, allRoots, lock, o.update, fromFiles, res, err)) return fail(err);

    if (res.chosen.empty()) {
        info(o, "nothing to install");
        return 0;
    }

    // Installing a package into its own source tree produces
    // greet/hive_modules/greet -- which shadows nothing, helps nothing, and is
    // usually a mistyped directory.
    if (!o.global && hasManifest && !o.force) {
        const std::string selfName = manifestJson.str("name");
        if (!selfName.empty() && res.chosen.count(selfName))
            return fail("'" + selfName + "' is this package (" + joinPath(project, kManifestName) +
                        ")\n       installing it into itself does nothing -- did you mean to run "
                        "this somewhere else?\n       pass --force if you really want to");
    }

    // ---- fetch and unpack ------------------------------------------------
    std::vector<std::string> installed, unchanged;
    // Packages that declared a "build" command, in the order they were
    // installed. Collected here and run once below, so a package is never built
    // before the dependencies it might build against are on disk.
    std::vector<std::pair<std::string, Manifest>> needSetup;
    for (auto& [name, vi] : res.chosen) {
        const bool direct = res.direct.count(name) > 0;
        const bool fromFile = fromFiles.count(name) > 0;

        InstallRecord existing;
        if (!o.force && readRecord(root, name, existing) && existing.version == vi.version &&
            isDir(joinPath(root, name)) &&
            (fromFile ? existing.sha256 == vi.sha256 : true)) {
            unchanged.push_back(name + "@" + vi.version);
            // Still refresh the record so a package promoted to direct sticks.
            if (direct && !existing.direct) {
                existing.direct = true;
                writeRecord(root, existing, err);
            }
            continue;
        }

        if (fromFile) {
            if (!unpackInto(root, fileArchives[name], "file", "", vi.sha256, direct, o, err))
                return fail(err);
            const Manifest& fm = fileArchives[name].manifest;
            if (!fm.build.empty() || !fm.binaries.empty()) needSetup.push_back({name, fm});
        } else {
            info(o, "fetching " + name + "@" + vi.version);
            std::string blob;
            if (!reg.download(vi, blob, err)) return fail(err);
            Archive ar;
            if (!readArchive(blob, ar, err)) return fail(name + "@" + vi.version + ": " + err);
            if (ar.manifest.name != name)
                return fail("registry served '" + ar.manifest.name + "' for '" + name + "'");
            if (ar.manifest.version != vi.version)
                return fail("registry served " + name + "@" + ar.manifest.version + ", expected " + vi.version);
            if (!unpackInto(root, ar, "registry", vi.url, vi.sha256, direct, o, err)) return fail(err);
            if (!ar.manifest.build.empty() || !ar.manifest.binaries.empty())
                needSetup.push_back({name, ar.manifest});
        }
        installed.push_back(name + "@" + vi.version);
    }

    // ---- native modules --------------------------------------------------
    // Prefer a prebuilt binary for this platform: installing should be a
    // download, not a compile. Building is the fallback for a platform the
    // package doesn't ship, which is the only case where a user needs a
    // toolchain at all.
    std::vector<std::string> buildFailed;
    const std::string platform = hostPlatform();
    for (auto& [name, m] : needSetup) {
        const std::string dir = joinPath(root, name);

        std::string prebuilt;
        for (auto& [key, path] : m.binaries)
            if (key == platform) { prebuilt = path; break; }

        if (!prebuilt.empty() && isFile(joinPath(dir, prebuilt))) {
            // Move it to the package root under its own name, where `import`
            // looks for it.
            std::string data;
            const std::string dest = joinPath(dir, baseName(prebuilt));
            if (readFile(joinPath(dir, prebuilt), data) && writeFileMkdirs(dest, data, err)) {
                info(o, "  using prebuilt " + name + " for " + platform);
                continue;
            }
            std::cerr << "hive: could not install the prebuilt binary for " << name
                      << " (" << err << "); building instead\n";
        } else if (!prebuilt.empty()) {
            std::cerr << "hive: " << name << " lists a prebuilt for " << platform
                      << " but '" << prebuilt << "' is missing from the package\n";
        }

        if (m.build.empty()) {
            if (!m.binaries.empty()) {
                buildFailed.push_back(name);
                std::string have;
                for (auto& [key, path] : m.binaries) have += (have.empty() ? "" : ", ") + key;
                std::cerr << "hive: " << name << " ships no binary for " << platform
                          << " and declares no \"build\" command\n"
                          << "      it has: " << have << "\n";
            }
            continue;
        }
        if (!o.build) {
            info(o, "  skipping build for " + name + " (--no-build)");
            continue;
        }

        if (!o.quiet) std::cout << "  building " << name << " (" << m.build << ")\n";
        std::string output;
        if (runInDir(dir, m.build, output) != 0) {
            buildFailed.push_back(name);
            std::cerr << "hive: build failed for " << name << "\n";
            std::istringstream lines(output);
            std::string line;
            while (std::getline(lines, line)) std::cerr << "       " << line << "\n";
        }
    }

    // ---- record what we did ---------------------------------------------
    if (!o.global) {
        // Merge into the lockfile, keeping entries for packages we didn't touch.
        auto merged = lock;
        for (auto& [name, vi] : res.chosen) {
            LockEntry e;
            e.version = vi.version;
            e.url = vi.url;
            e.sha256 = vi.sha256;
            e.dependencies = vi.dependencies;
            merged[name] = std::move(e);
        }
        for (auto it = merged.begin(); it != merged.end();)
            it = isDir(joinPath(root, it->first)) ? std::next(it) : merged.erase(it);
        // Don't litter a directory with an empty lockfile: with nothing locked
        // there is nothing to reproduce, and a stray hive.lock makes that
        // directory look like a project root to later commands.
        if ((!merged.empty() || isFile(joinPath(project, kLockName))) &&
            !writeLock(project, merged, err))
            return fail(err);
    }

    if (o.save && !o.global && hasManifest && !args.empty()) {
        Json* deps = manifestJson.find("dependencies");
        if (!deps) deps = &manifestJson.set("dependencies", Json::object());
        // Installing a package inside its own source tree must not record it as
        // depending on itself -- easy to do by accident, and nonsense to resolve.
        const std::string selfName = manifestJson.str("name");
        bool changed = false;
        for (auto& req : roots) {
            if (!selfName.empty() && req.name == selfName) {
                std::cerr << "hive: warning: not saving '" << req.name
                          << "' as a dependency of itself\n";
                continue;
            }
            auto it = res.chosen.find(req.name);
            std::string constraint = saveConstraint(req, it == res.chosen.end() ? "" : it->second.version);
            const Json* have = deps->find(req.name);
            if (have && have->isString() && have->text == constraint) continue;
            deps->set(req.name, Json(constraint));
            changed = true;
        }
        for (auto& [name, vi] : fromFiles) {
            if (!selfName.empty() && name == selfName) {
                std::cerr << "hive: warning: not saving '" << name
                          << "' as a dependency of itself\n";
                continue;
            }
            const Json* have = deps->find(name);
            std::string constraint = "^" + vi.version;
            if (have && have->isString()) continue;  // don't overwrite a real constraint
            deps->set(name, Json(constraint));
            changed = true;
        }
        if (changed && !writeFileMkdirs(joinPath(project, kManifestName), manifestJson.dump(2) + "\n", err))
            return fail(err);
    } else if (o.save && !o.global && !hasManifest && !args.empty()) {
        info(o, "note: no " + std::string(kManifestName) + " here, so nothing was saved as a dependency\n"
                "      run 'hive init' to create one");
    }

    // ---- summary ---------------------------------------------------------
    if (!o.quiet) {
        for (auto& s : installed) std::cout << "  + " << s << "\n";
        for (auto& s : unchanged) std::cout << "  = " << s << " (up to date)\n";
        std::cout << "installed " << installed.size() << " package"
                  << (installed.size() == 1 ? "" : "s") << " into " << root << "\n";
    }
    // The files are left in place: a build usually fails for a fixable reason
    // (a missing compiler or -dev package), and re-running the command by hand
    // in the package directory is then the whole fix.
    if (!buildFailed.empty()) {
        std::string names;
        for (size_t i = 0; i < buildFailed.size(); ++i)
            names += (i ? ", " : "") + buildFailed[i];
        std::cerr << "hive: " << buildFailed.size() << " package"
                  << (buildFailed.size() == 1 ? "" : "s") << " installed but failed to build: "
                  << names << "\n"
                  << "      the files are in place -- fix the cause and re-run the build in "
                  << joinPath(root, buildFailed[0]) << "\n";
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// hive uninstall
// ---------------------------------------------------------------------------
int cmdUninstall(const std::vector<std::string>& args, const Options& o) {
    if (args.empty()) return fail("usage: hive uninstall <package>...");
    const std::string root = modulesDir(o);
    const std::string project = projectRoot(o);

    std::string err;
    Json manifestJson = Json::object();
    bool hasManifest = false;
    if (!o.global && !readProjectManifest(project, manifestJson, hasManifest, err)) return fail(err);
    auto lock = o.global ? std::map<std::string, LockEntry>() : readLock(project);

    int removed = 0;
    for (auto& name : args) {
        InstallRecord rec;
        const bool known = readRecord(root, name, rec);
        const std::string target = joinPath(root, name);
        if (!known && !isDir(target)) {
            std::cerr << "hive: '" << name << "' is not installed in " << root << "\n";
            continue;
        }
        if (!known && !o.force)
            return fail("'" + target + "' was not installed by hive -- pass --force to remove it anyway");

        // Warn about anything that still needs it, but honour the request.
        for (auto& other : listInstalled(root)) {
            if (other == name) continue;
            auto it = lock.find(other);
            if (it == lock.end()) continue;
            for (auto& [depName, depConstraint] : it->second.dependencies) {
                (void)depConstraint;
                if (depName == name)
                    std::cerr << "hive: warning: '" << other << "' depends on '" << name << "'\n";
            }
        }

        if (!removeTree(target, err)) return fail(err);
        std::string ignored;
        removeTree(joinPath(joinPath(root, ".hive"), name + ".json"), ignored);
        lock.erase(name);
        info(o, "  - " + name + (rec.version.empty() ? "" : "@" + rec.version));
        removed++;

        if (o.save && !o.global && hasManifest) {
            if (Json* deps = manifestJson.find("dependencies")) deps->erase(name);
        }
    }

    if (removed && !o.global) {
        if (!writeLock(project, lock, err)) return fail(err);
        if (o.save && hasManifest &&
            !writeFileMkdirs(joinPath(project, kManifestName), manifestJson.dump(2) + "\n", err))
            return fail(err);
    }
    info(o, "removed " + std::to_string(removed) + " package" + (removed == 1 ? "" : "s"));
    return removed ? 0 : 1;
}

// ---------------------------------------------------------------------------
// hive list
// ---------------------------------------------------------------------------
int cmdList(const std::vector<std::string>& args, const Options& o) {
    if (!args.empty()) return fail("usage: hive list [-g]");
    const std::string root = modulesDir(o);
    auto names = listInstalled(root);
    if (names.empty()) {
        info(o, "no packages installed in " + root);
        return 0;
    }

    size_t width = 0;
    for (auto& n : names) width = std::max(width, n.size());

    std::cout << root << "\n";
    for (auto& name : names) {
        InstallRecord rec;
        readRecord(root, name, rec);

        // Prefer the installed package's own description.
        std::string description;
        std::string body;
        if (readFile(joinPath(joinPath(root, name), kManifestName), body)) {
            Json j;
            std::string err;
            if (Json::parse(body, j, err)) description = j.str("description");
        }

        std::cout << "  " << name << std::string(width - name.size(), ' ')
                  << "  " << (rec.version.empty() ? "?" : rec.version);
        if (!rec.direct) std::cout << "  (dependency)";
        if (!description.empty()) std::cout << "  " << description;
        std::cout << "\n";
    }
    return 0;
}

// ---------------------------------------------------------------------------
// hive info
// ---------------------------------------------------------------------------
int cmdInfo(const std::vector<std::string>& args, const Options& o) {
    if (args.size() != 1) return fail("usage: hive info <package>");
    Registry reg(configuredRegistry(o), o.offline);
    PackageInfo pkg;
    std::string err;
    if (!reg.fetchPackage(args[0], pkg, err)) return fail(err);

    std::cout << pkg.name << "\n";
    if (!pkg.description.empty()) std::cout << "  " << pkg.description << "\n";
    if (!pkg.homepage.empty()) std::cout << "  homepage: " << pkg.homepage << "\n";

    const VersionInfo* latest = nullptr;
    for (auto it = pkg.versions.rbegin(); it != pkg.versions.rend(); ++it)
        if (!it->yanked) { latest = &*it; break; }
    if (latest) {
        std::cout << "  latest:   " << latest->version << "\n";
        if (!latest->dependencies.empty()) {
            std::cout << "  requires:";
            for (auto& [n, c] : latest->dependencies) std::cout << " " << n << "@" << (c.empty() ? "*" : c);
            std::cout << "\n";
        }
    }

    std::cout << "  versions: ";
    for (size_t i = 0; i < pkg.versions.size(); ++i) {
        if (i) std::cout << ", ";
        std::cout << pkg.versions[i].version;
        if (pkg.versions[i].yanked) std::cout << " (yanked)";
    }
    std::cout << "\n";
    std::cout << "  install:  hive install " << pkg.name << "\n";
    return 0;
}

// ---------------------------------------------------------------------------
// hive search
// ---------------------------------------------------------------------------
static std::string toLower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

int cmdSearch(const std::vector<std::string>& args, const Options& o) {
    if (args.size() != 1) return fail("usage: hive search <query>");
    Registry reg(configuredRegistry(o), o.offline);
    Json index;
    std::string err;
    if (!reg.fetchIndex(index, err)) return fail(err);

    const Json* packages = index.find("packages");
    if (!packages || !packages->isArray()) return fail("registry index has no 'packages' array");

    const std::string q = toLower(args[0]);
    int hits = 0;
    for (auto& p : packages->items) {
        if (!p.isObject()) continue;
        const std::string name = p.str("name");
        const std::string description = p.str("description");
        if (toLower(name).find(q) == std::string::npos &&
            toLower(description).find(q) == std::string::npos)
            continue;
        std::cout << name;
        const std::string version = p.str("version");
        if (!version.empty()) std::cout << " " << version;
        if (!description.empty()) std::cout << "\n    " << description;
        std::cout << "\n";
        hits++;
    }
    if (!hits) info(o, "no packages matched '" + args[0] + "'");
    return 0;
}

// ---------------------------------------------------------------------------
// hive pack
// ---------------------------------------------------------------------------
static bool matchesPrefix(const std::string& rel, const std::string& pattern) {
    // A pattern matches a file exactly, or any file beneath it when it names a
    // directory.
    if (rel == pattern) return true;
    return rel.size() > pattern.size() && rel.compare(0, pattern.size(), pattern) == 0 &&
           rel[pattern.size()] == '/';
}

int cmdPack(const std::vector<std::string>& args, const Options& o) {
    if (args.size() > 1) return fail("usage: hive pack [directory] [-o out.pkg]");
    const std::string dir = args.empty() ? currentDir() : args[0];

    std::string body;
    if (!readFile(joinPath(dir, kManifestName), body))
        return fail("no " + std::string(kManifestName) + " in '" + dir + "' -- run 'hive init' first");

    Json manifestJson;
    std::string err;
    if (!Json::parse(body, manifestJson, err)) return fail(std::string(kManifestName) + ": " + err);
    Manifest m;
    if (!Manifest::fromJson(manifestJson, /*requireName=*/true, m, err))
        return fail(std::string(kManifestName) + ": " + err);

    // Build artefacts, VCS metadata and installed dependencies never belong in
    // a package.
    auto skip = [&](const std::string& rel) {
        const std::string base = baseName(rel);
        if (base == ".git" || base == ".hg" || base == ".svn") return true;
        if (base == kModulesName || base == ".hive") return true;
        if (base == ".DS_Store" || base == "Thumbs.db") return true;
        if (rel.size() > 4 && rel.compare(rel.size() - 4, 4, ".pkg") == 0) return true;
        if (rel.size() > 9 && rel.compare(rel.size() - 9, 9, ".hive-tmp") == 0) return true;
        for (auto& ex : m.exclude)
            if (matchesPrefix(rel, ex)) return true;
        return false;
    };

    std::vector<ArchiveEntry> entries;
    for (auto& rel : walkFiles(dir, skip)) {
        // An explicit "files" list is a whitelist; hive.json always ships.
        if (!m.files.empty() && rel != kManifestName) {
            bool wanted = false;
            for (auto& f : m.files)
                if (matchesPrefix(rel, f)) { wanted = true; break; }
            if (!wanted) continue;
        }
        if (!safeRelativePath(rel)) {
            std::cerr << "hive: skipping '" << rel << "' (unsupported path)\n";
            continue;
        }
        ArchiveEntry e;
        e.path = rel;
        if (!readFile(joinPath(dir, rel), e.data)) return fail("cannot read '" + rel + "'");
        entries.push_back(std::move(e));
    }

    // Without an entry module, `import <name>` would fail after install.
    const std::string entry = m.entryFile();
    bool haveEntry = false;
    for (auto& e : entries)
        if (e.path == entry) { haveEntry = true; break; }
    if (!haveEntry)
        return fail("entry module '" + entry + "' is missing from the package"
                    " (set \"main\" in " + std::string(kManifestName) + ", or add the file)");

    const std::string out = o.outFile.empty()
        ? joinPath(dir, m.name + "-" + m.version + ".pkg")
        : o.outFile;
    if (!writeArchive(out, manifestJson, entries, err)) return fail(err);

    if (!o.quiet) {
        size_t bytes = 0;
        for (auto& e : entries) bytes += e.data.size();
        std::cout << "packed " << m.name << "@" << m.version << " -- " << entries.size()
                  << " file" << (entries.size() == 1 ? "" : "s") << ", " << bytes << " bytes\n";
        std::string blob;
        readFile(out, blob);
        std::cout << out << "\n";
        std::cout << "sha256 " << sha256Hex(blob) << "\n";
    }
    return 0;
}

// ---------------------------------------------------------------------------
// hive cache
// ---------------------------------------------------------------------------
int cmdCache(const std::vector<std::string>& args, const Options& o) {
    const std::string sub = args.empty() ? "dir" : args[0];
    if (sub == "dir") {
        std::cout << cacheDir() << "\n";
        return 0;
    }
    if (sub == "clean") {
        std::string err;
        if (isDir(cacheDir()) && !removeTree(cacheDir(), err)) return fail(err);
        info(o, "cache cleared");
        return 0;
    }
    return fail("usage: hive cache [dir|clean]");
}

}  // namespace hive
