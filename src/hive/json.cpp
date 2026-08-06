// A small JSON reader/writer for manifests, lockfiles and registry metadata.
#include "hive.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace hive {

// ---------------------------------------------------------------- accessors
const Json* Json::find(const std::string& key) const {
    if (type != Type::Object) return nullptr;
    for (auto& m : members)
        if (m.first == key) return &m.second;
    return nullptr;
}

Json* Json::find(const std::string& key) {
    if (type != Type::Object) return nullptr;
    for (auto& m : members)
        if (m.first == key) return &m.second;
    return nullptr;
}

std::string Json::str(const std::string& key, const std::string& fallback) const {
    const Json* v = find(key);
    return (v && v->isString()) ? v->text : fallback;
}

double Json::num(const std::string& key, double fallback) const {
    const Json* v = find(key);
    return (v && v->isNumber()) ? v->number : fallback;
}

bool Json::flag(const std::string& key, bool fallback) const {
    const Json* v = find(key);
    return (v && v->isBool()) ? v->boolean : fallback;
}

Json& Json::set(const std::string& key, Json value) {
    type = Type::Object;
    for (auto& m : members) {
        if (m.first == key) {
            m.second = std::move(value);
            return m.second;
        }
    }
    members.emplace_back(key, std::move(value));
    return members.back().second;
}

void Json::erase(const std::string& key) {
    for (size_t i = 0; i < members.size(); ++i) {
        if (members[i].first == key) {
            members.erase(members.begin() + (long)i);
            return;
        }
    }
}

void Json::push(Json value) {
    type = Type::Array;
    items.push_back(std::move(value));
}

// ---------------------------------------------------------------- writing
static void writeString(const std::string& s, std::string& out) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\t': out += "\\t";  break;
            case '\r': out += "\\r";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if (c < 0x20) {
                    char buf[7];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    out += '"';
}

static void writeNumber(double n, std::string& out) {
    if (!std::isfinite(n)) { out += "null"; return; }
    // Integral values print without a decimal point; everything else gets
    // enough digits to round-trip.
    if (n == (double)(long long)n && std::fabs(n) < 1e15) {
        out += std::to_string((long long)n);
        return;
    }
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.17g", n);
    out += buf;
}

static void dumpValue(const Json& j, int indent, int depth, std::string& out) {
    const bool pretty = indent > 0;
    const std::string pad(pretty ? (size_t)indent * (size_t)(depth + 1) : 0, ' ');
    const std::string padEnd(pretty ? (size_t)indent * (size_t)depth : 0, ' ');

    switch (j.type) {
        case Json::Type::Null:   out += "null"; break;
        case Json::Type::Bool:   out += j.boolean ? "true" : "false"; break;
        case Json::Type::Number: writeNumber(j.number, out); break;
        case Json::Type::String: writeString(j.text, out); break;

        case Json::Type::Array:
            if (j.items.empty()) { out += "[]"; break; }
            out += '[';
            for (size_t i = 0; i < j.items.size(); ++i) {
                if (i) out += ',';
                if (pretty) { out += '\n'; out += pad; }
                dumpValue(j.items[i], indent, depth + 1, out);
            }
            if (pretty) { out += '\n'; out += padEnd; }
            out += ']';
            break;

        case Json::Type::Object:
            if (j.members.empty()) { out += "{}"; break; }
            out += '{';
            for (size_t i = 0; i < j.members.size(); ++i) {
                if (i) out += ',';
                if (pretty) { out += '\n'; out += pad; }
                writeString(j.members[i].first, out);
                out += pretty ? ": " : ":";
                dumpValue(j.members[i].second, indent, depth + 1, out);
            }
            if (pretty) { out += '\n'; out += padEnd; }
            out += '}';
            break;
    }
}

std::string Json::dump(int indent) const {
    std::string out;
    dumpValue(*this, indent, 0, out);
    return out;
}

// ---------------------------------------------------------------- parsing
namespace {

class Parser {
public:
    Parser(const std::string& s) : s_(s) {}

    bool run(Json& out) {
        skipWs();
        if (!value(out, 0)) return false;
        skipWs();
        if (i_ != s_.size()) return fail("unexpected trailing characters");
        return true;
    }

    const std::string& error() const { return err_; }

private:
    const std::string& s_;
    size_t i_ = 0;
    std::string err_;

    bool fail(const std::string& what) {
        if (err_.empty())
            err_ = what + " at offset " + std::to_string(i_);
        return false;
    }

    void skipWs() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r'))
            i_++;
    }

    char peek() const { return i_ < s_.size() ? s_[i_] : '\0'; }

    bool literal(const char* lit) {
        for (const char* p = lit; *p; ++p) {
            if (i_ >= s_.size() || s_[i_] != *p) return fail(std::string("expected '") + lit + "'");
            i_++;
        }
        return true;
    }

    bool value(Json& out, int depth) {
        if (depth > 64) return fail("nesting too deep");
        skipWs();
        switch (peek()) {
            case '{': return object(out, depth);
            case '[': return array(out, depth);
            case '"': {
                std::string t;
                if (!string(t)) return false;
                out = Json(t);
                return true;
            }
            case 't': if (!literal("true")) return false;  out = Json(true);  return true;
            case 'f': if (!literal("false")) return false; out = Json(false); return true;
            case 'n': if (!literal("null")) return false;  out = Json();     return true;
            default:  return number(out);
        }
    }

    bool object(Json& out, int depth) {
        i_++;  // '{'
        out = Json::object();
        skipWs();
        if (peek() == '}') { i_++; return true; }
        for (;;) {
            skipWs();
            std::string key;
            if (!string(key)) return false;
            skipWs();
            if (peek() != ':') return fail("expected ':'");
            i_++;
            Json v;
            if (!value(v, depth + 1)) return false;
            out.set(key, std::move(v));
            skipWs();
            if (peek() == ',') { i_++; continue; }
            if (peek() == '}') { i_++; return true; }
            return fail("expected ',' or '}'");
        }
    }

    bool array(Json& out, int depth) {
        i_++;  // '['
        out = Json::array();
        skipWs();
        if (peek() == ']') { i_++; return true; }
        for (;;) {
            Json v;
            if (!value(v, depth + 1)) return false;
            out.items.push_back(std::move(v));
            skipWs();
            if (peek() == ',') { i_++; continue; }
            if (peek() == ']') { i_++; return true; }
            return fail("expected ',' or ']'");
        }
    }

    // Encode one code point as UTF-8.
    static void appendUtf8(unsigned cp, std::string& out) {
        if (cp < 0x80) {
            out += (char)cp;
        } else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(unsigned& out) {
        out = 0;
        for (int k = 0; k < 4; ++k) {
            if (i_ >= s_.size()) return fail("truncated \\u escape");
            char c = s_[i_++];
            unsigned d;
            if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
            else return fail("bad \\u escape");
            out = out * 16 + d;
        }
        return true;
    }

    bool string(std::string& out) {
        if (peek() != '"') return fail("expected a string");
        i_++;
        out.clear();
        while (i_ < s_.size() && s_[i_] != '"') {
            char c = s_[i_++];
            if (c != '\\') { out += c; continue; }
            if (i_ >= s_.size()) return fail("truncated escape");
            char e = s_[i_++];
            switch (e) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'n':  out += '\n'; break;
                case 't':  out += '\t'; break;
                case 'r':  out += '\r'; break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'u': {
                    unsigned cp;
                    if (!hex4(cp)) return false;
                    // Combine a surrogate pair when the low half follows.
                    if (cp >= 0xD800 && cp <= 0xDBFF && i_ + 1 < s_.size() &&
                        s_[i_] == '\\' && s_[i_ + 1] == 'u') {
                        size_t save = i_;
                        i_ += 2;
                        unsigned lo;
                        if (!hex4(lo)) return false;
                        if (lo >= 0xDC00 && lo <= 0xDFFF)
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        else
                            i_ = save;  // not a pair; leave it for the next round
                    }
                    appendUtf8(cp, out);
                    break;
                }
                default: return fail("unknown escape");
            }
        }
        if (i_ >= s_.size()) return fail("unterminated string");
        i_++;  // closing quote
        return true;
    }

    bool number(Json& out) {
        size_t start = i_;
        if (peek() == '-' || peek() == '+') i_++;
        bool digits = false;
        while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') { i_++; digits = true; }
        if (i_ < s_.size() && s_[i_] == '.') {
            i_++;
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') { i_++; digits = true; }
        }
        if (!digits) return fail("unexpected character");
        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            i_++;
            if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) i_++;
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') i_++;
        }
        out = Json(std::strtod(s_.substr(start, i_ - start).c_str(), nullptr));
        return true;
    }
};

}  // namespace

bool Json::parse(const std::string& src, Json& out, std::string& err) {
    Parser p(src);
    if (p.run(out)) return true;
    err = p.error();
    return false;
}

}  // namespace hive
