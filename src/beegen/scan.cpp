// Walking a parsed header and deciding what can be bound.
#include "beegen.hpp"

#include <algorithm>
#include <set>

namespace beegen {

namespace {

bool startsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

// Strip the qualifiers and reference/pointer marks clang includes in a spelling,
// leaving something we can compare against a class name.
std::string bareName(std::string spelling) {
    for (const char* prefix : {"const ", "volatile ", "struct ", "class ", "enum ", "union "}) {
        std::string p = prefix;
        while (startsWith(spelling, p)) spelling.erase(0, p.size());
    }
    while (!spelling.empty() && (spelling.back() == '*' || spelling.back() == '&' ||
                                spelling.back() == ' '))
        spelling.pop_back();
    return spelling;
}

// The state a single run carries around: the scan needs to know which classes it
// has decided to bind before it can map a `Foo&` parameter to a handle.
class Scanner {
public:
    Scanner(Clang& clang, const Options& opts, Api& api)
        : clang_(clang), opts_(opts), api_(api) {}

    void run() {
        // Two passes: the first learns which records and enums are bindable, so
        // the second can map parameters that mention them.
        collectTypes(clang_.root(), "");
        collectDecls(clang_.root(), "");
    }

private:
    Clang& clang_;
    const Options& opts_;
    Api& api_;
    std::set<std::string> knownClasses_;
    std::set<std::string> knownEnums_;
    std::set<std::string> usedBeeNames_;

    // ---- filtering ----------------------------------------------------------
    // Only bind what the user asked for, and never anything from a system header
    // -- pulling in <string>'s internals is never what someone wants.
    bool wantedLocation(CXCursor c) const {
        if (clang_.inSystemHeader(c)) return false;
        const std::string file = clang_.fileOf(c);
        if (file.empty()) return false;
        return true;
    }

    bool wantedNamespace(const std::string& ns) const {
        if (opts_.namespaces.empty()) return ns.empty() || true;  // no filter: take all
        if (ns.empty()) return contains(opts_.namespaces, "");
        for (auto& want : opts_.namespaces)
            if (ns == want || startsWith(ns, want + "::")) return true;
        return false;
    }

    bool wantedName(const std::string& name) const {
        if (contains(opts_.skip, name)) return false;
        if (opts_.prefixes.empty()) return true;
        for (auto& p : opts_.prefixes)
            if (startsWith(name, p)) return true;
        return false;
    }

    void skip(const std::string& what, const std::string& reason) {
        api_.skipped.push_back({what, reason});
    }

    // A Bee-visible name must be unique across the flat native module.
    std::string uniqueName(const std::string& wanted) {
        std::string name = sanitizeIdentifier(wanted);
        if (usedBeeNames_.insert(name).second) return name;
        // An overload set: suffix rather than drop, so both stay reachable.
        for (int n = 2; n < 100; ++n) {
            std::string candidate = name + "_" + std::to_string(n);
            if (usedBeeNames_.insert(candidate).second) return candidate;
        }
        return name;
    }

    // ---- type mapping -------------------------------------------------------
    Mapped mapType(CXType type, bool isReturn) {
        Mapped m;
        m.cxxType = clang_.typeSpelling(type);

        CXType canon = clang_.canonical(type);
        switch (canon.kind) {
            case Type_Void:
                m.kind = isReturn ? Mapped::Kind::Void : Mapped::Kind::Unsupported;
                if (!isReturn) m.reason = "void parameter";
                return m;

            case Type_Bool:
                m.kind = Mapped::Kind::Bool;
                return m;

            case Type_Char_U: case Type_UChar: case Type_UShort: case Type_UInt:
            case Type_ULong: case Type_ULongLong:
            case Type_Char_S: case Type_SChar: case Type_Short: case Type_Int:
            case Type_Long: case Type_LongLong: case Type_WChar:
                m.kind = Mapped::Kind::Integer;
                m.cxxIntType = clang_.typeSpelling(canon);
                return m;

            case Type_Float: case Type_Double: case Type_LongDouble:
                m.kind = Mapped::Kind::Float;
                return m;

            case Type_Enum: {
                m.kind = Mapped::Kind::Enum;
                m.cxxIntType = "long long";
                return m;
            }

            case Type_Pointer: {
                CXType pointee = clang_.pointee(canon);
                CXType pointeeCanon = clang_.canonical(pointee);
                // const char* is a string; anything else is a handle if we know
                // the class, and unsupported otherwise.
                if ((pointeeCanon.kind == Type_Char_S || pointeeCanon.kind == Type_Char_U) &&
                    clang_.isConstQualified(pointee)) {
                    m.kind = Mapped::Kind::CString;
                    return m;
                }
                const std::string target = bareName(clang_.typeSpelling(pointeeCanon));
                if (knownClasses_.count(target)) {
                    m.kind = Mapped::Kind::Handle;
                    m.handleType = target;
                    m.byPointer = true;
                    return m;
                }
                m.reason = "pointer to unbound type '" + target + "'";
                return m;
            }

            case Type_LValueReference: case Type_RValueReference: {
                CXType referee = clang_.canonical(clang_.pointee(canon));
                const std::string spelled = clang_.typeSpelling(referee);
                if (isStdString(spelled)) {
                    m.kind = Mapped::Kind::StdString;
                    return m;
                }
                // A const reference to a buffer or a vector converts the same way
                // a value does.
                const std::string bareRef = bareName(spelled);
                if (bareRef == "BeeBuffer" || bareRef.rfind("std::vector<", 0) == 0) {
                    if (clang_.isConstQualified(clang_.pointee(canon)) || !isReturn) {
                        Mapped inner = mapType(referee, isReturn);
                        if (inner.ok()) return inner;
                    }
                }
                if (referee.kind == Type_Record) {
                    const std::string target = bareName(spelled);
                    if (knownClasses_.count(target)) {
                        m.kind = Mapped::Kind::Handle;
                        m.handleType = target;
                        m.byPointer = false;
                        return m;
                    }
                    m.reason = "reference to unbound type '" + target + "'";
                    return m;
                }
                // A reference to a scalar reads fine as a value in Bee, but only
                // when it can't be an out-parameter.
                if (clang_.isConstQualified(clang_.pointee(canon)))
                    return mapType(clang_.pointee(canon), isReturn);
                m.reason = "non-const reference (possible out-parameter)";
                return m;
            }

            case Type_Record: {
                const std::string spelled = clang_.typeSpelling(canon);
                if (isStdString(spelled)) {
                    m.kind = Mapped::Kind::StdString;
                    return m;
                }
                // BeeBuffer is the agreed way bulk data crosses: the buffer's own
                // memory is handed over by pointer, with no copy.
                if (bareName(spelled) == "BeeBuffer") {
                    m.kind = Mapped::Kind::BufferView;
                    return m;
                }
                const std::string bare = bareName(spelled);
                if (bare.rfind("std::vector<", 0) == 0 ||
                    bare.rfind("std::__cxx11::vector<", 0) == 0) {
                    if (clang_.numTemplateArgs(canon) < 1) {
                        m.reason = "vector with no element type";
                        return m;
                    }
                    Mapped elem = mapType(clang_.templateArg(canon, 0), false);
                    // Only element types that convert on their own: a vector of
                    // handles would need ownership rules Bee doesn't have.
                    switch (elem.kind) {
                        case Mapped::Kind::Bool:
                        case Mapped::Kind::Integer:
                        case Mapped::Kind::Float:
                        case Mapped::Kind::StdString:
                        case Mapped::Kind::CString:
                            m.kind = Mapped::Kind::Vector;
                            m.elemKind = elem.kind;
                            m.elemCxxType = elem.kind == Mapped::Kind::Integer
                                                ? elem.cxxIntType
                                                : clang_.typeSpelling(clang_.templateArg(canon, 0));
                            return m;
                        default:
                            m.reason = "vector of " + elem.cxxType + " (" +
                                       (elem.reason.empty() ? "unsupported element" : elem.reason) + ")";
                            return m;
                    }
                }
                const std::string target = bareName(spelled);
                if (knownClasses_.count(target)) {
                    // By value: on the way in we'd need a copy from a handle, on
                    // the way out a heap copy the caller has to free.
                    m.kind = isReturn ? Mapped::Kind::HandleValue : Mapped::Kind::Handle;
                    m.handleType = target;
                    m.byPointer = false;
                    return m;
                }
                m.reason = "unbound record type '" + target + "'";
                return m;
            }

            default:
                m.reason = "unsupported type '" + m.cxxType + "'";
                return m;
        }
    }

    static bool isStdString(const std::string& spelled) {
        const std::string bare = bareName(spelled);
        return bare == "std::string" || bare == "std::__cxx11::basic_string<char>" ||
               bare == "std::basic_string<char>" || bare == "std::string_view" ||
               bare == "std::basic_string_view<char>";
    }

    // ---- pass 1: which records and enums exist -----------------------------
    void collectTypes(CXCursor parent, const std::string& ns) {
        clang_.visit(parent, [&](CXCursor c, CXCursor) {
            switch (clang_.kind(c)) {
                case Cursor_Namespace: {
                    const std::string name = clang_.spelling(c);
                    collectTypes(c, ns.empty() ? name : ns + "::" + name);
                    break;
                }
                case Cursor_StructDecl:
                case Cursor_ClassDecl: {
                    const std::string name = clang_.spelling(c);
                    // A forward declaration (`struct Foo;`) gives us nothing to
                    // call, and binding a handle to it would produce functions
                    // no Bee code could ever supply an argument for.
                    if (!name.empty() && clang_.isDefinition(c) && wantedLocation(c) &&
                        wantedNamespace(ns) && wantedName(name) && opts_.bindClasses)
                        knownClasses_.insert(qualify(ns, name));
                    break;
                }
                case Cursor_EnumDecl: {
                    const std::string name = clang_.spelling(c);
                    if (!name.empty() && wantedLocation(c) && wantedNamespace(ns) &&
                        wantedName(name) && opts_.bindEnums)
                        knownEnums_.insert(qualify(ns, name));
                    break;
                }
                default: break;
            }
            return Visit_Continue;
        });
    }

    static std::string qualify(const std::string& ns, const std::string& name) {
        return ns.empty() ? name : ns + "::" + name;
    }

    // ---- pass 2: the declarations themselves -------------------------------
    void collectDecls(CXCursor parent, const std::string& ns) {
        clang_.visit(parent, [&](CXCursor c, CXCursor) {
            const int kind = clang_.kind(c);
            const std::string name = clang_.spelling(c);

            if (kind == Cursor_Namespace) {
                collectDecls(c, ns.empty() ? name : ns + "::" + name);
                return Visit_Continue;
            }
            if (!wantedLocation(c)) return Visit_Continue;

            switch (kind) {
                case Cursor_FunctionDecl:
                    if (wantedNamespace(ns) && wantedName(name)) addFunction(c, ns, name);
                    break;
                case Cursor_StructDecl:
                case Cursor_ClassDecl:
                    if (opts_.bindClasses && wantedNamespace(ns) && wantedName(name) &&
                        !name.empty() && clang_.isDefinition(c))
                        addClass(c, ns, name);
                    break;
                case Cursor_EnumDecl:
                    if (opts_.bindEnums && wantedNamespace(ns) && wantedName(name) &&
                        !name.empty())
                        addEnum(c, ns, name);
                    break;
                case Cursor_FunctionTemplate:
                    skip("template function " + name, "templates need explicit instantiation");
                    break;
                case Cursor_ClassTemplate:
                    skip("template class " + name, "templates need explicit instantiation");
                    break;
                default: break;
            }
            return Visit_Continue;
        });
    }

    // Collect parameters, reporting the first one that can't be mapped.
    bool mapParams(CXCursor c, const std::string& label, std::vector<Param>& out) {
        if (clang_.isVariadic(c)) {
            skip(label, "variadic (...) functions can't be called safely");
            return false;
        }
        const int count = clang_.numArgs(c);
        for (int i = 0; i < count; ++i) {
            CXCursor p = clang_.arg(c, (unsigned)i);
            Param param;
            param.name = clang_.spelling(p);
            if (param.name.empty()) param.name = "a" + std::to_string(i);
            // An initialiser makes this parameter optional. Expression cursor
            // kinds start at 100; anything below that is a type reference.
            clang_.visit(p, [&](CXCursor child, CXCursor) {
                if (clang_.kind(child) >= 100) param.hasDefault = true;
                return Visit_Continue;
            });
            param.type = mapType(clang_.type(p), /*isReturn=*/false);
            if (!param.type.ok()) {
                skip(label, "parameter " + std::to_string(i + 1) + " (" +
                            param.type.cxxType + "): " + param.type.reason);
                return false;
            }
            out.push_back(std::move(param));
        }
        return true;
    }

    // Everything up to the first defaulted parameter must be supplied.
    static size_t requiredCount(const std::vector<Param>& params) {
        for (size_t i = 0; i < params.size(); ++i)
            if (params[i].hasDefault) return i;
        return params.size();
    }

    void addFunction(CXCursor c, const std::string& ns, const std::string& name) {
        const std::string label = "function " + qualify(ns, name);
        if (clang_.isDeleted(c)) { skip(label, "deleted or unavailable"); return; }

        Function fn;
        fn.cxxName = qualify(ns, name);
        fn.header = clang_.fileOf(c);
        fn.line = clang_.lineOf(c);
        fn.result = mapType(clang_.resultType(c), /*isReturn=*/true);
        if (!fn.result.ok()) {
            skip(label, "return type (" + fn.result.cxxType + "): " + fn.result.reason);
            return;
        }
        if (!mapParams(c, label, fn.params)) return;
        fn.minArity = requiredCount(fn.params);
        fn.beeName = uniqueName(name);
        api_.functions.push_back(std::move(fn));
    }

    void addClass(CXCursor cursor, const std::string& ns, const std::string& name) {
        Class cls;
        cls.cxxName = qualify(ns, name);
        cls.beeName = sanitizeIdentifier(name);

        clang_.visit(cursor, [&](CXCursor c, CXCursor) {
            const int kind = clang_.kind(c);
            // Only the public interface: private members aren't ours to call.
            const int access = clang_.access(c);
            if (access != Access_Public && access != Access_Invalid) return Visit_Continue;

            const std::string member = clang_.spelling(c);
            const std::string label = cls.cxxName + "::" + member;

            switch (kind) {
                case Cursor_Constructor: {
                    if (clang_.isDeleted(c)) return Visit_Continue;
                    Function fn;
                    fn.isConstructor = true;
                    fn.owner = cls.cxxName;
                    fn.cxxName = cls.cxxName;
                    if (!mapParams(c, "constructor " + label, fn.params)) return Visit_Continue;
                    fn.minArity = requiredCount(fn.params);
                    fn.beeName = uniqueName(cls.beeName + "_new");
                    cls.constructors.push_back(std::move(fn));
                    break;
                }
                case Cursor_Destructor:
                    if (clang_.isDeleted(c)) cls.destructorDeleted = true;
                    else cls.hasDestructor = true;
                    break;
                case Cursor_CXXMethod: {
                    if (clang_.isDeleted(c)) { skip("method " + label, "deleted"); return Visit_Continue; }
                    if (!wantedName(member)) return Visit_Continue;
                    Function fn;
                    fn.isMethod = true;
                    fn.isStatic = clang_.isStaticMethod(c);
                    fn.owner = cls.cxxName;
                    fn.cxxName = member;
                    fn.header = clang_.fileOf(c);
                    fn.line = clang_.lineOf(c);
                    fn.result = mapType(clang_.resultType(c), true);
                    if (!fn.result.ok()) {
                        skip("method " + label,
                             "return type (" + fn.result.cxxType + "): " + fn.result.reason);
                        return Visit_Continue;
                    }
                    if (!mapParams(c, "method " + label, fn.params)) return Visit_Continue;
                    fn.minArity = requiredCount(fn.params);
                    if (clang_.isPureVirtual(c)) cls.isAbstract = true;
                    fn.beeName = uniqueName(cls.beeName + "_" + member);
                    cls.methods.push_back(std::move(fn));
                    break;
                }
                case Cursor_FieldDecl: {
                    Field f;
                    f.name = member;
                    f.type = mapType(clang_.type(c), true);
                    if (!f.type.ok()) {
                        skip("field " + label, f.type.reason);
                        return Visit_Continue;
                    }
                    // Only plain values are writable; handing back a handle to a
                    // member would outlive its owner too easily.
                    f.writable = f.type.kind != Mapped::Kind::Handle &&
                                 f.type.kind != Mapped::Kind::HandleValue &&
                                 !clang_.isConstQualified(clang_.type(c));
                    cls.fields.push_back(std::move(f));
                    break;
                }
                case Cursor_BaseSpecifier: {
                    // Record the base so a handle for this class is accepted
                    // where the base is wanted.
                    const std::string base = bareName(clang_.typeSpelling(clang_.type(c)));
                    if (knownClasses_.count(base)) cls.bases.push_back(base);
                    else skip("base " + base + " of " + cls.cxxName, "the base class is not bound");
                    break;
                }
                case Cursor_FunctionTemplate:
                    skip("template method " + label, "templates need explicit instantiation");
                    break;
                default: break;
            }
            return Visit_Continue;
        });

        // An abstract class cannot be constructed here at all -- its instances
        // come from a factory function -- so a declared constructor is moot.
        if (cls.isAbstract) cls.constructors.clear();

        // A class with nothing callable is noise in the output.
        if (cls.constructors.empty() && cls.methods.empty() && cls.fields.empty()) {
            skip("class " + cls.cxxName, "no bindable public members");
            return;
        }
        api_.classes.push_back(std::move(cls));
    }

    void addEnum(CXCursor cursor, const std::string& ns, const std::string& name) {
        Enum e;
        e.cxxName = qualify(ns, name);
        e.beeName = sanitizeIdentifier(name);
        clang_.visit(cursor, [&](CXCursor c, CXCursor) {
            if (clang_.kind(c) == Cursor_EnumConstantDecl)
                e.constants.push_back({clang_.spelling(c), clang_.enumValue(c)});
            return Visit_Continue;
        });
        if (e.constants.empty()) {
            skip("enum " + e.cxxName, "no constants");
            return;
        }
        api_.enums.push_back(std::move(e));
    }
};

}  // namespace

std::string sanitizeIdentifier(const std::string& name) {
    std::string out;
    for (char c : name) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '_';
        out += ok ? c : '_';
    }
    if (!out.empty() && out[0] >= '0' && out[0] <= '9') out.insert(out.begin(), '_');

    // A C++ name that happens to be a Bee keyword -- `in`, `class`, `from`
    // are all ordinary parameter names in C++ -- would not parse in generated
    // code, so it gets a trailing underscore.
    static const char* keywords[] = {
        "let", "const", "fn", "return", "if", "else", "while", "for", "in",
        "class", "extends", "this", "super", "new", "static", "import", "from",
        "as", "break", "continue", "try", "catch", "finally", "throw", "match",
        "case", "default", "and", "or", "not", "true", "false", "nil",
    };
    for (const char* kw : keywords)
        if (out == kw) return out + "_";
    return out;
}

bool scan(Clang& clang, const Options& opts, Api& api, std::string& err) {
    (void)err;
    api.headers = opts.headers;
    Scanner scanner(clang, opts, api);
    scanner.run();
    return true;
}

}  // namespace beegen
