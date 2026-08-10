#pragma once
// beegen -- generate Bee bindings from C++ headers.
//
// The pipeline is deliberately two-stage:
//
//   header --[scan]--> API model --[emit]--> <mod>_native.cpp   (a native module)
//                                           <mod>.bee           (an ergonomic wrapper)
//                                           hive.json, build.sh
//
// The native module exposes flat C-like functions -- `Rect_new`, `Rect_area(h)` --
// because that keeps the C++ side simple and the ABI narrow. The generated `.bee`
// wrapper then turns those into real Bee classes and enum dicts, so what a
// user imports looks hand-written. Anything the mapper can't represent is
// skipped *and reported*: silence must never imply coverage.
#include "clang.hpp"

#include <map>
#include <string>
#include <vector>

namespace beegen {

// ---------------------------------------------------------------------------
// Type mapping
// ---------------------------------------------------------------------------
struct Mapped {
    enum class Kind {
        Void,        // -> nil
        Bool,        // -> bool
        Integer,     // -> number, checked for truncation
        Float,       // -> number
        CString,     // const char* -> string (null becomes nil on return)
        StdString,   // std::string / std::string_view -> string
        Enum,        // -> number
        Handle,      // pointer/reference to a bound class -> opaque handle
        HandleValue, // class returned by value -> heap copy behind a handle
        BufferView,  // BeeBuffer -> a Bee buffer, passed by pointer, no copy
        Vector,      // std::vector<T> of a scalar or string -> a list
        Callback,    // a Bee function a native call can invoke
        Unsupported,
    };

    Kind kind = Kind::Unsupported;
    std::string cxxType;     // as spelled in the header
    std::string cxxIntType;  // for Integer: the concrete C++ type to convert to
    std::string handleType;  // for Handle/HandleValue: the class name
    bool byPointer = false;  // Handle: passed as T* rather than T&
    Kind elemKind = Kind::Unsupported;   // Vector: what it holds
    std::string elemCxxType;             // Vector: the element's C++ type
    std::string reason;      // why, when Unsupported

    bool ok() const { return kind != Kind::Unsupported; }
};

// ---------------------------------------------------------------------------
// The API model
// ---------------------------------------------------------------------------
struct Param {
    std::string name;   // may be empty in the header; then a0, a1, ... is used
    Mapped type;
    // C++ default arguments have no Bee equivalent, so instead of trying to
    // read the default *value* we emit one native entry point per arity and let
    // the C++ compiler fill the rest in.
    bool hasDefault = false;
};

struct Function {
    std::string beeName;      // what Bee code calls
    std::string cxxName;      // fully qualified, for the generated call
    std::vector<Param> params;
    Mapped result;
    bool isMethod = false;
    bool isStatic = false;
    bool isConstructor = false;
    bool isDestructor = false;
    size_t minArity = 0;      // params before the first defaulted one
    std::string owner;        // class name, for methods
    std::string header;       // where it was declared
    unsigned line = 0;
};

struct Field {
    std::string name;
    Mapped type;
    bool writable = false;
};

struct Class {
    std::string beeName;
    std::string cxxName;
    std::vector<Function> constructors;
    std::vector<Function> methods;   // instance and static, flattened
    std::vector<Field> fields;
    bool hasDestructor = false;      // a public one we may call
    bool destructorDeleted = false;
    // Abstract classes are never constructed with `new`; a factory function
    // hands back a pointer instead, and the wrapper wraps that handle.
    bool isAbstract = false;
    std::vector<std::string> bases;  // public bases, for handle upcasting
};

struct EnumConstant {
    std::string name;
    long long value = 0;
};

struct Enum {
    std::string beeName;
    std::string cxxName;
    std::vector<EnumConstant> constants;
};

// Something deliberately not bound, and why. Printed at the end of a run so the
// gaps in a generated binding are visible rather than assumed away.
struct Skipped {
    std::string what;    // e.g. "function draw(const Canvas&)"
    std::string reason;
};

struct Api {
    std::vector<Function> functions;
    std::vector<Class> classes;
    std::vector<Enum> enums;
    std::vector<Skipped> skipped;
    std::vector<std::string> headers;   // as given on the command line
};

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------
struct Options {
    std::vector<std::string> headers;
    std::string module;                    // required: the Bee module name
    std::string outDir = ".";
    std::vector<std::string> namespaces;   // if set, only these are bound
    std::vector<std::string> prefixes;     // if set, names must start with one
    std::vector<std::string> skip;         // exact names to leave out
    std::vector<std::string> clangArgs;    // passed through to clang
    std::string libclangPath;              // --libclang
    std::string beeSrc;                    // where bee_native.hpp lives
    bool bindClasses = true;
    bool bindEnums = true;
    bool writeManifest = true;             // emit hive.json
    bool quiet = false;
};

// ---------------------------------------------------------------------------
// Stages
// ---------------------------------------------------------------------------
// Walk the parsed headers and build the model. Anything unmappable lands in
// Api::skipped rather than stopping the run.
bool scan(Clang& clang, const Options& opts, Api& api, std::string& err);

// Write <module>_native.cpp, <module>.bee, build.sh and (optionally) hive.json.
bool emit(const Options& opts, const Api& api, std::vector<std::string>& written,
          std::string& err);

// Shared helpers.
std::string sanitizeIdentifier(const std::string& name);
bool writeFile(const std::string& path, const std::string& contents, std::string& err);

}  // namespace beegen
