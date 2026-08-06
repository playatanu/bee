#pragma once
#include "ast.hpp"
#include <map>
#include <set>
#include <string>
#include <vector>

namespace bee {

// Static scope-resolution pass. Walks the AST once after parsing and annotates
// every variable reference with a (depth, slot) coordinate, and every scope with
// the number of slots it needs. This turns runtime variable access from a chain
// of hash-map lookups into a couple of pointer hops plus an array index.
//
// The outermost (global / per-module top-level) scope is left "named": its
// declarations resolve by name at runtime, so imports and cross-module access
// keep working. Every nested scope (function frame, block, loop) is slotted.
class Resolver {
public:
    void resolve(Program& program);

private:
    struct Scope {
        bool named = false;
        std::map<std::string, int> names;
        std::set<std::string> consts;
        int count = 0;
    };
    std::vector<Scope> scopes;
    std::set<std::string> globalConsts;

    void markConst(const std::string& name);
    bool isConstBinding(const std::string& name);

    void beginScope(bool named) { scopes.push_back(Scope{named, {}, {}, 0}); }
    int endScope() { int c = scopes.back().count; scopes.pop_back(); return c; }
    bool isNamedScope() const { return scopes.back().named; }

    int declare(const std::string& name);   // returns slot, or -1 in a named scope
    void resolveNameUse(const std::string& name, int& depth, int& slot, bool& global);

    void resolveStatements(std::vector<StmtPtr>& stmts); // hoist fn names, then resolve
    void resolveStmt(Stmt* s);
    void resolveFunction(FunctionStmt* fn, bool isMethod, bool hasSuper);
    void resolveClass(ClassStmt* c);
    void resolveExpr(Expr* e);
};

} // namespace bee
