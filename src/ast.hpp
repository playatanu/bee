#pragma once
#include "value.hpp"
#include "token.hpp"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>
#include <string>

namespace bee {

// ---------------------------------------------------------------------------
// Type annotations
// ---------------------------------------------------------------------------
// Bee is gradually typed: annotations are optional, an unannotated binding is
// `Any`, and a program with no annotations at all behaves exactly as before.
// What an annotation buys is a *guarantee* -- checked where the value enters
// (a parameter, an annotated `let`, a `return`) and therefore relied on
// afterwards, which is what lets a compiler stop re-asking what a value is.
//
//   fn dot(a: buffer, b: buffer, n: num) -> num { ... }
//   let total: num = 0
//
struct TypeAnn {
    enum class Kind : uint8_t { Any, Num, Str, Bool, List, Dict, Buffer, Nil, Fn, Class };
    // Sized numeric sub-type. `Dyn` is the plain dynamic `num` (a double); the
    // others are fixed-width integers (two's-complement, wrapping on overflow)
    // and floats. All of them have kind == Num, so everything that already
    // treats a numeric annotation as "a number" keeps working unchanged; the
    // width only steers native storage/arithmetic in the compilers.
    enum class NumTy : uint8_t { Dyn, I8, U8, I16, U16, I32, U32, I64, U64, F16, F32, F64 };
    Kind kind = Kind::Any;
    NumTy num = NumTy::Dyn;   // meaningful only when kind == Num
    std::string className;    // when kind == Class
    int line = 0;

    bool declared() const { return kind != Kind::Any; }
    bool isSizedNum() const { return kind == Kind::Num && num != NumTy::Dyn; }
    static const char* numName(NumTy n) {
        switch (n) {
            case NumTy::I8:  return "i8";   case NumTy::U8:  return "u8";
            case NumTy::I16: return "i16";  case NumTy::U16: return "u16";
            case NumTy::I32: return "i32";  case NumTy::U32: return "u32";
            case NumTy::I64: return "i64";  case NumTy::U64: return "u64";
            case NumTy::F16: return "f16";  case NumTy::F32: return "f32";
            case NumTy::F64: return "f64";  case NumTy::Dyn: break;
        }
        return "num";
    }
    std::string name() const {
        switch (kind) {
            case Kind::Num:    return numName(num);
            case Kind::Str:    return "str";
            case Kind::Bool:   return "bool";
            case Kind::List:   return "list";
            case Kind::Dict:   return "dict";
            case Kind::Buffer: return "buffer";
            case Kind::Nil:    return "nil";
            case Kind::Fn:     return "fn";
            case Kind::Class:  return className;
            case Kind::Any:    break;
        }
        return "any";
    }
    // Coerce a number into a sized numeric type -- the one place all engines
    // (tree-walker, VM, AOT) agree on, so they produce bit-identical results.
    // Integers truncate toward zero then wrap two's-complement into range;
    // floats round to the type's precision. Non-finite -> 0 for integers
    // (a double->intN cast is otherwise undefined). Bee's numbers are doubles,
    // so i64/u64 carry only the ~53 bits a double can represent.
    static double coerce(double x, NumTy t) {
        switch (t) {
            case NumTy::Dyn: case NumTy::F64: return x;
            case NumTy::F32: return (double)(float)x;
            case NumTy::F16: return f16round(x);
            default: break;   // integers
        }
        // Fast path: if x is in int64 range, (int64_t)x truncates toward zero
        // and the narrowing cast does the two's-complement wrap -- no trunc/fmod
        // (those can be libm calls). The range test also rejects NaN and inf
        // (both fail the comparisons), which fall through to the slow path.
        if (x >= -9223372036854775808.0 && x < 9223372036854775808.0) {
            int64_t i = (int64_t)x;
            switch (t) {
                case NumTy::I8:  return (double)(int8_t)i;
                case NumTy::U8:  return (double)(uint8_t)i;
                case NumTy::I16: return (double)(int16_t)i;
                case NumTy::U16: return (double)(uint16_t)i;
                case NumTy::I32: return (double)(int32_t)i;
                case NumTy::U32: return (double)(uint32_t)i;
                case NumTy::I64: return (double)i;
                case NumTy::U64: return (double)(uint64_t)i;
                default: return x;
            }
        }
        // Slow path: |x| >= 2^63, or NaN/inf. Rare, so the libm calls are fine.
        if (!std::isfinite(x)) return 0.0;
        int bits; bool sign;
        switch (t) {
            case NumTy::I8:  bits = 8;  sign = true;  break;
            case NumTy::U8:  bits = 8;  sign = false; break;
            case NumTy::I16: bits = 16; sign = true;  break;
            case NumTy::U16: bits = 16; sign = false; break;
            case NumTy::I32: bits = 32; sign = true;  break;
            case NumTy::U32: bits = 32; sign = false; break;
            case NumTy::I64: bits = 64; sign = true;  break;
            case NumTy::U64: bits = 64; sign = false; break;
            default: return x;
        }
        double m = std::ldexp(1.0, bits);          // 2^bits (exact as a double)
        double r = std::fmod(std::trunc(x), m);    // (-m, m), toward zero
        if (r < 0) r += m;                          // [0, m)
        if (sign && r >= m / 2) r -= m;             // signed range
        return r;
    }
    // Round a double to IEEE-754 binary16 precision (returned as a double).
    // Self-contained so it matches everywhere; overflow saturates to +/-inf.
    static double f16round(double x) {
        float f = (float)x;
        uint32_t b;
        std::memcpy(&b, &f, 4);
        uint32_t sgn = (b >> 16) & 0x8000u;
        int32_t  exp = (int32_t)((b >> 23) & 0xFF) - 127 + 15;
        uint32_t man = b & 0x7FFFFFu;
        float out;
        if (((b >> 23) & 0xFF) == 0xFF) {           // inf / nan
            uint32_t h = sgn | 0x7C00u | (man ? 0x200u : 0);
            return (double)halfToFloat(h);
        }
        if (exp >= 0x1F) return (double)halfToFloat(sgn | 0x7C00u);   // overflow -> inf
        uint32_t h;
        if (exp <= 0) {                              // subnormal / underflow to 0
            if (exp < -10) h = sgn;
            else {
                man |= 0x800000u;
                int shift = 14 - exp;
                uint32_t rounded = (man + (1u << (shift - 1)) - 1 + ((man >> shift) & 1)) >> shift;
                h = sgn | rounded;
            }
        } else {
            uint32_t mant = man >> 13;
            uint32_t round = man & 0x1FFFu;
            h = sgn | ((uint32_t)exp << 10) | mant;
            if (round > 0x1000u || (round == 0x1000u && (mant & 1))) h++;   // round to nearest even
        }
        out = halfToFloat(h);
        return (double)out;
    }
    static float halfToFloat(uint32_t h) {
        uint32_t sgn = (h & 0x8000u) << 16;
        uint32_t exp = (h >> 10) & 0x1F;
        uint32_t man = h & 0x3FFu;
        uint32_t b;
        if (exp == 0) {
            if (man == 0) b = sgn;
            else {                                   // subnormal
                exp = 127 - 15 + 1;
                while (!(man & 0x400u)) { man <<= 1; exp--; }
                man &= 0x3FFu;
                b = sgn | (exp << 23) | (man << 13);
            }
        } else if (exp == 0x1F) {
            b = sgn | 0x7F800000u | (man << 13);
        } else {
            b = sgn | ((exp - 15 + 127) << 23) | (man << 13);
        }
        float f; std::memcpy(&f, &b, 4); return f;
    }

    // Map a sized-numeric type name to its NumTy, or Dyn if not one.
    static NumTy numTyNamed(const std::string& s) {
        if (s == "i8")  return NumTy::I8;
        if (s == "u8")  return NumTy::U8;
        if (s == "i16") return NumTy::I16;
        if (s == "u16") return NumTy::U16;
        if (s == "i32") return NumTy::I32;
        if (s == "u32") return NumTy::U32;
        if (s == "i64") return NumTy::I64;
        if (s == "u64") return NumTy::U64;
        if (s == "f16") return NumTy::F16;
        if (s == "f32") return NumTy::F32;
        if (s == "f64") return NumTy::F64;
        return NumTy::Dyn;
    }
    // The annotation a name spells, or Any if it is not a built-in type name --
    // in which case it is taken to be a class.
    static Kind builtinNamed(const std::string& s) {
        if (s == "num")    return Kind::Num;
        if (numTyNamed(s) != NumTy::Dyn) return Kind::Num;   // i8/u8/.../f32/f64
        if (s == "str")    return Kind::Str;
        if (s == "bool")   return Kind::Bool;
        if (s == "list")   return Kind::List;
        if (s == "dict")   return Kind::Dict;
        if (s == "buffer") return Kind::Buffer;
        if (s == "nil")    return Kind::Nil;
        if (s == "fn")     return Kind::Fn;
        return Kind::Any;   // `any`, or a class name
    }
};

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
    // The type this name was declared with, if any. An annotation has to hold
    // for the variable's whole life, not just its initialiser -- otherwise
    // nothing downstream could rely on it.
    TypeAnn declaredType;
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
    bool ownScope = true;       // false => merged into the enclosing frame
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
    TypeAnn type;        // `let x: num = 0`; Any when unannotated

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

// A scope that declares names normally needs a runtime Environment. It does not
// when the resolver could merge it into the enclosing frame -- see
// Resolver::canMerge(). `ownScope == false` means the declarations live in slots
// of the enclosing frame, so no environment is allocated for this node at all.
struct ForStmt : Stmt {           // C-style: for (init; cond; incr) body
    StmtPtr init;                 // may be null
    ExprPtr condition;            // may be null (=> true)
    ExprPtr increment;            // may be null
    StmtPtr body;
    int slotCount = 0;            // slots for the loop's own scope (the init var)
    bool ownScope = true;         // false => merged into the enclosing frame
    ForStmt() : Stmt(Kind::For) {}
};

struct ForInStmt : Stmt {         // for x in iterable { }
    std::string name;
    ExprPtr iterable;
    StmtPtr body;
    int slotCount = 0;            // slots for the loop's own scope
    int varSlot = 0;             // slot of the loop variable in that scope
    bool ownScope = true;         // false => merged into the enclosing frame
    ForInStmt() : Stmt(Kind::ForIn) {}
};

struct FunctionStmt : Stmt {
    std::string name;
    std::vector<std::string> params;
    std::vector<ExprPtr> defaults;  // parallel to params; null where no default
    std::vector<TypeAnn> paramTypes; // parallel to params; Any where unannotated
    TypeAnn returnType;             // `-> num`; Any when unannotated
    // True when any parameter or the return is annotated, so the common
    // unannotated case skips the per-call check with a single test.
    bool typed = false;
    // True when a sized numeric type (i8..u64, f16/f32/f64) appears in the
    // signature or body. Such a function needs wrapping semantics only the
    // tree-walker implements, so the register VM and the LLVM JIT decline it
    // (see the resolver, which sets this). Keeps the three engines in agreement.
    bool usesSized = false;
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
    bool catchOwnScope = true;     // false => merged into the enclosing frame
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
