// Talking to a registry, and remembering what we installed.
#include "hive.hpp"

#include <algorithm>
#include <cstdlib>

namespace hive {

std::string defaultRegistry() {
    // A registry is just static files, so this can point at anything that
    // serves them -- including a directory on disk.
    if (const char* r = std::getenv("HIVE_REGISTRY")) if (*r) return r;
    return "https://packages.beelang.dev";
}

// ------------------------------------------------------------------ registry
std::string Registry::resolveUrl(const std::string& urlOrPath) const {
    if (looksLikeUrl(urlOrPath)) return urlOrPath;
    // Registry metadata may give a URL relative to the registry root, so a
    // mirror keeps working when it's copied to a different host.
    std::string rel = urlOrPath;
    while (!rel.empty() && rel[0] == '/') rel.erase(0, 1);
    return joinPath(base_, rel);
}

static bool depsFromJson(const Json& j, DepList& out, std::string& err) {
    const Json* v = j.find("dependencies");
    if (!v || v->isNull()) return true;
    if (!v->isObject()) {
        err = "'dependencies' must be an object";
        return false;
    }
    for (auto& m : v->members) {
        if (!m.second.isString()) {
            err = "dependency '" + m.first + "' must map to a string";
            return false;
        }
        out.emplace_back(m.first, m.second.text);
    }
    return true;
}

bool Registry::fetchPackage(const std::string& name, PackageInfo& out, std::string& err) {
    if (!validPackageName(name)) {
        err = "'" + name + "' is not a valid package name";
        return false;
    }
    if (offline_) {
        err = "offline: cannot look up '" + name + "' in the registry";
        return false;
    }

    const std::string url = joinPath(base_, "packages/" + name + ".json");
    std::string body;
    if (!httpGet(url, body, err)) {
        // Could be a missing package or an unreachable registry; the underlying
        // error is appended so the user can tell which.
        err = "cannot find '" + name + "' in the registry " + base_ + "\n       " + err;
        return false;
    }

    Json j;
    std::string jsonErr;
    if (!Json::parse(body, j, jsonErr) || !j.isObject()) {
        err = "bad registry metadata for '" + name + "': " + jsonErr;
        return false;
    }

    out = PackageInfo();
    out.name = j.str("name", name);
    out.description = j.str("description");
    out.homepage = j.str("homepage");

    const Json* versions = j.find("versions");
    if (!versions || !versions->isObject() || versions->members.empty()) {
        err = "'" + name + "' has no published versions";
        return false;
    }
    for (auto& m : versions->members) {
        if (!m.second.isObject()) continue;
        VersionInfo v;
        v.version = m.first;
        if (!validVersion(v.version)) continue;  // ignore entries we can't order
        v.url = m.second.str("url");
        v.sha256 = m.second.str("sha256");
        v.description = m.second.str("description", out.description);
        v.yanked = m.second.flag("yanked", false);
        if (!depsFromJson(m.second, v.dependencies, err)) {
            err = "'" + name + "@" + v.version + "': " + err;
            return false;
        }
        if (v.url.empty()) continue;  // nothing to download
        v.url = resolveUrl(v.url);
        out.versions.push_back(std::move(v));
    }
    if (out.versions.empty()) {
        err = "'" + name + "' has no usable versions in the registry";
        return false;
    }
    std::sort(out.versions.begin(), out.versions.end(),
              [](const VersionInfo& a, const VersionInfo& b) {
                  return compareVersions(a.version, b.version) < 0;
              });
    return true;
}

bool Registry::fetchIndex(Json& out, std::string& err) {
    if (offline_) {
        err = "offline: cannot fetch the registry index";
        return false;
    }
    std::string body;
    if (!httpGet(joinPath(base_, "index.json"), body, err)) return false;
    std::string jsonErr;
    if (!Json::parse(body, out, jsonErr)) {
        err = "bad registry index: " + jsonErr;
        return false;
    }
    return true;
}

bool Registry::download(const VersionInfo& v, std::string& out, std::string& err) {
    // The cache is keyed by content hash, so a hit is self-verifying and two
    // packages that ship the same bytes share one entry.
    std::string cached;
    if (!v.sha256.empty()) {
        cached = joinPath(cacheDir(), v.sha256 + ".hive");
        if (readFile(cached, out) && sha256Hex(out) == v.sha256) return true;
    }

    if (offline_) {
        err = "offline: '" + v.version + "' is not in the cache";
        return false;
    }
    if (!httpGet(v.url, out, err)) return false;

    if (!v.sha256.empty()) {
        const std::string got = sha256Hex(out);
        if (got != v.sha256) {
            err = "checksum mismatch for " + v.url + "\n  expected " + v.sha256 + "\n  got      " + got;
            return false;
        }
        std::string ignored;
        writeFileMkdirs(cached, out, ignored);  // a cache miss is not fatal
    }
    return true;
}

// ------------------------------------------------------------------ records
static std::string recordPath(const std::string& root, const std::string& name) {
    return joinPath(joinPath(root, ".hive"), name + ".json");
}

bool readRecord(const std::string& root, const std::string& name, InstallRecord& out) {
    std::string body;
    if (!readFile(recordPath(root, name), body)) return false;
    Json j;
    std::string err;
    if (!Json::parse(body, j, err) || !j.isObject()) return false;

    out = InstallRecord();
    out.name = j.str("name", name);
    out.version = j.str("version");
    out.source = j.str("source", "registry");
    out.url = j.str("url");
    out.sha256 = j.str("sha256");
    out.direct = j.flag("direct", false);
    if (const Json* files = j.find("files"))
        for (auto& f : files->items)
            if (f.isString()) out.files.push_back(f.text);
    return true;
}

bool writeRecord(const std::string& root, const InstallRecord& rec, std::string& err) {
    Json j = Json::object();
    j.set("name", Json(rec.name));
    j.set("version", Json(rec.version));
    j.set("source", Json(rec.source));
    if (!rec.url.empty()) j.set("url", Json(rec.url));
    if (!rec.sha256.empty()) j.set("sha256", Json(rec.sha256));
    j.set("direct", Json(rec.direct));
    Json files = Json::array();
    for (auto& f : rec.files) files.push(Json(f));
    j.set("files", std::move(files));
    return writeFileMkdirs(recordPath(root, rec.name), j.dump(2) + "\n", err);
}

std::vector<std::string> listInstalled(const std::string& root) {
    std::vector<std::string> names;
    const std::string metaDir = joinPath(root, ".hive");
    for (auto& rel : walkFiles(metaDir, nullptr)) {
        if (rel.size() > 5 && rel.compare(rel.size() - 5, 5, ".json") == 0 &&
            rel.find('/') == std::string::npos)
            names.push_back(rel.substr(0, rel.size() - 5));
    }
    std::sort(names.begin(), names.end());
    return names;
}

}  // namespace hive
