// Extra built-ins: higher-order collection operations, a math library, and JSON.
#include "interpreter.hpp"

#include <cmath>
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace bee {

static double numArgE(const Value& v, const std::string& who) {
    if (!v.isNumber()) throw RuntimeError(who + ": expected a number");
    return v.asNumber();
}
static std::shared_ptr<ValueList> listArg(const Value& v, const std::string& who) {
    if (!v.isList()) throw RuntimeError(who + ": expected a list");
    return v.asList();
}

// Default ordering for sort() when no comparator is given.
static bool defaultLess(const Value& a, const Value& b) {
    if (a.isNumber() && b.isNumber()) return a.asNumber() < b.asNumber();
    if (a.isString() && b.isString()) return a.asString() < b.asString();
    throw RuntimeError("sort: values are not comparable (use a comparator function)");
}

// ------------------------------------------------------------------
// JSON
// ------------------------------------------------------------------

namespace {
struct JsonParser {
    const std::string& s;
    size_t i = 0;
    explicit JsonParser(const std::string& src) : s(src) {}

    [[noreturn]] void fail(const std::string& msg) { throw RuntimeError("json_parse: " + msg); }
    void ws() { while (i < s.size() && (s[i]==' '||s[i]=='\t'||s[i]=='\n'||s[i]=='\r')) i++; }
    char peek() { return i < s.size() ? s[i] : '\0'; }

    Value parse() {
        ws();
        Value v = value();
        ws();
        if (i != s.size()) fail("trailing characters");
        return v;
    }

    Value value() {
        ws();
        char c = peek();
        if (c == '{') return object();
        if (c == '[') return array();
        if (c == '"') return Value(string());
        if (c == 't' || c == 'f') return boolean();
        if (c == 'n') { expect("null"); return Value(); }
        if (c == '-' || (c >= '0' && c <= '9')) return number();
        fail("unexpected character");
    }

    void expect(const char* lit) {
        for (const char* p = lit; *p; ++p) {
            if (i >= s.size() || s[i] != *p) fail(std::string("expected '") + lit + "'");
            i++;
        }
    }

    Value boolean() {
        if (peek() == 't') { expect("true"); return Value(true); }
        expect("false"); return Value(false);
    }

    Value number() {
        size_t start = i;
        if (peek() == '-') i++;
        while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i]=='.' || s[i]=='e' || s[i]=='E' || s[i]=='+' || s[i]=='-')) i++;
        try {
            return Value(std::stod(s.substr(start, i - start)));
        } catch (...) { fail("invalid number"); }
    }

    std::string string() {
        if (peek() != '"') fail("expected string");
        i++; // opening quote
        std::string out;
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c == '\\') {
                if (i >= s.size()) fail("bad escape");
                char e = s[i++];
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': {
                        if (i + 4 > s.size()) fail("bad \\u escape");
                        int code = std::stoi(s.substr(i, 4), nullptr, 16);
                        i += 4;
                        // Minimal UTF-8 encoding of the code point.
                        if (code < 0x80) out += (char)code;
                        else if (code < 0x800) {
                            out += (char)(0xC0 | (code >> 6));
                            out += (char)(0x80 | (code & 0x3F));
                        } else {
                            out += (char)(0xE0 | (code >> 12));
                            out += (char)(0x80 | ((code >> 6) & 0x3F));
                            out += (char)(0x80 | (code & 0x3F));
                        }
                        break;
                    }
                    default: fail("bad escape");
                }
            } else {
                out += c;
            }
        }
        if (i >= s.size()) fail("unterminated string");
        i++; // closing quote
        return out;
    }

    Value array() {
        i++; // [
        auto list = std::make_shared<ValueList>();
        ws();
        if (peek() == ']') { i++; return Value(list); }
        while (true) {
            list->push_back(value());
            ws();
            if (peek() == ',') { i++; continue; }
            if (peek() == ']') { i++; break; }
            fail("expected ',' or ']'");
        }
        return Value(list);
    }

    Value object() {
        i++; // {
        auto dict = std::make_shared<ValueDict>();
        ws();
        if (peek() == '}') { i++; return Value(dict); }
        while (true) {
            ws();
            std::string key = string();
            ws();
            if (peek() != ':') fail("expected ':'");
            i++;
            (*dict)[key] = value();
            ws();
            if (peek() == ',') { i++; continue; }
            if (peek() == '}') { i++; break; }
            fail("expected ',' or '}'");
        }
        return Value(dict);
    }
};

std::string jsonNum(double d) {
    if (std::isnan(d) || std::isinf(d)) return "null"; // JSON has no NaN/Inf
    if (d == std::floor(d) && std::fabs(d) < 1e15)
        return std::to_string((long long)d);
    std::ostringstream os;
    os << std::setprecision(15) << d;
    return os.str();
}

void jsonEscape(const std::string& s, std::string& out) {
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}

void jsonWrite(const Value& v, std::string& out, int indent, int depth) {
    auto newlineIndent = [&](int d) {
        if (indent > 0) { out += '\n'; out.append((size_t)(indent * d), ' '); }
    };
    if (v.isNil())    { out += "null"; return; }
    if (v.isBool())   { out += v.asBool() ? "true" : "false"; return; }
    if (v.isNumber()) { out += jsonNum(v.asNumber()); return; }
    if (v.isString()) { jsonEscape(v.asString(), out); return; }
    if (v.isList()) {
        auto l = v.asList();
        if (l->empty()) { out += "[]"; return; }
        out += '[';
        for (size_t k = 0; k < l->size(); ++k) {
            if (k) out += ',';
            newlineIndent(depth + 1);
            jsonWrite((*l)[k], out, indent, depth + 1);
        }
        newlineIndent(depth);
        out += ']';
        return;
    }
    if (v.isDict()) {
        auto d = v.asDict();
        if (d->empty()) { out += "{}"; return; }
        out += '{';
        bool first = true;
        for (auto& kv : *d) {
            if (!first) out += ',';
            first = false;
            newlineIndent(depth + 1);
            jsonEscape(kv.first, out);
            out += indent > 0 ? ": " : ":";
            jsonWrite(kv.second, out, indent, depth + 1);
        }
        newlineIndent(depth);
        out += '}';
        return;
    }
    throw RuntimeError("json_str: value of this type is not serializable");
}
} // namespace

void Interpreter::defineExtraBuiltins() {
    auto def = [&](const std::string& n, int arity,
                   std::function<Value(Interpreter&, std::vector<Value>&)> f) {
        auto b = std::make_shared<Builtin>();
        b->name = n;
        b->arity = arity;
        b->fn = std::move(f);
        globals->define(n, Value(b));
    };

    // ---------------------------------------------------------------
    // Higher-order collection operations
    // ---------------------------------------------------------------
    def("map", 2, [](Interpreter& I, std::vector<Value>& a) -> Value {
        Value fn = a[0];
        auto l = listArg(a[1], "map");
        auto out = std::make_shared<ValueList>();
        out->reserve(l->size());
        for (auto& x : *l) {
            std::vector<Value> args{ x };
            out->push_back(I.callValue(fn, args, 0));
        }
        return Value(out);
    });

    def("filter", 2, [](Interpreter& I, std::vector<Value>& a) -> Value {
        Value fn = a[0];
        auto l = listArg(a[1], "filter");
        auto out = std::make_shared<ValueList>();
        for (auto& x : *l) {
            std::vector<Value> args{ x };
            if (I.callValue(fn, args, 0).truthy()) out->push_back(x);
        }
        return Value(out);
    });

    def("reduce", -1, [](Interpreter& I, std::vector<Value>& a) -> Value {
        if (a.size() < 2 || a.size() > 3)
            throw RuntimeError("reduce: expects (fn, list) or (fn, list, initial)");
        Value fn = a[0];
        auto l = listArg(a[1], "reduce");
        size_t start = 0;
        Value acc;
        if (a.size() == 3) { acc = a[2]; }
        else {
            if (l->empty()) throw RuntimeError("reduce: empty list with no initial value");
            acc = (*l)[0];
            start = 1;
        }
        for (size_t k = start; k < l->size(); ++k) {
            std::vector<Value> args{ acc, (*l)[k] };
            acc = I.callValue(fn, args, 0);
        }
        return acc;
    });

    def("sort", -1, [](Interpreter& I, std::vector<Value>& a) -> Value {
        if (a.empty() || a.size() > 2) throw RuntimeError("sort: expects (list) or (list, cmp)");
        auto src = listArg(a[0], "sort");
        auto out = std::make_shared<ValueList>(*src); // sort a copy
        if (a.size() == 2) {
            Value cmp = a[1];
            std::stable_sort(out->begin(), out->end(), [&](const Value& x, const Value& y) {
                std::vector<Value> args{ x, y };
                return I.callValue(cmp, args, 0).truthy();
            });
        } else {
            std::stable_sort(out->begin(), out->end(), defaultLess);
        }
        return Value(out);
    });

    def("reverse", 1, [](Interpreter&, std::vector<Value>& a) -> Value {
        auto src = listArg(a[0], "reverse");
        auto out = std::make_shared<ValueList>(src->rbegin(), src->rend());
        return Value(out);
    });

    // (list joining is the method `list.join(sep)`; `join` is thread join)

    def("sum", 1, [](Interpreter&, std::vector<Value>& a) -> Value {
        auto l = listArg(a[0], "sum");
        double total = 0;
        for (auto& x : *l) total += numArgE(x, "sum");
        return Value(total);
    });

    def("any", 2, [](Interpreter& I, std::vector<Value>& a) -> Value {
        Value fn = a[0];
        auto l = listArg(a[1], "any");
        for (auto& x : *l) {
            std::vector<Value> args{ x };
            if (I.callValue(fn, args, 0).truthy()) return Value(true);
        }
        return Value(false);
    });

    def("all", 2, [](Interpreter& I, std::vector<Value>& a) -> Value {
        Value fn = a[0];
        auto l = listArg(a[1], "all");
        for (auto& x : *l) {
            std::vector<Value> args{ x };
            if (!I.callValue(fn, args, 0).truthy()) return Value(false);
        }
        return Value(true);
    });

    def("find", 2, [](Interpreter& I, std::vector<Value>& a) -> Value {
        Value fn = a[0];
        auto l = listArg(a[1], "find");
        for (auto& x : *l) {
            std::vector<Value> args{ x };
            if (I.callValue(fn, args, 0).truthy()) return x;
        }
        return Value();
    });

    // ---------------------------------------------------------------
    // Math
    // ---------------------------------------------------------------
    globals->define("PI", Value(3.14159265358979323846));
    globals->define("E", Value(2.71828182845904523536));

    auto math1 = [&](const std::string& n, double (*f)(double)) {
        def(n, 1, [f, n](Interpreter&, std::vector<Value>& a) {
            return Value(f(numArgE(a[0], n)));
        });
    };
    math1("sin", std::sin);   math1("cos", std::cos);   math1("tan", std::tan);
    math1("asin", std::asin); math1("acos", std::acos); math1("atan", std::atan);
    math1("exp", std::exp);   math1("log", std::log);
    math1("log2", std::log2); math1("log10", std::log10);

    def("atan2", 2, [](Interpreter&, std::vector<Value>& a) {
        return Value(std::atan2(numArgE(a[0], "atan2"), numArgE(a[1], "atan2")));
    });
    def("hypot", 2, [](Interpreter&, std::vector<Value>& a) {
        return Value(std::hypot(numArgE(a[0], "hypot"), numArgE(a[1], "hypot")));
    });

    // ---------------------------------------------------------------
    // JSON
    // ---------------------------------------------------------------
    def("json_parse", 1, [](Interpreter&, std::vector<Value>& a) -> Value {
        if (!a[0].isString()) throw RuntimeError("json_parse: expected a string");
        JsonParser p(a[0].asString());
        return p.parse();
    });

    def("json_str", -1, [](Interpreter&, std::vector<Value>& a) -> Value {
        if (a.empty() || a.size() > 2)
            throw RuntimeError("json_str: expects (value) or (value, indent)");
        int indent = (a.size() == 2) ? (int)numArgE(a[1], "json_str") : 0;
        std::string out;
        jsonWrite(a[0], out, indent, 0);
        return Value(out);
    });
}

} // namespace bee
