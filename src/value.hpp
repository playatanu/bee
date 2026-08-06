#pragma once
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <variant>
#include <stdexcept>

namespace bee {

// Forward declarations
struct FunctionStmt;      // AST node (defined in ast.hpp)
class Environment;
class Interpreter;
struct Value;

using ValueList = std::vector<Value>;
using ValueDict = std::map<std::string, Value>;

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
    bool isFunction() const { return std::holds_alternative<std::shared_ptr<Function>>(data); }
    bool isClass()  const { return std::holds_alternative<std::shared_ptr<Class>>(data); }
    bool isInstance() const { return std::holds_alternative<std::shared_ptr<Instance>>(data); }
    bool isModule() const { return std::holds_alternative<std::shared_ptr<Module>>(data); }
    bool isBuiltin() const { return std::holds_alternative<std::shared_ptr<Builtin>>(data); }
    bool isCallable() const { return isFunction() || isClass() || isBuiltin(); }

    bool asBool()   const { return std::get<bool>(data); }
    double asNumber() const { return std::get<double>(data); }
    const std::string& asString() const { return *std::get<std::shared_ptr<std::string>>(data); }
    std::shared_ptr<ValueList> asList() const { return std::get<std::shared_ptr<ValueList>>(data); }
    std::shared_ptr<ValueDict> asDict() const { return std::get<std::shared_ptr<ValueDict>>(data); }
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

// An instance of a class.
struct Instance {
    std::shared_ptr<Class> klass;
    std::map<std::string, Value> fields;
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
