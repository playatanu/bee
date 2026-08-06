// The .hive container: pack, unpack, verify. See hive.hpp for the layout.
#include "hive.hpp"

#include <cstdlib>
#include <fstream>

namespace hive {

static const char* kMagic = "HIVE1";

bool safeRelativePath(const std::string& path) {
    if (path.empty() || path.size() > 1024) return false;
    if (path != normalizeSlashes(path)) return false;  // no backslashes
    if (path[0] == '/') return false;                  // no absolute paths
    if (path.size() > 1 && path[1] == ':') return false;  // no C:\ drive paths
    if (path.find('\0') != std::string::npos) return false;

    // Reject "..", "." and empty components, which is what stops a crafted
    // archive from writing outside the package directory.
    size_t start = 0;
    while (start <= path.size()) {
        size_t slash = path.find('/', start);
        std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part.empty() || part == "." || part == "..") return false;
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return true;
}

// ------------------------------------------------------------------ writing
bool writeArchive(const std::string& outPath, const Json& manifestJson,
                  const std::vector<ArchiveEntry>& entries, std::string& err) {
    Json files = Json::array();
    for (auto& e : entries) {
        if (!safeRelativePath(e.path)) {
            err = "refusing to pack unsafe path '" + e.path + "'";
            return false;
        }
        Json f = Json::object();
        f.set("path", Json(e.path));
        f.set("size", Json((double)e.data.size()));
        f.set("sha256", Json(sha256Hex(e.data)));
        files.push(std::move(f));
    }

    Json header = Json::object();
    header.set("format", Json((double)1));
    header.set("manifest", manifestJson);
    header.set("files", std::move(files));

    const std::string headerText = header.dump();
    std::string blob;
    blob.reserve(headerText.size() + 64);
    blob += kMagic;
    blob += '\n';
    blob += std::to_string(headerText.size());
    blob += '\n';
    blob += headerText;
    blob += '\n';
    for (auto& e : entries) blob += e.data;

    return writeFileMkdirs(outPath, blob, err);
}

// ------------------------------------------------------------------ reading
bool readArchive(const std::string& blob, Archive& out, std::string& err) {
    size_t pos = 0;
    auto line = [&](std::string& dst) -> bool {
        size_t nl = blob.find('\n', pos);
        if (nl == std::string::npos) return false;
        dst = blob.substr(pos, nl - pos);
        pos = nl + 1;
        return true;
    };

    std::string magic;
    if (!line(magic)) {
        err = "not a .hive archive (no header)";
        return false;
    }
    if (magic != kMagic) {
        err = "unsupported archive format '" + magic + "' (this hive reads " + kMagic + ")";
        return false;
    }

    std::string lenText;
    if (!line(lenText)) {
        err = "truncated archive header";
        return false;
    }
    char* end = nullptr;
    unsigned long long headerLen = std::strtoull(lenText.c_str(), &end, 10);
    if (!end || *end != '\0' || lenText.empty() || headerLen == 0 || headerLen > 8u * 1024 * 1024) {
        err = "bad archive header length '" + lenText + "'";
        return false;
    }
    if (pos + headerLen + 1 > blob.size()) {
        err = "truncated archive header";
        return false;
    }

    Json header;
    std::string jsonErr;
    if (!Json::parse(blob.substr(pos, (size_t)headerLen), header, jsonErr)) {
        err = "bad archive header: " + jsonErr;
        return false;
    }
    pos += (size_t)headerLen + 1;  // header text plus its newline

    if ((int)header.num("format", 0) != 1) {
        err = "unsupported archive format version";
        return false;
    }
    const Json* mj = header.find("manifest");
    if (!mj || !mj->isObject()) {
        err = "archive has no manifest";
        return false;
    }
    out.manifestJson = *mj;
    if (!Manifest::fromJson(*mj, /*requireName=*/true, out.manifest, err)) {
        err = "bad manifest in archive: " + err;
        return false;
    }

    const Json* files = header.find("files");
    if (!files || !files->isArray()) {
        err = "archive has no file list";
        return false;
    }

    out.entries.clear();
    for (auto& f : files->items) {
        if (!f.isObject()) {
            err = "bad file entry in archive";
            return false;
        }
        ArchiveEntry e;
        e.path = f.str("path");
        if (!safeRelativePath(e.path)) {
            err = "archive contains an unsafe path '" + e.path + "'";
            return false;
        }
        double size = f.num("size", -1);
        if (size < 0 || size > 512.0 * 1024 * 1024) {
            err = "bad size for '" + e.path + "'";
            return false;
        }
        size_t n = (size_t)size;
        if (pos + n > blob.size()) {
            err = "archive is truncated at '" + e.path + "'";
            return false;
        }
        e.data = blob.substr(pos, n);
        pos += n;

        const std::string want = f.str("sha256");
        if (!want.empty() && sha256Hex(e.data) != want) {
            err = "checksum mismatch for '" + e.path + "' -- the archive is corrupt";
            return false;
        }
        out.entries.push_back(std::move(e));
    }

    if (pos != blob.size()) {
        err = "archive has " + std::to_string(blob.size() - pos) + " unexpected trailing bytes";
        return false;
    }
    return true;
}

}  // namespace hive
