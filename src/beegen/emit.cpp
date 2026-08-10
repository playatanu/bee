// Turning the API model into a native module, a Bee wrapper, and a build script.
#include "beegen.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace beegen {

bool writeFile(const std::string& path, const std::string& contents, std::string& err) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        err = "cannot write '" + path + "'";
        return false;
    }
    f << contents;
    if (!f) {
        err = "cannot write '" + path + "'";
        return false;
    }
    return true;
}

namespace {

// ---------------------------------------------------------------------------
// C++ side
// ---------------------------------------------------------------------------
// The expression that turns argument `index` into the C++ value a call needs.
std::string readArg(const Mapped& t, int index, const std::string& who) {
    const std::string a = "a[" + std::to_string(index) + "]";
    const std::string ctx = "\"" + who + "\", " + std::to_string(index);
    switch (t.kind) {
        case Mapped::Kind::Bool:      return "bee::native::boolean(" + a + ", " + ctx + ")";
        case Mapped::Kind::Integer:   return "bee::native::integer<" + t.cxxIntType + ">(" + a + ", " + ctx + ")";
        case Mapped::Kind::Float:     return "bee::native::num(" + a + ", " + ctx + ")";
        case Mapped::Kind::CString:   return "bee::native::str(" + a + ", " + ctx + ").c_str()";
        case Mapped::Kind::StdString: return "bee::native::str(" + a + ", " + ctx + ")";
        case Mapped::Kind::Enum:
            return "(" + t.cxxType + ")bee::native::integer<long long>(" + a + ", " + ctx + ")";
        case Mapped::Kind::Handle:
        case Mapped::Kind::HandleValue: {
            std::string ptr = "bee::native::handle<" + t.handleType + ">(" + a + ", \"" +
                              t.handleType + "\", " + ctx + ")";
            return t.byPointer ? ptr : "*" + ptr;
        }
        case Mapped::Kind::BufferView:
            return "bee::native::bufferView(" + a + ", " + ctx + ")";
        case Mapped::Kind::Vector: {
            // Element conversion is spelled out so the vector's own type does the
            // rest; a list of the wrong element type still reports properly.
            std::string conv;
            switch (t.elemKind) {
                case Mapped::Kind::Bool:
                    conv = "bee::native::boolean(v, w, i)"; break;
                case Mapped::Kind::Integer:
                    conv = "bee::native::integer<" + t.elemCxxType + ">(v, w, i)"; break;
                case Mapped::Kind::Float:
                    conv = "bee::native::num(v, w, i)"; break;
                case Mapped::Kind::StdString:
                case Mapped::Kind::CString:
                    conv = "bee::native::str(v, w, i)"; break;
                default:
                    conv = "bee::Value()"; break;
            }
            const std::string elem = t.elemKind == Mapped::Kind::Float ? "double"
                                   : t.elemKind == Mapped::Kind::Bool ? "bool"
                                   : t.elemKind == Mapped::Kind::StdString ||
                                     t.elemKind == Mapped::Kind::CString ? "std::string"
                                   : t.elemCxxType;
            return "bee::native::toVector<" + elem + ">(" + a + ", " + ctx +
                   ", [](const bee::Value& v, const char* w, int i) { return " + conv + "; })";
        }
        default: return "/* unsupported */";
    }
}

// Wrap a C++ call expression so it becomes a Bee Value.
std::string wrapResult(const Mapped& t, const std::string& call) {
    switch (t.kind) {
        case Mapped::Kind::Void:
            return call + ";\n        return bee::Value();";
        case Mapped::Kind::Bool:
            return "return bee::Value((bool)(" + call + "));";
        case Mapped::Kind::Integer:
        case Mapped::Kind::Float:
            return "return bee::Value((double)(" + call + "));";
        case Mapped::Kind::Enum:
            return "return bee::Value((double)(long long)(" + call + "));";
        case Mapped::Kind::CString: {
            // A null string pointer is nil, not the empty string: they mean
            // different things in every C API.
            std::ostringstream o;
            o << "const char* r = " << call << ";\n"
              << "        return r ? bee::Value(std::string(r)) : bee::Value();";
            return o.str();
        }
        case Mapped::Kind::StdString:
            return "return bee::Value(std::string(" + call + "));";
        case Mapped::Kind::Handle: {
            std::ostringstream o;
            o << "auto* r = " << (t.byPointer ? "" : "&") << "(" << call << ");\n"
              << "        return r ? bee::native::makeHandle(\"" << t.handleType
              << "\", (void*)r) : bee::Value();";
            return o.str();
        }
        case Mapped::Kind::Vector: {
            const bool text = t.elemKind == Mapped::Kind::StdString ||
                              t.elemKind == Mapped::Kind::CString;
            std::string conv = text ? "bee::Value(std::string(x))" : "bee::Value((double)x)";
            return "return bee::native::fromVector(" + call + ", [](const auto& x) { return " +
                   conv + "; });";
        }
        case Mapped::Kind::HandleValue:
            // Returned by value, so Bee gets a heap copy and owns it. The
            // generated wrapper's free() releases it.
            return "return bee::native::makeHandle(\"" + t.handleType + "\", (void*)new " +
                   t.handleType + "(" + call + "));";
        default:
            return "return bee::Value();";
    }
}

std::string argList(const std::vector<Param>& params, const std::string& who, int offset) {
    std::string out;
    for (size_t i = 0; i < params.size(); ++i) {
        if (i) out += ", ";
        out += readArg(params[i].type, (int)i + offset, who);
    }
    return out;
}

void emitFunctionBody(std::ostringstream& o, const Function& fn, int arity, int offset);

// One entry point, taking exactly `take` of the C++ parameters. Everything after
// that is left to the C++ default arguments -- which is why beegen never has to
// read what those defaults actually are.
void emitEntry(std::ostringstream& o, const Function& fn, size_t take,
               const std::string& entryName) {
    const int offset = (fn.isMethod && !fn.isStatic) ? 1 : 0;
    const int arity = (int)take + offset;

    std::vector<Param> params(fn.params.begin(), fn.params.begin() + (long)take);
    Function trimmed = fn;
    trimmed.params = params;
    trimmed.beeName = entryName;
    emitFunctionBody(o, trimmed, arity, offset);
}

void emitFunction(std::ostringstream& o, const Function& fn) {
    // A function with default arguments gets one entry per callable arity; the
    // Bee wrapper picks between them by how many arguments it was given.
    if (fn.minArity < fn.params.size()) {
        for (size_t take = fn.minArity; take <= fn.params.size(); ++take)
            emitEntry(o, fn, take, fn.beeName + "__" + std::to_string(take));
        return;
    }
    const int offset = (fn.isMethod && !fn.isStatic) ? 1 : 0;
    emitFunctionBody(o, fn, (int)fn.params.size() + offset, offset);
}

void emitFunctionBody(std::ostringstream& o, const Function& fn, int arity, int offset) {
    (void)offset;
    o << "    m->def(\"" << fn.beeName << "\", " << arity
      << ", [](bee::Interpreter&, std::vector<bee::Value>& a) -> bee::Value {\n";
    if (arity == 0) o << "        (void)a;\n";

    const int argOffset = (fn.isMethod && !fn.isStatic) ? 1 : 0;
    std::string call;
    if (fn.isConstructor) {
        call = "new " + fn.owner + "(" + argList(fn.params, fn.beeName, 0) + ")";
        o << "        return bee::native::makeHandle(\"" << fn.owner << "\", (void*)" << call
          << ");\n";
    } else if (fn.isMethod && !fn.isStatic) {
        o << "        auto* self = bee::native::handle<" << fn.owner << ">(a[0], \"" << fn.owner
          << "\", \"" << fn.beeName << "\", 0);\n";
        call = "self->" + fn.cxxName + "(" + argList(fn.params, fn.beeName, argOffset) + ")";
        o << "        " << wrapResult(fn.result, call) << "\n";
    } else if (fn.isMethod && fn.isStatic) {
        call = fn.owner + "::" + fn.cxxName + "(" + argList(fn.params, fn.beeName, 0) + ")";
        o << "        " << wrapResult(fn.result, call) << "\n";
    } else {
        call = fn.cxxName + "(" + argList(fn.params, fn.beeName, 0) + ")";
        o << "        " << wrapResult(fn.result, call) << "\n";
    }
    o << "    });\n";
}

void emitClass(std::ostringstream& o, const Class& cls) {
    o << "\n    // ---- " << cls.cxxName << " ----\n";

    // Teach the handle checker that this class may stand in for its bases. The
    // conversion is a real static_cast, so the pointer is adjusted correctly even
    // under multiple inheritance.
    for (auto& base : cls.bases)
        o << "    bee::native::registerUpcast(\"" << cls.cxxName << "\", \"" << base
          << "\", [](void* p) -> void* { return static_cast<" << base << "*>(("
          << cls.cxxName << "*)p); });\n";

    for (auto& ctor : cls.constructors) emitFunction(o, ctor);

    // An abstract class has no `new`: its instances come from a factory function
    // elsewhere in the API, and the wrapper takes that handle as-is.
    if (cls.isAbstract) {
        o << "    // " << cls.cxxName << " is abstract -- no constructor is generated.\n";
    } else if (cls.constructors.empty()) {
        o << "    m->def(\"" << cls.beeName << "_new\", 0, [](bee::Interpreter&, "
          << "std::vector<bee::Value>& a) -> bee::Value {\n"
          << "        (void)a;\n"
          << "        return bee::native::makeHandle(\"" << cls.cxxName << "\", (void*)new "
          << cls.cxxName << "());\n"
          << "    });\n";
    }

    // Deleting through an abstract base is only safe with a virtual destructor,
    // and such APIs supply their own destroy()/release() instead.
    if (!cls.destructorDeleted && !cls.isAbstract) {
        o << "    m->def(\"" << cls.beeName << "_free\", 1, [](bee::Interpreter&, "
          << "std::vector<bee::Value>& a) -> bee::Value {\n"
          << "        delete bee::native::handle<" << cls.cxxName << ">(a[0], \"" << cls.cxxName
          << "\", \"" << cls.beeName << "_free\", 0);\n"
          << "        // Blank the handle so a second free, or a use after it, is\n"
          << "        // reported instead of corrupting the heap.\n"
          << "        if (a[0].isDict()) (*a[0].asDict())[\"__handle\"] = bee::Value(0.0);\n"
          << "        return bee::Value();\n"
          << "    });\n";
    }

    for (auto& fn : cls.methods) emitFunction(o, fn);

    for (auto& f : cls.fields) {
        const std::string getter = cls.beeName + "_get_" + sanitizeIdentifier(f.name);
        o << "    m->def(\"" << getter << "\", 1, [](bee::Interpreter&, "
          << "std::vector<bee::Value>& a) -> bee::Value {\n"
          << "        auto* self = bee::native::handle<" << cls.cxxName << ">(a[0], \""
          << cls.cxxName << "\", \"" << getter << "\", 0);\n"
          << "        " << wrapResult(f.type, "self->" + f.name) << "\n"
          << "    });\n";
        if (!f.writable) continue;
        const std::string setter = cls.beeName + "_set_" + sanitizeIdentifier(f.name);
        o << "    m->def(\"" << setter << "\", 2, [](bee::Interpreter&, "
          << "std::vector<bee::Value>& a) -> bee::Value {\n"
          << "        auto* self = bee::native::handle<" << cls.cxxName << ">(a[0], \""
          << cls.cxxName << "\", \"" << setter << "\", 0);\n"
          << "        self->" << f.name << " = " << readArg(f.type, 1, setter) << ";\n"
          << "        return bee::Value();\n"
          << "    });\n";
    }
}

std::string emitNativeSource(const Options& opts, const Api& api) {
    std::ostringstream o;
    o << "// Generated by beegen -- do not edit.\n"
      << "//\n"
      << "// A native module for Bee, binding:\n";
    for (auto& h : api.headers) o << "//   " << h << "\n";
    o << "//\n"
      << "// Build it with ./build.sh, or see the command inside that file.\n"
      << "#include \"bee_native.hpp\"\n\n";
    for (auto& h : api.headers) o << "#include \"" << h << "\"\n";
    o << "\nextern \"C\" const char* bee_native_abi() { return BEE_NATIVE_ABI; }\n\n"
      << "extern \"C\" int bee_module_init(bee::NativeModule* m) {\n";

    if (!api.enums.empty()) {
        o << "    // ---- enum constants ----\n";
        for (auto& e : api.enums)
            for (auto& c : e.constants)
                o << "    m->constant(\"" << e.beeName << "_" << sanitizeIdentifier(c.name)
                  << "\", bee::Value((double)" << c.value << "));\n";
    }

    if (!api.functions.empty()) {
        o << "\n    // ---- functions ----\n";
        for (auto& fn : api.functions) emitFunction(o, fn);
    }

    for (auto& cls : api.classes) emitClass(o, cls);

    o << "\n    m->constant(\"__module\", bee::Value(std::string(\"" << opts.module << "\")));\n"
      << "    return 0;\n"
      << "}\n";
    return o.str();
}

// ---------------------------------------------------------------------------
// Bee side
// ---------------------------------------------------------------------------
// One Bee function per C++ function. When the C++ side has default
// arguments there are several native entry points, so the wrapper takes varargs
// and dispatches on how many it was given -- which is also how two overloads of
// different arity end up sharing one name.
void emitBeeFunction(std::ostringstream& o, const std::string& nativeName, const Function& fn,
                     const std::string& selfArg) {
    const std::string indent = selfArg.empty() ? "" : "    ";
    const std::string beeName = selfArg.empty() ? fn.beeName : sanitizeIdentifier(fn.cxxName);
    const std::string lead = selfArg.empty() ? "" : selfArg;

    if (fn.minArity == fn.params.size()) {
        std::string params;
        for (size_t i = 0; i < fn.params.size(); ++i)
            params += (i ? ", " : "") + sanitizeIdentifier(fn.params[i].name);
        const std::string args = lead.empty() ? params
                                             : (params.empty() ? lead : lead + ", " + params);
        o << indent << (selfArg.empty() ? "fn " : "") << beeName << "(" << params << ") {\n"
          << indent << "    return " << nativeName << "." << fn.beeName << "(" << args << ")\n"
          << indent << "}\n";
        return;
    }

    // Required parameters stay named, so a missing one is still an arity error;
    // the optional tail arrives as a list.
    std::string required;
    for (size_t i = 0; i < fn.minArity; ++i)
        required += (i ? ", " : "") + sanitizeIdentifier(fn.params[i].name);
    const std::string sep = required.empty() ? "" : ", ";
    o << indent << (selfArg.empty() ? "fn " : "") << beeName << "(" << required << sep
      << "...rest) {\n"
      << indent << "    # " << fn.cxxName << " has default arguments: dispatch on how many\n"
      << indent << "    # were supplied and let C++ fill in the rest.\n";
    for (size_t take = fn.minArity; take <= fn.params.size(); ++take) {
        const size_t extra = take - fn.minArity;
        o << indent << "    " << (take == fn.minArity ? "if" : "} else if")
          << " len(rest) == " << extra << " {\n";
        std::string args = lead;
        for (size_t i = 0; i < fn.minArity; ++i) {
            if (!args.empty()) args += ", ";
            args += sanitizeIdentifier(fn.params[i].name);
        }
        for (size_t i = 0; i < extra; ++i) {
            if (!args.empty()) args += ", ";
            args += "rest[" + std::to_string(i) + "]";
        }
        o << indent << "        return " << nativeName << "." << fn.beeName << "__" << take
          << "(" << args << ")\n";
    }
    o << indent << "    }\n"
      << indent << "    throw \"" << beeName << ": expects between " << fn.minArity << " and "
      << fn.params.size() << " argument(s)\"\n"
      << indent << "}\n";
}

// The wrapper is what users actually import: it turns the flat native functions
// into classes and enum dicts, so the binding reads like hand-written Bee.
std::string emitBeeWrapper(const Options& opts, const Api& api) {
    const std::string nativeName = opts.module + "_native";
    std::ostringstream o;
    o << "# Generated by beegen -- do not edit.\n"
      << "#\n"
      << "# Bee bindings for:\n";
    for (auto& h : api.headers) o << "#   " << h << "\n";
    o << "#\n"
      << "# The flat native functions live in " << nativeName << "; this file wraps\n"
      << "# them in classes and enums. Import this one.\n\n"
      << "import " << nativeName << "\n\n";

    if (!api.enums.empty()) {
        o << "# ---- enums ----\n";
        for (auto& e : api.enums) {
            o << "let " << e.beeName << " = {";
            for (size_t i = 0; i < e.constants.size(); ++i) {
                if (i) o << ", ";
                o << "\"" << e.constants[i].name << "\": " << e.constants[i].value;
            }
            o << "}\n";
        }
        o << "\n";
    }

    if (!api.functions.empty()) {
        o << "# ---- functions ----\n";
        for (auto& fn : api.functions) emitBeeFunction(o, nativeName, fn, "");
        o << "\n";
    }

    for (auto& cls : api.classes) {
        o << "# ---- " << cls.cxxName << " ----\n"
          << "class " << cls.beeName << " {\n";

        // init() takes the first constructor's arguments; the others stay
        // reachable as static-style helpers below.
        const Function* primary = cls.constructors.empty() ? nullptr : &cls.constructors.front();
        std::string ctorParams;
        if (primary)
            for (size_t i = 0; i < primary->params.size(); ++i)
                ctorParams += (i ? ", " : "") + sanitizeIdentifier(primary->params[i].name);

        // Two ways to get an instance, and both matter: construct a new object,
        // or adopt a handle a factory function returned. Library APIs of any size
        // are full of factories (imread, create_engine, CreateSession), so a
        // wrapper that can only construct is a wrapper you can't use.
        o << "    # Construct a new " << cls.cxxName << ", or adopt a handle that a\n"
          << "    # factory function returned.\n"
          << "    init(...args) {\n"
          << "        # _handle is the C++ object; treat it as private.\n"
          << "        if len(args) == 1 and type(args[0]) == \"dict\" {\n"
          << "            this._handle = args[0]\n";

        if (cls.isAbstract) {
            o << "        } else {\n"
              << "            # " << cls.cxxName << " is abstract; only a factory can make one.\n"
              << "            throw \"" << cls.beeName
              << ": expects a handle from a factory function\"\n"
              << "        }\n"
              << "    }\n";
        } else {
            const size_t minA = primary ? primary->minArity : 0;
            const size_t maxA = primary ? primary->params.size() : 0;
            for (size_t take = minA; take <= maxA; ++take) {
                o << "        } else if len(args) == " << take << " {\n";
                std::string args;
                for (size_t i = 0; i < take; ++i)
                    args += (i ? ", " : "") + std::string("args[") + std::to_string(i) + "]";
                std::string entry = primary ? primary->beeName : cls.beeName + "_new";
                if (primary && minA < maxA) entry += "__" + std::to_string(take);
                o << "            this._handle = " << nativeName << "." << entry << "(" << args
                  << ")\n";
            }
            o << "        } else {\n"
              << "            throw \"" << cls.beeName << ": expects "
              << (minA == maxA ? std::to_string(minA)
                               : std::to_string(minA) + " to " + std::to_string(maxA))
              << " argument(s), or a handle\"\n"
              << "        }\n"
              << "    }\n";
        }

        for (auto& fn : cls.methods) {
            // A static C++ method has no instance to hang off, and Bee
            // classes have no static members, so it is emitted as a module-level
            // function below instead.
            if (fn.isStatic) continue;
            o << "\n";
            emitBeeFunction(o, nativeName, fn, "this._handle");
        }

        for (auto& f : cls.fields) {
            const std::string field = sanitizeIdentifier(f.name);
            o << "\n    get_" << field << "() {\n"
              << "        return " << nativeName << "." << cls.beeName << "_get_" << field
              << "(this._handle)\n"
              << "    }\n";
            if (!f.writable) continue;
            o << "\n    set_" << field << "(value) {\n"
              << "        return " << nativeName << "." << cls.beeName << "_set_" << field
              << "(this._handle, value)\n"
              << "    }\n";
        }

        if (!cls.destructorDeleted && !cls.isAbstract) {
            o << "\n    # Release the C++ object. Bee's own values are collected for you,\n"
              << "    # but a handle points at memory only C++ knows about.\n"
              << "    free() {\n"
              << "        if this._handle != nil {\n"
              << "            " << nativeName << "." << cls.beeName << "_free(this._handle)\n"
              << "            this._handle = nil\n"
              << "        }\n"
              << "    }\n";
        }
        o << "}\n";

        // Statics, as free functions named <Class>_<method>.
        bool wroteStatic = false;
        for (auto& fn : cls.methods) {
            if (!fn.isStatic) continue;
            if (!wroteStatic) {
                o << "\n# " << cls.cxxName << "'s static methods: Bee classes have no\n"
                  << "# static members, so these are module-level functions.\n";
                wroteStatic = true;
            }
            std::string params;
            for (size_t i = 0; i < fn.params.size(); ++i)
                params += (i ? ", " : "") + sanitizeIdentifier(fn.params[i].name);
            o << "fn " << fn.beeName << "(" << params << ") {\n"
              << "    return " << nativeName << "." << fn.beeName << "(" << params << ")\n"
              << "}\n";
        }
        o << "\n";
    }

    return o.str();
}

// ---------------------------------------------------------------------------
// Build script and manifest
// ---------------------------------------------------------------------------
std::string emitBuildScript(const Options& opts, const Api& api) {
    std::ostringstream o;
    const std::string src = opts.module + "_native.cpp";
    const std::string beeSrc = opts.beeSrc.empty() ? "/path/to/beelang/src" : opts.beeSrc;

    o << "#!/usr/bin/env bash\n"
      << "# Generated by beegen -- builds the native module for Bee.\n"
      << "#\n"
      << "# A native module is a C++ ABI, so it must be built with the same compiler\n"
      << "# and standard library as the `bee` that will load it. If bee was built by a\n"
      << "# different toolchain, rebuild bee or rebuild this with that toolchain.\n"
      << "set -euo pipefail\n\n"
      << "BEE_SRC=\"${BEE_SRC:-" << beeSrc << "}\"\n"
      << "CXX=\"${CXX:-g++}\"\n\n";
    for (auto& h : api.headers) {
        (void)h;
        break;
    }
    o << "\"$CXX\" -std=c++17 -O2 -fPIC -shared \\\n"
      << "    -I\"$BEE_SRC\" \\\n";
    for (auto& arg : opts.clangArgs) {
        // Include paths matter to the real compile too; other clang flags don't.
        if (arg.rfind("-I", 0) == 0) o << "    " << arg << " \\\n";
    }
    o << "    " << src << " -o " << opts.module << "_native.so\n\n"
      << "echo \"built " << opts.module << "_native.so\"\n"
      << "echo \"try it:  bee -e 'import " << opts.module << "; print(" << opts.module
      << ".__module)'\"\n";
    return o.str();
}

std::string emitManifest(const Options& opts) {
    std::ostringstream o;
    o << "{\n"
      << "  \"name\": \"" << opts.module << "\",\n"
      << "  \"version\": \"0.1.0\",\n"
      << "  \"description\": \"Generated Bee bindings\",\n"
      << "  \"license\": \"MIT\",\n"
      << "  \"main\": \"" << opts.module << ".bee\",\n"
      << "  \"files\": [\"" << opts.module << ".bee\", \"" << opts.module << "_native.so\"],\n"
      << "  \"dependencies\": {}\n"
      << "}\n";
    return o.str();
}

}  // namespace

bool emit(const Options& opts, const Api& api, std::vector<std::string>& written,
          std::string& err) {
    auto out = [&](const std::string& name) {
        return (fs::path(opts.outDir) / name).string();
    };

    struct Output { std::string path, contents; bool skipIfExists; };
    std::vector<Output> outputs = {
        {out(opts.module + "_native.cpp"), emitNativeSource(opts, api), false},
        {out(opts.module + ".bee"), emitBeeWrapper(opts, api), false},
        {out("build.sh"), emitBuildScript(opts, api), false},
    };
    // Never overwrite a manifest someone has edited -- it holds their metadata.
    if (opts.writeManifest) outputs.push_back({out("hive.json"), emitManifest(opts), true});

    for (auto& o : outputs) {
        if (o.skipIfExists && fs::exists(o.path)) continue;
        if (!writeFile(o.path, o.contents, err)) return false;
        written.push_back(o.path);
    }

    std::error_code ec;
    fs::permissions(out("build.sh"),
                    fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                    fs::perm_options::add, ec);
    return true;
}

}  // namespace beegen
