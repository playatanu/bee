#pragma once
#include "value.hpp"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <stdexcept>

namespace bee {

struct RuntimeError : std::runtime_error {
    explicit RuntimeError(const std::string& msg) : std::runtime_error(msg) {}
};

// A lexical scope.
//
// Nested scopes (function frames, blocks, loops) keep their variables in `slots`
// and are addressed by a resolved (depth, slot) pair, so a lookup is a few
// pointer hops plus an array index -- no hashing, no string compares.
//
// The global scope and each module's top-level scope are "named": their bindings
// live in `values` and are looked up by name. This keeps imports, cross-module
// member access, and dynamically-added builtins working.
class Environment {
public:
    std::shared_ptr<Environment> parent;
    std::vector<Value> slots;
    std::map<std::string, Value> values;

    Environment() = default;
    explicit Environment(std::shared_ptr<Environment> p) : parent(std::move(p)) {}
    Environment(std::shared_ptr<Environment> p, int slotCount)
        : parent(std::move(p)), slots((size_t)slotCount) {}

    // ---- named access (globals, module top-level, imports) ----
    void define(const std::string& name, const Value& v) { values[name] = v; }

    bool tryGetName(const std::string& name, Value& out) {
        for (Environment* e = this; e; e = e->parent.get()) {
            if (!e->values.empty()) {
                auto it = e->values.find(name);
                if (it != e->values.end()) { out = it->second; return true; }
            }
        }
        return false;
    }

    bool assignName(const std::string& name, const Value& v) {
        for (Environment* e = this; e; e = e->parent.get()) {
            if (!e->values.empty()) {
                auto it = e->values.find(name);
                if (it != e->values.end()) { it->second = v; return true; }
            }
        }
        return false;
    }

    // ---- slotted access (resolved locals) ----
    Environment* ancestor(int depth) {
        Environment* e = this;
        while (depth-- > 0) e = e->parent.get();
        return e;
    }
    const Value& getAt(int depth, int slot) { return ancestor(depth)->slots[(size_t)slot]; }
    void setAt(int depth, int slot, const Value& v) { ancestor(depth)->slots[(size_t)slot] = v; }
};

} // namespace bee
