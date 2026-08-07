// The .pkg container: pack, unpack, verify. See hive.hpp for the layout.
//
//     BEEPKG1\n                 magic line, so a wrong file says so plainly
//     <u32 LE> <u32 LE>         uncompressed size, compressed size
//     <compressed bytes>        LZSS of:  <header length>\n<header JSON>\n<file bytes>
//
// The payload is compressed rather than stored, which makes a package a binary
// blob instead of a text file with the sources sitting in it, and roughly halves
// its size. To be clear about what that is and isn't: it is not protection. The
// interpreter has to read a package, so anything that can install one can also
// extract one. It keeps a package from being casually browsed or edited in a
// text editor; it does not keep source code secret.
//
// Compression is written out here rather than pulled in, because a package
// manager that needs zlib to read its own format is a package manager with a
// dependency problem.
#include "hive.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

namespace hive {

static const char* kMagic = "BEEPKG1";

// ---------------------------------------------------------------------------
// LZSS
// ---------------------------------------------------------------------------
// One flag byte carries eight tokens: a clear bit is a literal, a set bit is a
// back-reference of (u16 offset, u8 length - kMinMatch).
namespace {

const size_t kMinMatch = 4;
const size_t kMaxMatch = 255 + kMinMatch;
const size_t kMaxOffset = 65535;
const int kMaxChain = 64;          // probes per position: ratio vs. time

inline uint32_t hash4(const char* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return (v * 2654435761u) >> 16;      // 16-bit bucket index
}

std::string compress(const std::string& in) {
    std::string out;
    if (in.empty()) return out;
    out.reserve(in.size() / 2 + 16);

    const size_t n = in.size();
    std::vector<int32_t> head(1 << 16, -1);
    std::vector<int32_t> prev(n, -1);

    size_t flagPos = out.size();
    out.push_back(0);
    uint8_t flags = 0;
    int nbits = 0;

    size_t i = 0;
    while (i < n) {
        if (nbits == 8) {
            out[flagPos] = (char)flags;
            flagPos = out.size();
            out.push_back(0);
            flags = 0;
            nbits = 0;
        }

        size_t bestLen = 0, bestOff = 0;
        if (i + kMinMatch <= n) {
            const uint32_t h = hash4(in.data() + i);
            int32_t cand = head[h];
            int probes = 0;
            while (cand >= 0 && probes++ < kMaxChain) {
                const size_t off = i - (size_t)cand;
                if (off == 0 || off > kMaxOffset) break;
                const size_t limit = std::min(kMaxMatch, n - i);
                size_t len = 0;
                while (len < limit && in[(size_t)cand + len] == in[i + len]) len++;
                if (len > bestLen) {
                    bestLen = len;
                    bestOff = off;
                    if (len == kMaxMatch) break;
                }
                cand = prev[(size_t)cand];
            }
        }

        auto insert = [&](size_t at) {
            if (at + kMinMatch > n) return;
            const uint32_t h = hash4(in.data() + at);
            prev[at] = head[h];
            head[h] = (int32_t)at;
        };

        if (bestLen >= kMinMatch) {
            flags |= (uint8_t)(1u << nbits);
            out.push_back((char)(bestOff & 0xff));
            out.push_back((char)((bestOff >> 8) & 0xff));
            out.push_back((char)(bestLen - kMinMatch));
            for (size_t k = 0; k < bestLen; ++k) insert(i + k);
            i += bestLen;
        } else {
            out.push_back(in[i]);
            insert(i);
            i += 1;
        }
        nbits++;
    }
    out[flagPos] = (char)flags;
    return out;
}

// Bounds-checked throughout: this parses a file that may be hostile, so every
// read is against the actual buffer and every back-reference against what has
// been produced so far.
bool decompress(const std::string& in, size_t expected, std::string& out, std::string& err) {
    out.clear();
    if (expected == 0) return in.empty();
    out.reserve(expected);

    size_t p = 0;
    while (out.size() < expected) {
        if (p >= in.size()) {
            err = "truncated package data";
            return false;
        }
        const uint8_t flags = (uint8_t)in[p++];
        for (int b = 0; b < 8 && out.size() < expected; ++b) {
            if (flags & (1u << b)) {
                if (p + 3 > in.size()) {
                    err = "truncated package data";
                    return false;
                }
                const size_t off = (uint8_t)in[p] | ((size_t)(uint8_t)in[p + 1] << 8);
                const size_t len = (size_t)(uint8_t)in[p + 2] + kMinMatch;
                p += 3;
                if (off == 0 || off > out.size()) {
                    err = "corrupt package data (bad back-reference)";
                    return false;
                }
                if (out.size() + len > expected) {
                    err = "corrupt package data (over-long match)";
                    return false;
                }
                const size_t from = out.size() - off;
                for (size_t k = 0; k < len; ++k) out.push_back(out[from + k]);
            } else {
                if (p >= in.size()) {
                    err = "truncated package data";
                    return false;
                }
                out.push_back(in[p++]);
            }
        }
    }
    return true;
}

// Whitening. LZSS emits literals before it has anything to match against, so
// the first few hundred bytes of a compressed package are otherwise plainly
// readable JSON. XORing with a keystream makes the file uniformly opaque.
//
// This is obfuscation, not encryption, and the difference matters: the key is
// four lines above this comment, in source anyone can read. It stops a package
// from being browsed or hand-edited in a text editor. It does not keep anything
// in a package secret, and nothing should be shipped in one that needs to be.
void whiten(std::string& data, uint32_t seed) {
    uint32_t x = seed ? seed : 0x9e3779b9u;
    for (size_t i = 0; i < data.size(); ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        data[i] = (char)((uint8_t)data[i] ^ (uint8_t)(x >> 24));
    }
}

void putU32(std::string& s, uint32_t v) {
    s.push_back((char)(v & 0xff));
    s.push_back((char)((v >> 8) & 0xff));
    s.push_back((char)((v >> 16) & 0xff));
    s.push_back((char)((v >> 24) & 0xff));
}

uint32_t getU32(const std::string& s, size_t at) {
    return (uint32_t)(uint8_t)s[at] | ((uint32_t)(uint8_t)s[at + 1] << 8) |
           ((uint32_t)(uint8_t)s[at + 2] << 16) | ((uint32_t)(uint8_t)s[at + 3] << 24);
}

}  // namespace

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
    std::string payload;
    payload.reserve(headerText.size() + 64);
    payload += std::to_string(headerText.size());
    payload += '\n';
    payload += headerText;
    payload += '\n';
    for (auto& e : entries) payload += e.data;

    if (payload.size() > 0xffffffffull) {
        err = "package is too large to pack (over 4 GB)";
        return false;
    }

    std::string packed = compress(payload);
    // Keyed on the payload length, so two packages don't share a keystream.
    whiten(packed, (uint32_t)payload.size() * 2654435761u);
    std::string blob;
    blob.reserve(packed.size() + 16);
    blob += kMagic;
    blob += '\n';
    putU32(blob, (uint32_t)payload.size());
    putU32(blob, (uint32_t)packed.size());
    blob += packed;

    return writeFileMkdirs(outPath, blob, err);
}

// ------------------------------------------------------------------ reading
bool readArchive(const std::string& raw, Archive& out, std::string& err) {
    // ---- outer container: magic, sizes, compressed payload ----------------
    const size_t nl = raw.find('\n');
    if (nl == std::string::npos || nl > 32) {
        err = "not a .pkg package (no header)";
        return false;
    }
    const std::string magic = raw.substr(0, nl);
    if (magic != kMagic) {
        err = "unsupported package format '" + magic + "' (this hive reads " + kMagic + ")";
        if (magic == "HIVE1")
            err += "\n       that is an old .hive archive -- repack it with 'hive pack'";
        return false;
    }
    if (raw.size() < nl + 1 + 8) {
        err = "truncated package";
        return false;
    }
    const uint32_t rawSize = getU32(raw, nl + 1);
    const uint32_t packedSize = getU32(raw, nl + 5);
    if (rawSize > 512u * 1024 * 1024) {
        err = "package claims an implausible uncompressed size";
        return false;
    }
    if (raw.size() != nl + 9 + (size_t)packedSize) {
        err = "truncated or padded package (expected " + std::to_string(packedSize) +
              " compressed bytes)";
        return false;
    }

    std::string packed = raw.substr(nl + 9);
    whiten(packed, (uint32_t)rawSize * 2654435761u);

    std::string blob;
    if (!decompress(packed, rawSize, blob, err)) return false;

    size_t pos = 0;
    auto line = [&](std::string& dst) -> bool {
        size_t at = blob.find('\n', pos);
        if (at == std::string::npos) return false;
        dst = blob.substr(pos, at - pos);
        pos = at + 1;
        return true;
    };

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
