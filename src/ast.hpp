#pragma once
#include "value.hpp"
#include "token.hpp"
#include <memory>
#include <vector>
#include <string>

namespace bee {

// ---- Expressions ----
struct Expr {
    enum class Kind {
        Literal, ListLit, DictLit, Variable, Assign,
        Binary, Logical, Unary, Call, Get, Set,
        Index, IndexSet, Slice, This, Super, Grouping,
        Ternary, Function, ListComp
    };
    Kind kind;
    int line = 0;
    explicit Expr(Kind k) : kind(k) {}
    virtual ~Expr() = default;
};
using ExprPtr = std::unique_ptr<Expr>;

struct LiteralExpr : Expr {
    Value value;
    explicit LiteralExpr(Value v) : Expr(Kind::Literal), value(std::move(v)) {}
};

struct ListLitExpr : Expr {
    std::vector<ExprPtr> elements;
    std::vector<bool> spread;   // parallel to elements: true => splice this list in (...x)
    ListLitExpr() : Expr(Kind::ListLit) {}
};

struct DictLitExpr : Expr {
    std::vector<std::pair<ExprPtr, ExprPtr>> entries; // key expr, value expr
    DictLitExpr() : Expr(Kind::DictLit) {}
};

struct VariableExpr : Expr {
    std::string name;
    // Filled in by the resolver: either a slotted local (global=false) or a
    // name-based global lookup (global=true, the default until resolved).
    int depth = 0;
    int slot = -1;
    bool global = true;
    // Inline cache for name-based (global) lookups: when this node is evaluated
    // repeatedly against the same environment (e.g. a top-level loop), the map
    // walk is skipped and the binding is read through a cached pointer.
    Environment* cacheEnv = nullptr;
    Value* cacheSlot = nullptr;
    explicit VariableExpr(std::string n, int ln = 0)
        : Expr(Kind::Variable), name(std::move(n)) { line = ln; }
};

struct AssignExpr : Expr {
    std::string name;
    ExprPtr value;
    int depth = 0;      // resolver-filled, mirrors VariableExpr
    int slot = -1;
    bool global = true;
    // Inline cache for name-based (global) assignment; see VariableExpr.
    Environment* cacheEnv = nullptr;
    Value* cacheSlot = nullptr;
    AssignExpr() : Expr(Kind::Assign) {}
};

struct BinaryExpr : Expr {
    ExprPtr left, right;
    TokenType op;
    BinaryExpr() : Expr(Kind::Binary) {}
};

struct LogicalExpr : Expr {
    ExprPtr left, right;
    TokenType op; // AND or OR
    LogicalExpr() : Expr(Kind::Logical) {}
};

struct UnaryExpr : Expr {
    TokenType op;
    ExprPtr right;
    UnaryExpr() : Expr(Kind::Unary) {}
};

struct CallExpr : Expr {
    ExprPtr callee;
    std::vector<ExprPtr> args;
    std::vector<bool> spread;   // parallel to args: true => spread this arg (...x)
    CallExpr() : Expr(Kind::Call) {}
};

struct GetExpr : Expr {         // object.name
    ExprPtr object;
    std::string name;
    GetExpr() : Expr(Kind::Get) {}
};

struct SetExpr : Expr {         // object.name = value  (op != ASSIGN => compound, e.g. +=)
    ExprPtr object;
    std::string name;
    ExprPtr value;
    TokenType op = TokenType::ASSIGN;
    SetExpr() : Expr(Kind::Set) {}
};

struct IndexExpr : Expr {       // object[index]
    ExprPtr object, index;
    IndexExpr() : Expr(Kind::Index) {}
};

struct SliceExpr : Expr {       // object[start:end]
    ExprPtr object;
    ExprPtr start, end;         // either may be null: [:n], [n:], [:]
    SliceExpr() : Expr(Kind::Slice) {}
};

struct IndexSetExpr : Expr {    // object[index] = value  (op != ASSIGN => compound)
    ExprPtr object, index, value;
    TokenType op = TokenType::ASSIGN;
    IndexSetExpr() : Expr(Kind::IndexSet) {}
};

struct ThisExpr : Expr {
    // Hop count to the method frame where `this` lives (slot 0). -1 => not in a method.
    int depth = -1;
    ThisExpr() : Expr(Kind::This) {}
};

struct SuperExpr : Expr {       // super.method
    std::string method;
    // Hop count to the method frame: `this` is slot 0, the superclass is slot 1.
    int depth = -1;
    SuperExpr() : Expr(Kind::Super) {}
};

struct GroupingExpr : Expr {
    ExprPtr inner;
    GroupingExpr() : Expr(Kind::Grouping) {}
};

struct TernaryExpr : Expr {     // cond ? thenBranch : elseBranch
    ExprPtr cond, thenBranch, elseBranch;
    TernaryExpr() : Expr(Kind::Ternary) {}
};

struct ListCompExpr : Expr {    // [ elem for name in iterable (if cond)? ]
    ExprPtr elem;
    std::string name;
    ExprPtr iterable;
    ExprPtr cond;               // may be null
    int varSlot = 0;
    int slotCount = 0;
    ListCompExpr() : Expr(Kind::ListComp) {}
};

// ---- Statements ----
struct Stmt {
    enum class Kind {
        Expression, Let, Block, If, While, For, ForIn,
        Function, Return, Class, Import, Break, Continue,
        Try, Throw, Match
    };
    Kind kind;
    int line = 0;
    explicit Stmt(Kind k) : kind(k) {}
    virtual ~Stmt() = default;
};
using StmtPtr = std::unique_ptr<Stmt>;

struct ExprStmt : Stmt {
    ExprPtr expr;
    ExprStmt() : Stmt(Kind::Expression) {}
};

struct LetStmt : Stmt {
    std::string name;
    ExprPtr initializer; // may be null
    int slot = -1;       // resolver-filled slot in the current scope
    bool global = true;  // true => define by name in a named (global/module) scope
    bool isConst = false;

    // Destructuring: `let [a, b] = ...` or `let {x, y} = ...`.
    bool isDestructure = false;
    bool destructureDict = false;         // false => list, true => dict
    std::vector<std::string> names;        // targets when destructuring
    std::vector<int> nameSlots;            // resolver-filled slots (parallel to names)
    LetStmt() : Stmt(Kind::Let) {}
};

struct BlockStmt : Stmt {
    std::vector<StmtPtr> statements;
    int slotCount = 0;         // number of slots this block's scope needs
    bool transparent = false;  // declares nothing => runs in the parent env, no allocation
    BlockStmt() : Stmt(Kind::Block) {}
};

struct IfStmt : Stmt {
    ExprPtr condition;
    StmtPtr thenBranch;
    StmtPtr elseBranch; // may be null
    IfStmt() : Stmt(Kind::If) {}
};

struct WhileStmt : Stmt {
    ExprPtr condition;
    StmtPtr body;
    WhileStmt() : Stmt(Kind::While) {}
};

struct ForStmt : Stmt {           // C-style: for (init; cond; incr) body
    StmtPtr init;                 // may be null
    ExprPtr condition;            // may be null (=> true)
    ExprPtr increment;            // may be null
    StmtPtr body;
    int slotCount = 0;            // slots for the loop's own scope (the init var)
    ForStmt() : Stmt(Kind::For) {}
};

struct ForInStmt : Stmt {         // for x in iterable { }
    std::string name;
    ExprPtr iterable;
    StmtPtr body;
    int slotCount = 0;            // slots for the loop's own scope
    int varSlot = 0;             // slot of the loop variable in that scope
    ForInStmt() : Stmt(Kind::ForIn) {}
};

struct FunctionStmt : Stmt {
    std::string name;
    std::vector<std::string> params;
    std::vector<ExprPtr> defaults;  // parallel to params; null where no default
    int restParam = -1;             // index of a `...rest` param, or -1
    std::vector<StmtPtr> body;
    // Resolver-filled frame layout: total slots, and where params begin (after
    // the hidden `this`/`super` slots a method reserves).
    int frameSlots = 0;
    int paramStart = 0;
    // Where this function's own name binds in the enclosing scope.
    int nameSlot = -1;
    bool nameGlobal = true;
    FunctionStmt() : Stmt(Kind::Function) {}
};

struct ReturnStmt : Stmt {
    ExprPtr value; // may be null
    ReturnStmt() : Stmt(Kind::Return) {}
};

// Anonymous function expression: `fn (params) { body }`. Wraps a nameless
// FunctionStmt so it shares the resolver/interpreter machinery.
struct FunctionExpr : Expr {
    std::unique_ptr<FunctionStmt> fn;
    FunctionExpr() : Expr(Kind::Function) {}
};

struct ClassStmt : Stmt {
    std::string name;
    std::string superclassName; // empty if none
    std::vector<std::unique_ptr<FunctionStmt>> methods;
    // Where the class name binds in the enclosing scope.
    int nameSlot = -1;
    bool nameGlobal = true;
    // Resolution of the superclass name reference (when superclassName is set).
    int superDepth = 0;
    int superSlot = -1;
    bool superGlobal = true;
    ClassStmt() : Stmt(Kind::Class) {}
};

struct ImportStmt : Stmt {
    // Forms:
    //   import a.b.c            -> module="a/b/c", bindName="c" (or alias)
    //   import a as x           -> alias="x"
    //   from a import x, y      -> names=[x,y]
    //   from a import *         -> importAll=true
    std::string moduleName;         // dotted path joined by '/'
    std::string alias;              // for `import X as alias`
    std::string bindName;           // default binding name
    std::vector<std::pair<std::string,std::string>> names; // (name, alias) for `from`
    bool isFrom = false;
    bool importAll = false;
    ImportStmt() : Stmt(Kind::Import) {}
};

struct BreakStmt : Stmt { BreakStmt() : Stmt(Kind::Break) {} };
struct ContinueStmt : Stmt { ContinueStmt() : Stmt(Kind::Continue) {} };

struct TryStmt : Stmt {                 // try { } catch (e) { } finally { }
    StmtPtr body;                       // block
    bool hasCatch = false;
    std::string catchName;              // empty => catch without binding
    StmtPtr catchBody;                  // block (when hasCatch)
    bool hasFinally = false;
    StmtPtr finallyBody;                // block (when hasFinally)
    // Resolver-filled scope wrapping the catch binding.
    int catchSlot = 0;
    int catchScopeSlots = 0;
    TryStmt() : Stmt(Kind::Try) {}
};

struct ThrowStmt : Stmt {
    ExprPtr value;
    ThrowStmt() : Stmt(Kind::Throw) {}
};

struct MatchCase {
    std::vector<ExprPtr> values;   // `case v1, v2 { ... }` — match any of these
    StmtPtr body;                  // block
};

struct MatchStmt : Stmt {          // match subject { case ... default ... }
    ExprPtr subject;
    std::vector<MatchCase> cases;
    bool hasDefault = false;
    StmtPtr defaultBody;           // block (when hasDefault)
    MatchStmt() : Stmt(Kind::Match) {}
};

// A parsed program: the top-level statement list of one file.
using Program = std::vector<StmtPtr>;

} // namespace bee
