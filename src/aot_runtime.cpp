//
// Runtime helpers for AOT-compiled Bee programs. Compiled into libbee_runtime,
// so a `beec`-produced executable links against the same runtime the
// interpreter uses.
//
#include "bee_aot.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace bee {
namespace aot {

Value neg(Interpreter& I, const Value& r, int line) {
    if (!r.isNumber()) I.error("operand of unary '-' must be a number", line);
    return Value(-r.asNumber());
}

Value bnot(Interpreter& I, const Value& r, int line) {
    if (!r.isNumber()) I.error("operand of unary '~' must be a number", line);
    return Value((double)(~(long long)r.asNumber()));
}

std::vector<Value> flatten(Interpreter& I, std::initializer_list<Arg> items, int line) {
    std::vector<Value> out;
    out.reserve(items.size());
    for (const Arg& a : items) {
        if (!a.spread) { out.push_back(a.v); continue; }
        if (a.v.isList()) {
            for (const Value& x : a.v.listRef()) out.push_back(x);
        } else if (a.v.isString()) {
            for (char c : a.v.asString()) out.push_back(Value(std::string(1, c)));
        } else {
            I.error("only a list or string can be spread with '...'", line);
        }
    }
    return out;
}

std::shared_ptr<ValueList> iterate(Interpreter& I, const Value& v, int line) {
    if (v.isList()) return v.asList();   // iterate the live list, like the VM
    auto l = std::make_shared<ValueList>();
    if (v.isString()) {
        for (char c : v.asString()) l->push_back(Value(std::string(1, c)));
        return l;
    }
    if (v.isDict()) {
        for (auto& kv : v.dictRef()) l->push_back(Value(kv.first));
        return l;
    }
    I.error("value is not iterable", line);
    return l;
}

void doThrow(Interpreter& I, const Value& v) {
    (void)I;
    throw BeeThrow{ v, "" };
}

Value makeClass(Interpreter& I, const char* name, const Value& superclass,
                std::initializer_list<MethodDef> methods, int line) {
    auto klass = std::make_shared<Class>();
    klass->name = name;
    if (!superclass.isNil()) {
        if (!superclass.isClass())
            I.error(std::string("superclass of '") + name + "' is not a class", line);
        klass->superclass = superclass.asClass();
    }
    for (const MethodDef& m : methods) {
        auto fn = m.fn.asFunction();          // a makeFn() Function with native set
        fn->name = m.name;
        fn->isInitializer = (std::string(m.name) == "init");
        fn->definingClass = klass;            // drives `super`
        klass->methods[m.name] = fn;
    }
    return Value(klass);
}

Value setProp(Interpreter& I, const Value& object, const char* name, const Value& v, int line) {
    if (!object.isInstance()) I.error("only instances have fields", line);
    object.asInstance()->setField(name, v);
    return v;
}

Value makeModule(Interpreter& I, const char* name, std::shared_ptr<Environment>& envOut) {
    envOut = std::make_shared<Environment>(I.globals);   // chains to the stdlib
    auto mod = std::make_shared<Module>();
    mod->name = name;
    mod->env = envOut;
    return Value(mod);
}

int run(int argc, char** argv, const char* sourceName,
        const std::function<void(Interpreter&)>& body) {
    Interpreter I;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
    I.setScriptArgs(args);
    if (sourceName) I.aotSetCurrentFile(sourceName);   // for error locations/traces

    // Hold the GIL for the whole run; blocking built-ins and spawned threads
    // hand it back and forth. The guard joins outstanding threads and releases
    // the lock however we leave -- exactly as the interpreter's top level does.
    I.gilAcquire();
    struct Guard {
        Interpreter* it;
        ~Guard() { it->joinAllThreads(); it->gilRelease(); }
    } guard{ &I };

    try {
        body(I);
    } catch (BeeThrow& t) {
        std::string msg = "Uncaught: " + I.stringify(t.value);
        if (!t.trace.empty()) msg += "\n" + t.trace;
        std::cerr << msg << "\n";
        return 70;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 70;
    }
    return 0;
}

} // namespace aot
} // namespace bee
