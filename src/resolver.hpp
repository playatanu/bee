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
        std::map<std::string, TypeAnn> declTypes;   // names given an annotation
        int count = 0;      // next free slot
        int maxCount = 0;   // high-water mark: how many slots the frame needs
    };
    std::vector<Scope> scopes;
    std::set<std::string> globalConsts;
    std::map<std::string, TypeAnn> globalTypes;

    // A merged scope declares into the enclosing frame instead of getting one of
    // its own. Its names are visible only for its extent, so the enclosing
    // scope's name table is saved here and restored on exit. The slot counter is
    // rewound too, letting sibling blocks reuse the same slots -- safe precisely
    // because a merged scope contains no closure that could outlive it.
    struct Mark {
        std::map<std::string, int> names;
        std::set<std::string> consts;
        std::map<std::string, TypeAnn> declTypes;
        int count = 0;
    };
    std::vector<Mark> marks;

    void markConst(const std::string& name);
    bool isConstBinding(const std::string& name);

    void beginScope(bool named) { scopes.push_back(Scope{named, {}, {}, {}, 0, 0}); }
    int endScope() { int c = scopes.back().maxCount; scopes.pop_back(); return c; }
    bool isNamedScope() const { return scopes.back().named; }

    // Can a scope for this subtree be merged into the enclosing frame? Only when
    // there is a slotted frame to merge into, and nothing inside creates a
    // closure -- a function or class defined in the scope would capture the
    // environment, and merging would change which binding it captured.
    bool canMerge(const Stmt* subtree) const;
    void beginMerged();
    void endMerged();

    int declare(const std::string& name);   // returns slot, or -1 in a named scope
    void resolveNameUse(const std::string& name, int& depth, int& slot, bool& global);

    // Remember that `name` was declared with a type, and find it again from a
    // later use. An annotation binds the name for its whole life, so assigning
    // to it has to satisfy the same type its initialiser did.
    void recordType(const std::string& name, const TypeAnn& t);
    TypeAnn declaredTypeOf(const std::string& name) const;

    void resolveStatements(std::vector<StmtPtr>& stmts); // hoist fn names, then resolve
    void resolveStmt(Stmt* s);
    void resolveFunction(FunctionStmt* fn, bool isMethod, bool hasSuper);
    void resolveClass(ClassStmt* c);
    void resolveExpr(Expr* e);
};

} // namespace bee
