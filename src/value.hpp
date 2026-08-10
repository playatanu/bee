#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <variant>
#include <stdexcept>
#include <cmath>

namespace bee {

// Modulo on Bee's numbers. Bee has only doubles, so `%` used std::fmod -- a
// non-inlined libcall that measured ~3x slower than an integer remainder. For
// integer-valued operands (the common case in loops: `i % 7`, `x % 2`) an
// integer `%` gives a bit-identical result and is far cheaper. Doubles with
// magnitude <= 2^53 are exact integers that fit in int64, so the cast is well
// defined; anything larger, or with a fractional part, falls back to fmod.
// Callers guarantee b != 0 (they report modulo-by-zero themselves), but the
// fast path double-checks before dividing.
inline double beeMod(double a, double b) {
    constexpr double kExactIntLimit = 9007199254740992.0;  // 2^53
    if (a >= -kExactIntLimit && a <= kExactIntLimit &&
        b >= -kExactIntLimit && b <= kExactIntLimit) {
        long long ia = (long long)a, ib = (long long)b;
        if ((double)ia == a && (double)ib == b && ib != 0)
            return (double)(ia % ib);
    }
    return std::fmod(a, b);
}

// Forward declarations
struct FunctionStmt;      // AST node (defined in ast.hpp)
class Environment;
class Interpreter;
struct Value;

using ValueList = std::vector<Value>;
using ValueDict = std::map<std::string, Value>;

// ---------------------------------------------------------------------------
// Buffer -- a contiguous typed array
// ---------------------------------------------------------------------------
// Bee's numbers are doubles and its lists are vectors of 16-byte Values, so
// a 640x480 RGB image held as a list costs ~15 MB and has to be converted
// element by element every time it crosses into C++. A Buffer is the opposite:
// raw bytes with a dtype and a shape, so image and tensor data can be handed to
// a native library by pointer, with no copy at all.
enum class DType { F32, F64, I8, U8, I16, U16, I32, I64 };

struct Buffer {
    DType dtype = DType::F64;
    std::vector<long long> shape;   // e.g. {480, 640, 3}; empty means a scalar
    std::vector<uint8_t> bytes;

    static size_t widthOf(DType t) {
        switch (t) {
            case DType::I8: case DType::U8: return 1;
            case DType::I16: case DType::U16: return 2;
            case DType::F32: case DType::I32: return 4;
            case DType::F64: case DType::I64: return 8;
        }
        return 8;
    }

    size_t itemSize() const { return widthOf(dtype); }
    size_t count() const { return itemSize() ? bytes.size() / itemSize() : 0; }

    // Elements are read and written as doubles, because that is the only number
    // Bee has. The dtype decides how they are stored.
    double get(size_t i) const {
        const void* p = bytes.data() + i * itemSize();
        switch (dtype) {
            case DType::F32: return *(const float*)p;
            case DType::F64: return *(const double*)p;
            case DType::I8:  return *(const int8_t*)p;
            case DType::U8:  return *(const uint8_t*)p;
            case DType::I16: return *(const int16_t*)p;
            case DType::U16: return *(const uint16_t*)p;
            case DType::I32: return *(const int32_t*)p;
            case DType::I64: return (double)*(const int64_t*)p;
        }
        return 0;
    }

    void set(size_t i, double v) {
        void* p = bytes.data() + i * itemSize();
        switch (dtype) {
            case DType::F32: *(float*)p = (float)v; break;
            case DType::F64: *(double*)p = v; break;
            case DType::I8:  *(int8_t*)p = (int8_t)v; break;
            case DType::U8:  *(uint8_t*)p = (uint8_t)v; break;
            case DType::I16: *(int16_t*)p = (int16_t)v; break;
            case DType::U16: *(uint16_t*)p = (uint16_t)v; break;
            case DType::I32: *(int32_t*)p = (int32_t)v; break;
            case DType::I64: *(int64_t*)p = (int64_t)v; break;
        }
    }

    static const char* name(DType t) {
        switch (t) {
            case DType::F32: return "f32";
            case DType::F64: return "f64";
            case DType::I8:  return "i8";
            case DType::U8:  return "u8";
            case DType::I16: return "i16";
            case DType::U16: return "u16";
            case DType::I32: return "i32";
            case DType::I64: return "i64";
        }
        return "?";
    }

    static bool parseName(const std::string& s, DType& out) {
        if (s == "f32" || s == "float32" || s == "float") { out = DType::F32; return true; }
        if (s == "f64" || s == "float64" || s == "double") { out = DType::F64; return true; }
        if (s == "i8"  || s == "int8")   { out = DType::I8;  return true; }
        if (s == "u8"  || s == "uint8")  { out = DType::U8;  return true; }
        if (s == "i16" || s == "int16")  { out = DType::I16; return true; }
        if (s == "u16" || s == "uint16") { out = DType::U16; return true; }
        if (s == "i32" || s == "int32")  { out = DType::I32; return true; }
        if (s == "i64" || s == "int64")  { out = DType::I64; return true; }
        return false;
    }
};

struct Buffer;
struct Function;
struct Class;
struct Instance;
struct Module;
struct Builtin;

// Runtime value: a tagged union of all first-class types.
struct Value {
    using Variant = std::variant<
        std::monostate,                    // nil
        bool,
        double,
        std::shared_ptr<std::string>,
        std::shared_ptr<ValueList>,
        std::shared_ptr<ValueDict>,
        std::shared_ptr<Buffer>,
        std::shared_ptr<Function>,
        std::shared_ptr<Class>,
        std::shared_ptr<Instance>,
        std::shared_ptr<Module>,
        std::shared_ptr<Builtin>
    >;
    Variant data;

    Value() : data(std::monostate{}) {}
    Value(std::nullptr_t) : data(std::monostate{}) {}
    Value(bool b) : data(b) {}
    Value(double d) : data(d) {}
    Value(int i) : data((double)i) {}
    Value(const char* s) : data(std::make_shared<std::string>(s)) {}
    Value(const std::string& s) : data(std::make_shared<std::string>(s)) {}
    Value(std::shared_ptr<std::string> s) : data(std::move(s)) {}
    Value(std::shared_ptr<ValueList> l) : data(std::move(l)) {}
    Value(std::shared_ptr<ValueDict> d) : data(std::move(d)) {}
    Value(std::shared_ptr<Buffer> b) : data(std::move(b)) {}
    Value(std::shared_ptr<Function> f) : data(std::move(f)) {}
    Value(std::shared_ptr<Class> c) : data(std::move(c)) {}
    Value(std::shared_ptr<Instance> i) : data(std::move(i)) {}
    Value(std::shared_ptr<Module> m) : data(std::move(m)) {}
    Value(std::shared_ptr<Builtin> b) : data(std::move(b)) {}

    bool isNil()    const { return std::holds_alternative<std::monostate>(data); }
    bool isBool()   const { return std::holds_alternative<bool>(data); }
    bool isNumber() const { return std::holds_alternative<double>(data); }
    bool isString() const { return std::holds_alternative<std::shared_ptr<std::string>>(data); }
    bool isList()   const { return std::holds_alternative<std::shared_ptr<ValueList>>(data); }
    bool isDict()   const { return std::holds_alternative<std::shared_ptr<ValueDict>>(data); }
    bool isBuffer() const { return std::holds_alternative<std::shared_ptr<Buffer>>(data); }
    bool isFunction() const { return std::holds_alternative<std::shared_ptr<Function>>(data); }
    bool isClass()  const { return std::holds_alternative<std::shared_ptr<Class>>(data); }
    bool isInstance() const { return std::holds_alternative<std::shared_ptr<Instance>>(data); }
    bool isModule() const { return std::holds_alternative<std::shared_ptr<Module>>(data); }
    bool isBuiltin() const { return std::holds_alternative<std::shared_ptr<Builtin>>(data); }
    bool isCallable() const { return isFunction() || isClass() || isBuiltin(); }

    // Borrowing accessors for hot paths (indexing, iteration). The by-value
    // asList()/asDict() below each cost a pair of atomic refcount operations,
    // which is most of the work in `xs[i]`; these hand back a reference to the
    // pointer the Value already holds. Only valid while the Value is alive.
    // Const like shared_ptr's operator*: the Value owns the handle, not the
    // container, so a const Value still hands out a mutable list.
    ValueList& listRef() const { return *std::get<std::shared_ptr<ValueList>>(data); }
    ValueDict& dictRef() const { return *std::get<std::shared_ptr<ValueDict>>(data); }
    Buffer& bufRef() const { return *std::get<std::shared_ptr<Buffer>>(data); }
    // The string handle itself, for the in-place append fast path, which has to
    // ask whether anything else is holding the same buffer.
    const std::shared_ptr<std::string>& strPtr() const {
        return std::get<std::shared_ptr<std::string>>(data);
    }
    // The function handle without a refcount bump, for the VM's direct-call
    // path. Valid only while this Value is alive.
    const std::shared_ptr<Function>& funcRef() const {
        return std::get<std::shared_ptr<Function>>(data);
    }
    const std::shared_ptr<Instance>& instRef() const {
        return std::get<std::shared_ptr<Instance>>(data);
    }

    bool asBool()   const { return std::get<bool>(data); }
    double asNumber() const { return std::get<double>(data); }
    // The number without the alternative check std::get performs. Only for code
    // that already knows this is a number -- the typed opcodes, where a declared
    // annotation was enforced when the value entered.
    double num() const { return *std::get_if<double>(&data); }
    const std::string& asString() const { return *std::get<std::shared_ptr<std::string>>(data); }
    std::shared_ptr<ValueList> asList() const { return std::get<std::shared_ptr<ValueList>>(data); }
    std::shared_ptr<ValueDict> asDict() const { return std::get<std::shared_ptr<ValueDict>>(data); }
    std::shared_ptr<Buffer> asBuffer() const { return std::get<std::shared_ptr<Buffer>>(data); }
    std::shared_ptr<Function> asFunction() const { return std::get<std::shared_ptr<Function>>(data); }
    std::shared_ptr<Class> asClass() const { return std::get<std::shared_ptr<Class>>(data); }
    std::shared_ptr<Instance> asInstance() const { return std::get<std::shared_ptr<Instance>>(data); }
    std::shared_ptr<Module> asModule() const { return std::get<std::shared_ptr<Module>>(data); }
    std::shared_ptr<Builtin> asBuiltin() const { return std::get<std::shared_ptr<Builtin>>(data); }

    // "Truthiness": nil and false are falsey; everything else is truthy.
    bool truthy() const {
        if (isNil()) return false;
        if (isBool()) return asBool();
        return true;
    }
};

// A user-defined function or method.
struct Function {
    const FunctionStmt* decl = nullptr;          // AST for params + body
    // An AOT-compiled body. When set, the function is native code rather than an
    // interpreted AST: callFunction invokes this directly (and `decl` is null).
    // A bound method receives its receiver as the first argument.
    std::function<Value(Interpreter&, std::vector<Value>&)> native;
    std::shared_ptr<Environment> closure;        // captured lexical scope
    std::shared_ptr<Instance> boundThis;         // non-null for bound methods
    std::shared_ptr<Class> definingClass;        // for `super` resolution
    bool isInitializer = false;                  // constructor `init`
    std::string name;
    // The source file this function was written in, shared with every other
    // function from the same file. Stack traces need it: a function can be
    // called from anywhere, so the call site's file says nothing about where
    // the function itself lives.
    std::shared_ptr<const std::string> file;
};

// A class definition.
struct Class {
    std::string name;
    std::shared_ptr<Class> superclass;
    std::map<std::string, std::shared_ptr<Function>> methods;

    std::shared_ptr<Function> findMethod(const std::string& n) const {
        auto it = methods.find(n);
        if (it != methods.end()) return it->second;
        if (superclass) return superclass->findMethod(n);
        return nullptr;
    }
};

// ---------------------------------------------------------------------------
// Shapes (hidden classes)
// ---------------------------------------------------------------------------
// An instance's fields used to live in a std::map<std::string, Value>, so every
// `p.x` was a tree walk with string comparisons -- measured at ~41 ns, which was
// most of the cost of object-oriented code.
//
// A Shape records which field names an instance has and which slot each one
// occupies. Instances that acquired their fields in the same order (which, for
// a class with a normal `init`, means all of them) share a single Shape, so the
// fields themselves are a flat vector and a field access is an array index. A
// call site can then cache "shape S => slot 3" and skip the lookup entirely
// while it keeps seeing the same shape.
struct Shape {
    std::map<std::string, int> index;            // field name -> slot
    std::vector<std::string> names;              // slot -> field name
    std::map<std::string, Shape*> transitions;   // adding this name leads here

    int slotOf(const std::string& n) const {
        auto it = index.find(n);
        return it == index.end() ? -1 : it->second;
    }
    // The shape reached by adding `n` to this one. Cached, so every instance of
    // a class walks the same chain and they all end up sharing one shape.
    Shape* with(const std::string& n);
};

// The shape of an instance with no fields yet; every chain starts here. Shapes
// are allocated for the process's lifetime -- there is one per distinct field
// sequence, which is bounded by the program text.
Shape* emptyShape();

// An instance of a class.
struct Instance {
    std::shared_ptr<Class> klass;
    Shape* shape = emptyShape();
    std::vector<Value> slots;

    // Null when there is no such field. Valid until the instance gains a field.
    Value* field(const std::string& n) {
        int i = shape->slotOf(n);
        return i < 0 ? nullptr : &slots[(size_t)i];
    }
    const Value* field(const std::string& n) const {
        int i = shape->slotOf(n);
        return i < 0 ? nullptr : &slots[(size_t)i];
    }
    void setField(const std::string& n, const Value& v) {
        int i = shape->slotOf(n);
        if (i >= 0) { slots[(size_t)i] = v; return; }
        shape = shape->with(n);          // a new field: move to the next shape
        slots.push_back(v);
    }
};

// An imported module: a named namespace of top-level bindings.
struct Module {
    std::string name;
    std::string path;
    std::shared_ptr<Environment> env;   // module's global scope
};

// A native (C++) function exposed to Bee programs.
struct Builtin {
    std::string name;
    int arity = -1; // -1 means variadic
    std::function<Value(Interpreter&, std::vector<Value>&)> fn;
};

} // namespace bee
