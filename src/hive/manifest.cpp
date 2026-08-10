// Reading hive.json, and the version arithmetic that dependency resolution needs.
#include "hive.hpp"

#include <cctype>
#include <cstdlib>

namespace hive {

// ------------------------------------------------------------------ names
bool validPackageName(const std::string& name) {
    // Lowercase, digits, '-' and '_'; must start with a letter. Deliberately
    // narrow: a package name becomes a directory name *and* a Bee
    // identifier in `import <name>`, so it has to be legal as both.
    if (name.size() < 2 || name.size() > 64) return false;
    if (!(name[0] >= 'a' && name[0] <= 'z')) return false;
    for (char c : name) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

// ------------------------------------------------------------------ versions
namespace {

struct Version {
    long long parts[3] = {0, 0, 0};
    std::string prerelease;
    bool valid = false;
};

Version parseVersion(const std::string& s) {
    Version v;
    size_t i = 0;
    std::string core = s;
    size_t dash = s.find('-');
    if (dash != std::string::npos) {
        core = s.substr(0, dash);
        v.prerelease = s.substr(dash + 1);
        if (v.prerelease.empty()) return v;
    }

    int part = 0;
    i = 0;
    while (part < 3) {
        size_t start = i;
        while (i < core.size() && std::isdigit((unsigned char)core[i])) i++;
        if (i == start) return v;  // missing number
        v.parts[part++] = std::strtoll(core.substr(start, i - start).c_str(), nullptr, 10);
        if (i == core.size()) break;
        if (core[i] != '.') return v;
        i++;
    }
    if (i != core.size()) return v;  // trailing junk, or a 4th component
    v.valid = true;
    return v;
}

int cmpNum(long long a, long long b) { return a < b ? -1 : (a > b ? 1 : 0); }

}  // namespace

bool validVersion(const std::string& v) { return parseVersion(v).valid; }

int compareVersions(const std::string& a, const std::string& b) {
    Version x = parseVersion(a), y = parseVersion(b);
    for (int i = 0; i < 3; ++i)
        if (int c = cmpNum(x.parts[i], y.parts[i])) return c;
    // A prerelease sorts before the release it leads up to.
    if (x.prerelease.empty() != y.prerelease.empty())
        return x.prerelease.empty() ? 1 : -1;
    if (x.prerelease == y.prerelease) return 0;
    return x.prerelease < y.prerelease ? -1 : 1;
}

namespace {

// Split "^1.0.0, <2.0.0" into its individual terms.
std::vector<std::string> constraintTerms(const std::string& constraint) {
    std::vector<std::string> terms;
    std::string cur;
    for (char c : constraint) {
        if (c == ',' || c == ' ' || c == '\t') {
            if (!cur.empty()) { terms.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) terms.push_back(cur);
    return terms;
}

struct Term {
    enum class Op { Any, Eq, Caret, Tilde, Ge, Gt, Le, Lt } op = Op::Any;
    std::string version;
    bool valid = false;
};

Term parseTerm(const std::string& raw) {
    Term t;
    if (raw.empty() || raw == "*" || raw == "latest" || raw == "x") {
        t.op = Term::Op::Any;
        t.valid = true;
        return t;
    }
    size_t i = 0;
    if (raw.rfind(">=", 0) == 0)      { t.op = Term::Op::Ge;    i = 2; }
    else if (raw.rfind("<=", 0) == 0) { t.op = Term::Op::Le;    i = 2; }
    else if (raw[0] == '>')           { t.op = Term::Op::Gt;    i = 1; }
    else if (raw[0] == '<')           { t.op = Term::Op::Lt;    i = 1; }
    else if (raw[0] == '^')           { t.op = Term::Op::Caret; i = 1; }
    else if (raw[0] == '~')           { t.op = Term::Op::Tilde; i = 1; }
    else if (raw[0] == '=')           { t.op = Term::Op::Eq;    i = 1; }
    else                              { t.op = Term::Op::Eq;    i = 0; }

    t.version = raw.substr(i);
    t.valid = validVersion(t.version);
    return t;
}

bool satisfiesTerm(const std::string& version, const Term& t) {
    if (t.op == Term::Op::Any) return true;
    int c = compareVersions(version, t.version);
    switch (t.op) {
        case Term::Op::Eq: return c == 0;
        case Term::Op::Ge: return c >= 0;
        case Term::Op::Gt: return c > 0;
        case Term::Op::Le: return c <= 0;
        case Term::Op::Lt: return c < 0;
        case Term::Op::Caret: {
            // Compatible-with: no change to the leftmost non-zero component.
            if (c < 0) return false;
            Version want = parseVersion(t.version), have = parseVersion(version);
            if (want.parts[0] != 0) return have.parts[0] == want.parts[0];
            if (want.parts[1] != 0) return have.parts[0] == 0 && have.parts[1] == want.parts[1];
            return have.parts[0] == 0 && have.parts[1] == 0 && have.parts[2] == want.parts[2];
        }
        case Term::Op::Tilde: {
            // Patch-level changes only.
            if (c < 0) return false;
            Version want = parseVersion(t.version), have = parseVersion(version);
            return have.parts[0] == want.parts[0] && have.parts[1] == want.parts[1];
        }
        case Term::Op::Any: return true;
    }
    return false;
}

}  // namespace

bool validConstraint(const std::string& constraint) {
    auto terms = constraintTerms(constraint);
    if (terms.empty()) return true;  // empty means "any"
    for (auto& raw : terms) {
        Term t = parseTerm(raw);
        if (!t.valid) return false;
    }
    return true;
}

bool versionSatisfies(const std::string& version, const std::string& constraint) {
    if (!validVersion(version)) return false;
    auto terms = constraintTerms(constraint);
    if (terms.empty()) return true;
    for (auto& raw : terms) {
        Term t = parseTerm(raw);
        if (!t.valid) return false;
        if (!satisfiesTerm(version, t)) return false;
    }
    return true;
}

// ------------------------------------------------------------------ manifests
static bool readStringList(const Json& obj, const std::string& key,
                           std::vector<std::string>& out, std::string& err) {
    const Json* v = obj.find(key);
    if (!v || v->isNull()) return true;
    if (!v->isArray()) {
        err = "'" + key + "' must be an array of strings";
        return false;
    }
    for (auto& item : v->items) {
        if (!item.isString()) {
            err = "'" + key + "' must contain only strings";
            return false;
        }
        out.push_back(item.text);
    }
    return true;
}

// Dependencies are read from an object of name -> constraint.
static bool readDeps(const Json& obj, DepList& out, std::string& err) {
    const Json* v = obj.find("dependencies");
    if (!v || v->isNull()) return true;
    if (!v->isObject()) {
        err = "'dependencies' must be an object of name -> version constraint";
        return false;
    }
    for (auto& m : v->members) {
        if (!validPackageName(m.first)) {
            err = "'" + m.first + "' is not a valid package name";
            return false;
        }
        if (!m.second.isString()) {
            err = "dependency '" + m.first + "' must map to a version constraint string";
            return false;
        }
        if (!validConstraint(m.second.text)) {
            err = "dependency '" + m.first + "' has an invalid version constraint '" + m.second.text + "'";
            return false;
        }
        out.emplace_back(m.first, m.second.text);
    }
    return true;
}

bool Manifest::fromJson(const Json& j, bool requireName, Manifest& out, std::string& err) {
    if (!j.isObject()) {
        err = "expected a JSON object";
        return false;
    }
    out = Manifest();
    out.raw = j;
    out.name = j.str("name");
    out.version = j.str("version");
    out.description = j.str("description");
    out.author = j.str("author");
    out.license = j.str("license");
    out.main = j.str("main");
    out.homepage = j.str("homepage");
    out.repository = j.str("repository");

    if (requireName) {
        if (out.name.empty()) { err = "missing 'name'"; return false; }
        if (!validPackageName(out.name)) {
            err = "invalid package name '" + out.name +
                  "' (use lowercase letters, digits, '-' and '_', starting with a letter)";
            return false;
        }
        if (out.version.empty()) { err = "missing 'version'"; return false; }
        if (!validVersion(out.version)) {
            err = "invalid version '" + out.version + "' (expected MAJOR.MINOR.PATCH)";
            return false;
        }
    } else if (!out.name.empty() && !validPackageName(out.name)) {
        err = "invalid package name '" + out.name + "'";
        return false;
    } else if (!out.version.empty() && !validVersion(out.version)) {
        err = "invalid version '" + out.version + "'";
        return false;
    }

    if (!out.main.empty() && !safeRelativePath(out.main)) {
        err = "'main' must be a path inside the package";
        return false;
    }
    if (!readStringList(j, "keywords", out.keywords, err)) return false;
    if (!readStringList(j, "files", out.files, err)) return false;
    if (!readStringList(j, "exclude", out.exclude, err)) return false;
    if (const Json* bins = j.find("binaries")) {
        if (!bins->isObject()) {
            err = "'binaries' must be an object of platform -> path";
            return false;
        }
        for (auto& m : bins->members) {
            if (!m.second.isString()) {
                err = "'binaries." + m.first + "' must be a path";
                return false;
            }
            out.binaries.emplace_back(m.first, m.second.text);
        }
    }
    out.build = j.str("build");
    if (out.build.find('\n') != std::string::npos) {
        err = "'build' must be a single command";
        return false;
    }
    if (!readDeps(j, out.dependencies, err)) return false;
    return true;
}

std::string Manifest::entryFile() const {
    return main.empty() ? "init.bee" : main;
}

}  // namespace hive
