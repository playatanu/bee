//
// AOT code generator: a resolved Bee AST -> a C++ translation unit.
//
// The strategy (see bee_aot.hpp): every Bee function becomes a native C++
// lambda wrapped as a callable Value; variables become shared_ptr<Value> cells
// so closures capture them by sharing; and every operation is delegated to the
// existing runtime. Control flow (if/while/for/return/break/continue/try) maps
// straight onto C++ control flow, so the compiled program runs as machine code
// with no interpreter dispatch.
//
#include "aot_codegen.hpp"
#include "token.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace bee {
namespace {

std::string numLit(double d) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.17g", d);
    return std::string(buf);
}

std::string cstr(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '\\': o += "\\\\"; break;
            case '"':  o += "\\\""; break;
            case '\n': o += "\\n";  break;
            case '\t': o += "\\t";  break;
            case '\r': o += "\\r";  break;
            default:
                if (c < 32) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\%03o", c);
                    o += b;
                } else {
                    o += (char)c;
                }
        }
    }
    o += "\"";
    return o;
}

// The name of a binary/relational operator's TokenType, for applyBinary. Only
// operators that reach a BinaryExpr appear (compound assignment is desugared to
// `x = x <op> y` by the parser).
const char* opName(TokenType t) {
    switch (t) {
        case TokenType::PLUS:    return "PLUS";
        case TokenType::MINUS:   return "MINUS";
        case TokenType::STAR:    return "STAR";
        case TokenType::SLASH:   return "SLASH";
        case TokenType::PERCENT: return "PERCENT";
        case TokenType::EQ:      return "EQ";
        case TokenType::NEQ:     return "NEQ";
        case TokenType::LT:      return "LT";
        case TokenType::GT:      return "GT";
        case TokenType::LE:      return "LE";
        case TokenType::GE:      return "GE";
        case TokenType::BIT_AND: return "BIT_AND";
        case TokenType::BIT_OR:  return "BIT_OR";
        case TokenType::BIT_XOR: return "BIT_XOR";
        case TokenType::SHL:     return "SHL";
        case TokenType::SHR:     return "SHR";
        default:                 return nullptr;
    }
}

// ---- Bound-name collection ------------------------------------------------
// To specialise a built-in like `range`, the codegen must be sure the name
// still refers to the built-in. These walkers collect every identifier the
// program binds or assigns -- at any scope, in any module. If `range` never
// appears among them, the global `range` is guaranteed the built-in. This is
// deliberately conservative: any use of the identifier as a target (even a
// local param in an unrelated function) disables the optimisation, which is
// safe -- it never specialises a user-controlled name.
void collectBound(Expr* e, std::set<std::string>& out);
void collectBoundStmt(Stmt* s, std::set<std::string>& out);

void collectBound(Expr* e, std::set<std::string>& out) {
    if (!e) return;
    switch (e->kind) {
        case Expr::Kind::Assign: {
            auto* a = static_cast<AssignExpr*>(e);
            out.insert(a->name);
            collectBound(a->value.get(), out);
            break;
        }
        case Expr::Kind::Binary: {
            auto* b = static_cast<BinaryExpr*>(e);
            collectBound(b->left.get(), out); collectBound(b->right.get(), out); break;
        }
        case Expr::Kind::Logical: {
            auto* b = static_cast<LogicalExpr*>(e);
            collectBound(b->left.get(), out); collectBound(b->right.get(), out); break;
        }
        case Expr::Kind::Unary:   collectBound(static_cast<UnaryExpr*>(e)->right.get(), out); break;
        case Expr::Kind::Grouping:collectBound(static_cast<GroupingExpr*>(e)->inner.get(), out); break;
        case Expr::Kind::Call: {
            auto* c = static_cast<CallExpr*>(e);
            collectBound(c->callee.get(), out);
            for (auto& a : c->args) collectBound(a.get(), out);
            break;
        }
        case Expr::Kind::Get:     collectBound(static_cast<GetExpr*>(e)->object.get(), out); break;
        case Expr::Kind::Set: {
            auto* st = static_cast<SetExpr*>(e);
            collectBound(st->object.get(), out); collectBound(st->value.get(), out); break;
        }
        case Expr::Kind::Index: {
            auto* i = static_cast<IndexExpr*>(e);
            collectBound(i->object.get(), out); collectBound(i->index.get(), out); break;
        }
        case Expr::Kind::IndexSet: {
            auto* i = static_cast<IndexSetExpr*>(e);
            collectBound(i->object.get(), out); collectBound(i->index.get(), out);
            collectBound(i->value.get(), out); break;
        }
        case Expr::Kind::Slice: {
            auto* sl = static_cast<SliceExpr*>(e);
            collectBound(sl->object.get(), out); collectBound(sl->start.get(), out);
            collectBound(sl->end.get(), out); break;
        }
        case Expr::Kind::ListLit:
            for (auto& x : static_cast<ListLitExpr*>(e)->elements) collectBound(x.get(), out);
            break;
        case Expr::Kind::DictLit:
            for (auto& kv : static_cast<DictLitExpr*>(e)->entries) {
                collectBound(kv.first.get(), out); collectBound(kv.second.get(), out);
            }
            break;
        case Expr::Kind::Ternary: {
            auto* t = static_cast<TernaryExpr*>(e);
            collectBound(t->cond.get(), out); collectBound(t->thenBranch.get(), out);
            collectBound(t->elseBranch.get(), out); break;
        }
        case Expr::Kind::Function: {
            auto* fn = static_cast<FunctionExpr*>(e)->fn.get();
            for (auto& p : fn->params) out.insert(p);
            for (auto& d : fn->defaults) if (d) collectBound(d.get(), out);
            for (auto& st : fn->body) collectBoundStmt(st.get(), out);
            break;
        }
        case Expr::Kind::ListComp: {
            auto* lc = static_cast<ListCompExpr*>(e);
            out.insert(lc->name);
            collectBound(lc->elem.get(), out); collectBound(lc->iterable.get(), out);
            collectBound(lc->cond.get(), out); break;
        }
        default: break;   // Literal, Variable, This, Super: nothing bound
    }
}

void collectBoundStmt(Stmt* s, std::set<std::string>& out) {
    if (!s) return;
    switch (s->kind) {
        case Stmt::Kind::Expression: collectBound(static_cast<ExprStmt*>(s)->expr.get(), out); break;
        case Stmt::Kind::Let: {
            auto* l = static_cast<LetStmt*>(s);
            if (l->isDestructure) for (auto& n : l->names) out.insert(n);
            else out.insert(l->name);
            collectBound(l->initializer.get(), out);
            break;
        }
        case Stmt::Kind::Block:
            for (auto& st : static_cast<BlockStmt*>(s)->statements) collectBoundStmt(st.get(), out);
            break;
        case Stmt::Kind::If: {
            auto* i = static_cast<IfStmt*>(s);
            collectBound(i->condition.get(), out);
            collectBoundStmt(i->thenBranch.get(), out); collectBoundStmt(i->elseBranch.get(), out);
            break;
        }
        case Stmt::Kind::While: {
            auto* w = static_cast<WhileStmt*>(s);
            collectBound(w->condition.get(), out); collectBoundStmt(w->body.get(), out); break;
        }
        case Stmt::Kind::For: {
            auto* f = static_cast<ForStmt*>(s);
            collectBoundStmt(f->init.get(), out); collectBound(f->condition.get(), out);
            collectBound(f->increment.get(), out); collectBoundStmt(f->body.get(), out); break;
        }
        case Stmt::Kind::ForIn: {
            auto* f = static_cast<ForInStmt*>(s);
            out.insert(f->name);
            collectBound(f->iterable.get(), out); collectBoundStmt(f->body.get(), out); break;
        }
        case Stmt::Kind::Function: {
            auto* fn = static_cast<FunctionStmt*>(s);
            out.insert(fn->name);
            for (auto& p : fn->params) out.insert(p);
            for (auto& d : fn->defaults) if (d) collectBound(d.get(), out);
            for (auto& st : fn->body) collectBoundStmt(st.get(), out);
            break;
        }
        case Stmt::Kind::Return: collectBound(static_cast<ReturnStmt*>(s)->value.get(), out); break;
        case Stmt::Kind::Throw:  collectBound(static_cast<ThrowStmt*>(s)->value.get(), out); break;
        case Stmt::Kind::Try: {
            auto* t = static_cast<TryStmt*>(s);
            collectBoundStmt(t->body.get(), out);
            if (!t->catchName.empty()) out.insert(t->catchName);
            collectBoundStmt(t->catchBody.get(), out); collectBoundStmt(t->finallyBody.get(), out);
            break;
        }
        case Stmt::Kind::Class: {
            auto* c = static_cast<ClassStmt*>(s);
            out.insert(c->name);
            for (auto& m : c->methods) {
                for (auto& p : m->params) out.insert(p);
                for (auto& d : m->defaults) if (d) collectBound(d.get(), out);
                for (auto& st : m->body) collectBoundStmt(st.get(), out);
            }
            break;
        }
        case Stmt::Kind::Import: {
            auto* im = static_cast<ImportStmt*>(s);
            if (!im->bindName.empty()) out.insert(im->bindName);
            if (!im->alias.empty()) out.insert(im->alias);
            for (auto& pr : im->names) out.insert(pr.second.empty() ? pr.first : pr.second);
            break;
        }
        case Stmt::Kind::Match: {
            auto* m = static_cast<MatchStmt*>(s);
            collectBound(m->subject.get(), out);
            for (auto& c : m->cases) {
                for (auto& v : c.values) collectBound(v.get(), out);
                collectBoundStmt(c.body.get(), out);
            }
            collectBoundStmt(m->defaultBody.get(), out);
            break;
        }
        default: break;   // Break, Continue
    }
}

class Codegen {
public:
    std::vector<AotError>& errors;
    explicit Codegen(std::vector<AotError>& errs) : errors(errs) {}

    std::string run(const Program& program, const std::string& sourceName,
                    const std::vector<AotModule>& modules) {
        active_ = &out_;
        raw("// Generated by beec from " + sourceName + ". Do not edit.\n");
        raw("#include \"bee_aot.hpp\"\n\n");

        // Decide once whether built-ins we specialise are still the built-ins:
        // if the program (or any module) never binds/assigns the name, it is.
        std::set<std::string> bound;
        for (auto& s : program) collectBoundStmt(s.get(), bound);
        for (const auto& m : modules) for (auto& s : *m.program) collectBoundStmt(s.get(), bound);
        rangeIsBuiltin_ = !bound.count("range");

        for (const auto& m : modules) modByName_[m.name] = &m;

        // Forward declarations, so modules can import each other in any order.
        for (const auto& m : modules)
            raw("static bee::Value bee_module_" + m.id + "(bee::Interpreter&);\n");
        if (!modules.empty()) raw("\n");
        for (const auto& m : modules) emitModule(m);

        pushScope(/*global=*/true);   // genv_ stays "(*I.globals)" for the main program
        emit("static void bee_main(bee::Interpreter& I) {");
        indent_++;
        for (auto& s : program) emitStmt(s.get());
        indent_--;
        emit("}");
        popScope();

        raw("\nint main(int argc, char** argv) {\n");
        raw("    return bee::aot::run(argc, argv, " + cstr(sourceName) + ", bee_main);\n");
        raw("}\n");
        return out_.str();
    }

    // Each module becomes a function that builds its namespace once (cached, and
    // registered before its body runs so circular imports resolve).
    void emitModule(const AotModule& m) {
        emit("static bee::Value bee_module_" + m.id + "(bee::Interpreter& I) {");
        indent_++;
        emit("static bee::Value _cached; static bool _done = false;");
        emit("if (_done) return _cached;");
        emit("std::shared_ptr<bee::Environment> _env;");
        emit("_cached = bee::aot::makeModule(I, " + cstr(m.name) + ", _env);");
        emit("_done = true;");
        std::string savedGenv = genv_;
        genv_ = "(*_env)";
        pushScope(/*global=*/true);   // module top level is a named scope -> _env
        for (auto& s : *m.program) emitStmt(s.get());
        popScope();
        genv_ = savedGenv;
        emit("return _cached;");
        indent_--;
        emit("}");
    }

private:
    std::ostringstream out_;
    std::ostringstream* active_ = nullptr;   // current output target
    int indent_ = 0;
    int uid_ = 0;

    // A local binding. Normally a reference-counted cell (shared_ptr<Value>);
    // for a non-escaping sized-numeric `let` it is instead a plain C++ `double`
    // holding the coerced value, which skips the cell/Value indirection in hot
    // loops. `native` selects which; `nty` is the sized type for coercion.
    // `native` marks an unboxed sized-numeric local; `nativeInt` further marks
    // the integer ones (stored as int64_t, wrapped in the integer domain) apart
    // from the float ones (stored as double, coerced via TypeAnn::coerce).
    struct VarInfo { std::string cvar; bool native = false; bool nativeInt = false;
                     TypeAnn::NumTy nty = TypeAnn::NumTy::Dyn; };
    struct Scope { bool global; std::map<std::string, VarInfo> vars; };
    std::vector<Scope> scopes_;
    // True while emitting a function whose body defines no closures, so a native
    // local can't be captured by-value and lose shared mutation. Conservative:
    // any closure anywhere in the function disables native locals for all of it.
    bool nativeLocalsOk_ = false;

    std::string curThisCell_;    // the current method's `this` cell, or "" outside a method
    std::string curSuperName_;   // the current class's superclass name, or "" if none
    TypeAnn curReturnType_;      // the enclosing function's declared return type
    std::string genv_ = "(*I.globals)";   // the named env the current top-level scope binds into
    std::map<std::string, const AotModule*> modByName_;
    bool rangeIsBuiltin_ = true;   // is `range` still the built-in (never user-bound)?

    void pushScope(bool global = false) { scopes_.push_back({global, {}}); }
    void popScope() { scopes_.pop_back(); }
    std::string fresh(const char* hint = "v") { return "_b" + std::string(hint) + std::to_string(uid_++); }

    std::string declare(const std::string& name) {
        Scope& s = scopes_.back();
        if (s.global) return "";
        std::string c = fresh("v");
        s.vars[name] = VarInfo{c, false, false, TypeAnn::NumTy::Dyn};
        return c;
    }
    // Declare a native (unboxed `double`) sized-float local; returns its C var.
    std::string declareNative(const std::string& name, TypeAnn::NumTy nty) {
        Scope& s = scopes_.back();
        std::string c = fresh("n");
        s.vars[name] = VarInfo{c, true, false, nty};
        return c;
    }
    // Declare a native (unboxed `int64_t`) sized-integer local; returns its C var.
    std::string declareNativeInt(const std::string& name, TypeAnn::NumTy nty) {
        Scope& s = scopes_.back();
        std::string c = fresh("n");
        s.vars[name] = VarInfo{c, true, true, nty};
        return c;
    }
    const VarInfo* resolveInfo(const std::string& name) {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it)
            if (!it->global) {
                auto f = it->vars.find(name);
                if (f != it->vars.end()) return &f->second;
            }
        return nullptr;
    }
    std::string resolve(const std::string& name) {
        const VarInfo* v = resolveInfo(name);
        return v ? v->cvar : std::string();
    }
    // The C++ expression yielding the Value bound to `name` (a local cell load,
    // or a global/built-in lookup). A global reference caches its stable slot in
    // a per-site static Value*, so after the first hit it costs a pointer load
    // rather than a std::map string lookup -- the dominant cost in hot loops.
    std::string nameValue(const std::string& name, int line) {
        if (const VarInfo* v = resolveInfo(name)) {
            if (v->native)   // box as a double, matching Bee's numeric Value
                return v->nativeInt ? "bee::Value((double)" + v->cvar + ")"
                                    : "bee::Value(" + v->cvar + ")";
            return "bee::aot::load(" + v->cvar + ")";
        }
        return "([&]()->bee::Value&{ static bee::Value* _s=nullptr; return bee::aot::slotRef(I, " +
               genv_ + ", " + cstr(name) + ", " + std::to_string(line) + ", _s); }())";
    }

    // Try to emit `e` as a native C++ `double` expression (no Value boxing),
    // returning "" if it can't. Covers what a numeric hot loop needs: native
    // locals, number literals, and +/-/* over them (which match Bee's double
    // arithmetic exactly). Division and % are excluded -- they carry Bee's
    // divide-by-zero error and modulo semantics -- as are comparisons (they
    // yield booleans), so those fall back to the boxed path.
    std::string nativeNum(Expr* e) {
        if (!e) return "";
        switch (e->kind) {
            case Expr::Kind::Literal: {
                auto* l = static_cast<LiteralExpr*>(e);
                return l->value.isNumber() ? "(" + numLit(l->value.asNumber()) + ")" : std::string();
            }
            case Expr::Kind::Variable: {
                const VarInfo* v = resolveInfo(static_cast<VariableExpr*>(e)->name);
                if (!v || !v->native) return "";
                return v->nativeInt ? "(double)" + v->cvar : v->cvar;   // ints widen to double
            }
            case Expr::Kind::Grouping:
                return nativeNum(static_cast<GroupingExpr*>(e)->inner.get());
            case Expr::Kind::Unary: {
                auto* u = static_cast<UnaryExpr*>(e);
                if (u->op != TokenType::MINUS) return "";
                std::string r = nativeNum(u->right.get());
                return r.empty() ? "" : "(-" + r + ")";
            }
            case Expr::Kind::Binary: {
                auto* b = static_cast<BinaryExpr*>(e);
                const char* op = b->op == TokenType::PLUS ? "+" :
                                 b->op == TokenType::MINUS ? "-" :
                                 b->op == TokenType::STAR ? "*" : nullptr;
                if (!op) return "";
                std::string l = nativeNum(b->left.get());
                if (l.empty()) return "";
                std::string r = nativeNum(b->right.get());
                if (r.empty()) return "";
                return "(" + l + " " + op + " " + r + ")";
            }
            default: return "";
        }
    }
    // A native `double` for `e`, falling back to unboxing a boxed Value when it
    // isn't natively expressible. Used to feed a native (sized) store.
    std::string asNativeNum(Expr* e) {
        std::string n = nativeNum(e);
        return !n.empty() ? n : "(" + emitExpr(e) + ").asNumber()";
    }

    // Integer sized types stored as a native int64_t working value. u64 is left
    // out (its wrap/ordering don't fit a signed working width cleanly) and keeps
    // the double+coerce path; floats are always double-native.
    static bool isNativeIntTy(TypeAnn::NumTy t) {
        switch (t) {
            case TypeAnn::NumTy::I8:  case TypeAnn::NumTy::U8:
            case TypeAnn::NumTy::I16: case TypeAnn::NumTy::U16:
            case TypeAnn::NumTy::I32: case TypeAnn::NumTy::U32:
            case TypeAnn::NumTy::I64: return true;
            default:                  return false;
        }
    }
    // Wrap an int64_t expression into a sized integer type (two's-complement, via
    // the native narrowing cast), yielding an int64_t. This is the integer-domain
    // equivalent of TypeAnn::coerce -- same result within the ~53-bit range every
    // engine shares, but with no double<->int round trip. I64 needs no narrowing.
    static std::string intCoerce(const std::string& e, TypeAnn::NumTy t) {
        switch (t) {
            case TypeAnn::NumTy::I8:  return "(int64_t)(int8_t)("   + e + ")";
            case TypeAnn::NumTy::U8:  return "(int64_t)(uint8_t)("  + e + ")";
            case TypeAnn::NumTy::I16: return "(int64_t)(int16_t)("  + e + ")";
            case TypeAnn::NumTy::U16: return "(int64_t)(uint16_t)(" + e + ")";
            case TypeAnn::NumTy::I32: return "(int64_t)(int32_t)("  + e + ")";
            case TypeAnn::NumTy::U32: return "(int64_t)(uint32_t)(" + e + ")";
            default:                  return "(" + e + ")";   // I64: identity
        }
    }

    // Try to emit `e` as a native C++ int64_t expression, "" if it can't. Covers
    // integer-native locals, integer-valued literals, and +/-/* over them. The
    // arithmetic is done in uint64_t (well-defined two's-complement wrap, no UB)
    // then read back as int64_t -- bit-identical to the double path wherever a
    // double is exact (the ~53-bit range the language promises for integers).
    std::string nativeIntNum(Expr* e) {
        if (!e) return "";
        switch (e->kind) {
            case Expr::Kind::Literal: {
                auto* l = static_cast<LiteralExpr*>(e);
                if (!l->value.isNumber()) return "";
                double d = l->value.asNumber();
                if (!(d >= -9223372036854775808.0 && d < 9223372036854775808.0)) return "";
                if (d != (double)(int64_t)d) return "";   // fractional -> not integer
                char buf[32];
                std::snprintf(buf, sizeof buf, "%lldLL", (long long)(int64_t)d);
                return "((int64_t)" + std::string(buf) + ")";
            }
            case Expr::Kind::Variable: {
                const VarInfo* v = resolveInfo(static_cast<VariableExpr*>(e)->name);
                return (v && v->native && v->nativeInt) ? v->cvar : std::string();
            }
            case Expr::Kind::Grouping:
                return nativeIntNum(static_cast<GroupingExpr*>(e)->inner.get());
            case Expr::Kind::Unary: {
                auto* u = static_cast<UnaryExpr*>(e);
                if (u->op != TokenType::MINUS) return "";
                std::string r = nativeIntNum(u->right.get());
                return r.empty() ? "" : "((int64_t)(0 - (uint64_t)(" + r + ")))";
            }
            case Expr::Kind::Binary: {
                auto* b = static_cast<BinaryExpr*>(e);
                const char* op = b->op == TokenType::PLUS ? "+" :
                                 b->op == TokenType::MINUS ? "-" :
                                 b->op == TokenType::STAR ? "*" : nullptr;
                if (!op) return "";
                std::string l = nativeIntNum(b->left.get());
                if (l.empty()) return "";
                std::string r = nativeIntNum(b->right.get());
                if (r.empty()) return "";
                return "((int64_t)((uint64_t)(" + l + ") " + op + " (uint64_t)(" + r + ")))";
            }
            default: return "";
        }
    }

    // The native C++ bool for a comparison of two native-double operands `l` and
    // `r`, matching applyBinary on two numbers *exactly* -- NaN included. Bee's
    // LE/GE treat unordered operands as equal (they return true for NaN), unlike
    // native `<=`/`>=`, so those are written as negated strict comparisons.
    static std::string nativeCmpForm(TokenType op, const std::string& l, const std::string& r) {
        switch (op) {
            case TokenType::LT:  return "(" + l + " < "  + r + ")";
            case TokenType::GT:  return "(" + l + " > "  + r + ")";
            case TokenType::LE:  return "(!(" + l + " > " + r + "))";
            case TokenType::GE:  return "(!(" + l + " < " + r + "))";
            case TokenType::EQ:  return "(" + l + " == " + r + ")";
            case TokenType::NEQ: return "(" + l + " != " + r + ")";
            default:             return "";
        }
    }

    // Try to emit `e` as a native C++ `bool` (no Value boxing), returning "" if
    // it can't. Covers the shape of a numeric loop/if condition: comparisons of
    // native-numeric operands, and &&/||/! over such comparisons. Because the
    // operands come from nativeNum (always Bee numbers), the native comparison
    // matches the runtime's number path exactly; see nativeCmpForm for NaN.
    std::string nativeBool(Expr* e) {
        if (!e) return "";
        switch (e->kind) {
            case Expr::Kind::Grouping:
                return nativeBool(static_cast<GroupingExpr*>(e)->inner.get());
            case Expr::Kind::Unary: {
                auto* u = static_cast<UnaryExpr*>(e);
                if (u->op != TokenType::NOT) return "";
                std::string r = nativeBool(u->right.get());
                return r.empty() ? "" : "(!" + r + ")";
            }
            case Expr::Kind::Logical: {
                auto* b = static_cast<LogicalExpr*>(e);
                const char* op = b->op == TokenType::AND ? "&&" :
                                 b->op == TokenType::OR  ? "||" : nullptr;
                if (!op) return "";
                std::string l = nativeBool(b->left.get());
                if (l.empty()) return "";
                std::string r = nativeBool(b->right.get());
                if (r.empty()) return "";
                return "(" + l + " " + op + " " + r + ")";
            }
            case Expr::Kind::Binary: {
                auto* b = static_cast<BinaryExpr*>(e);
                // Both integer-native: compare as int64 (exact, no NaN). Every
                // included type's values fit signed int64 with correct ordering
                // (u8..u32 land as small non-negatives), so a signed compare is
                // right. Otherwise fall back to the double comparison path.
                std::string li = nativeIntNum(b->left.get());
                if (!li.empty()) {
                    std::string ri = nativeIntNum(b->right.get());
                    if (!ri.empty()) return nativeCmpForm(b->op, li, ri);
                }
                std::string l = nativeNum(b->left.get());
                if (l.empty()) return "";
                std::string r = nativeNum(b->right.get());
                if (r.empty()) return "";
                return nativeCmpForm(b->op, l, r);
            }
            default: return "";
        }
    }

    // A C++ bool for a condition: the native form when available, else the boxed
    // Value's truthiness. Used for every if/while/for/ternary test.
    std::string emitCond(Expr* e) {
        std::string nb = nativeBool(e);
        return !nb.empty() ? nb : "(" + emitExpr(e) + ").truthy()";
    }

    void raw(const std::string& s) { (*active_) << s; }
    void emit(const std::string& line) {
        for (int i = 0; i < indent_; ++i) (*active_) << "    ";
        (*active_) << line << "\n";
    }
    std::string indentStr() { std::string s; for (int i = 0; i < indent_; ++i) s += "    "; return s; }
    void err(int line, const std::string& msg) { errors.push_back({line, msg}); }

    // Redirect emit()/raw() into a fresh buffer; returns the previous target.
    std::ostringstream* pushOut() {
        std::ostringstream* prev = active_;
        active_ = new std::ostringstream();
        return prev;
    }
    // Return the redirected text and restore the previous target.
    std::string popOut(std::ostringstream* prev) {
        std::string s = active_->str();
        delete active_;
        active_ = prev;
        return s;
    }

    // ---- expressions (each returns a C++ expression of type bee::Value) ----
    std::string emitExpr(Expr* e) {
        if (!e) return "bee::Value()";
        switch (e->kind) {
            case Expr::Kind::Literal:   return litExpr(static_cast<LiteralExpr*>(e));
            case Expr::Kind::Variable:  return varExpr(static_cast<VariableExpr*>(e));
            case Expr::Kind::Assign:    return assignExpr(static_cast<AssignExpr*>(e));
            case Expr::Kind::Binary:    return binaryExpr(static_cast<BinaryExpr*>(e));
            case Expr::Kind::Logical:   return logicalExpr(static_cast<LogicalExpr*>(e));
            case Expr::Kind::Unary:     return unaryExpr(static_cast<UnaryExpr*>(e));
            case Expr::Kind::Call:      return callExpr(static_cast<CallExpr*>(e));
            case Expr::Kind::Get:       return getExpr(static_cast<GetExpr*>(e));
            case Expr::Kind::Index:     return indexExpr(static_cast<IndexExpr*>(e));
            case Expr::Kind::IndexSet:  return indexSetExpr(static_cast<IndexSetExpr*>(e));
            case Expr::Kind::Slice:     return sliceExpr(static_cast<SliceExpr*>(e));
            case Expr::Kind::ListLit:   return listExpr(static_cast<ListLitExpr*>(e));
            case Expr::Kind::DictLit:   return dictExpr(static_cast<DictLitExpr*>(e));
            case Expr::Kind::Grouping:  return "(" + emitExpr(static_cast<GroupingExpr*>(e)->inner.get()) + ")";
            case Expr::Kind::Ternary:   return ternaryExpr(static_cast<TernaryExpr*>(e));
            case Expr::Kind::Function:  return funcValue(static_cast<FunctionExpr*>(e)->fn.get());
            case Expr::Kind::ListComp:  return listCompExpr(static_cast<ListCompExpr*>(e));
            case Expr::Kind::Set:       return setExpr(static_cast<SetExpr*>(e));
            case Expr::Kind::This:      return thisExpr(static_cast<ThisExpr*>(e));
            case Expr::Kind::Super:     return superExpr(static_cast<SuperExpr*>(e));
        }
        err(e->line, "unsupported expression");
        return "bee::Value()";
    }

    // The C++ enumerator for a sized numeric type, e.g. "bee::TypeAnn::NumTy::I32".
    static std::string numTyEnum(TypeAnn::NumTy t) {
        const char* n = "Dyn";
        switch (t) {
            case TypeAnn::NumTy::I8:  n = "I8";  break; case TypeAnn::NumTy::U8:  n = "U8";  break;
            case TypeAnn::NumTy::I16: n = "I16"; break; case TypeAnn::NumTy::U16: n = "U16"; break;
            case TypeAnn::NumTy::I32: n = "I32"; break; case TypeAnn::NumTy::U32: n = "U32"; break;
            case TypeAnn::NumTy::I64: n = "I64"; break; case TypeAnn::NumTy::U64: n = "U64"; break;
            case TypeAnn::NumTy::F16: n = "F16"; break; case TypeAnn::NumTy::F32: n = "F32"; break;
            case TypeAnn::NumTy::F64: n = "F64"; break; case TypeAnn::NumTy::Dyn: n = "Dyn"; break;
        }
        return std::string("bee::TypeAnn::NumTy::") + n;
    }
    // Wrap a value expression in a sized-numeric coercion if `t` is one, else
    // return it unchanged.
    std::string coerceIfSized(const TypeAnn& t, const std::string& valExpr) {
        if (!t.isSizedNum()) return valExpr;
        return "bee::aot::coerceNum((" + valExpr + "), " + numTyEnum(t.num) + ")";
    }

    std::string litExpr(LiteralExpr* e) {
        const Value& v = e->value;
        if (v.isString()) return "bee::Value(std::string(" + cstr(v.asString()) + "))";
        if (v.isNumber()) return "bee::Value((double)(" + numLit(v.asNumber()) + "))";
        if (v.isBool())   return v.asBool() ? "bee::Value(true)" : "bee::Value(false)";
        if (v.isNil())    return "bee::Value()";
        err(e->line, "unsupported literal");
        return "bee::Value()";
    }

    std::string varExpr(VariableExpr* e) {
        return nameValue(e->name, e->line);   // local cell, else the current named env
    }

    std::string assignExpr(AssignExpr* e) {
        // Native (unboxed) sized local: store the coerced value, yielding the
        // boxed value so `x = ...` still works as an expression.
        if (const VarInfo* v = resolveInfo(e->name); v && v->native) {
            if (v->nativeInt) {
                // Integer store: prefer a native-int RHS (wrapped in the integer
                // domain -- the hot `i += 1` becomes a plain int64 add); else a
                // double RHS (e.g. `x = x / 2`) coerced once and truncated in.
                std::string iv = nativeIntNum(e->value.get());
                std::string rhs = !iv.empty()
                    ? intCoerce(iv, v->nty)
                    : "(int64_t)bee::TypeAnn::coerce((" + asNativeNum(e->value.get()) + "), " +
                      numTyEnum(v->nty) + ")";
                return "([&]{ " + v->cvar + " = " + rhs + "; return bee::Value((double)" +
                       v->cvar + "); }())";
            }
            return "([&]{ " + v->cvar + " = bee::TypeAnn::coerce((" + asNativeNum(e->value.get()) +
                   "), " + numTyEnum(v->nty) + "); return bee::Value(" + v->cvar + "); }())";
        }
        std::string val = coerceIfSized(e->declaredType, emitExpr(e->value.get()));
        std::string c = resolve(e->name);
        if (!c.empty()) return "bee::aot::store(" + c + ", (" + val + "))";
        // Assign through the cached global slot (see nameValue). The assignment
        // yields the stored Value, matching assignIn's return.
        return "([&]()->bee::Value{ static bee::Value* _s=nullptr; return (bee::aot::slotRef(I, " +
               genv_ + ", " + cstr(e->name) + ", " + std::to_string(e->line) + ", _s) = (" + val +
               ")); }())";
    }

    std::string binaryExpr(BinaryExpr* e) {
        // Fast paths, safe only when both operands are statically native numbers:
        // a comparison becomes a native C++ bool, arithmetic a native double --
        // each boxed once, with no runtime operator dispatch. Both match the
        // runtime's two-number path exactly (see nativeCmpForm / nativeNum).
        if (std::string nb = nativeBool(e); !nb.empty())
            return "bee::Value(" + nb + ")";
        if (std::string ni = nativeIntNum(e); !ni.empty())
            return "bee::Value((double)(" + ni + "))";
        if (std::string nn = nativeNum(e); !nn.empty())
            return "bee::Value((double)(" + nn + "))";
        const char* op = opName(e->op);
        if (!op) { err(e->line, "unsupported binary operator"); return "bee::Value()"; }
        std::string l = emitExpr(e->left.get());
        std::string r = emitExpr(e->right.get());
        return "([&]{ bee::Value _l=(" + l + "); bee::Value _r=(" + r +
               "); return bee::aot::binary(I, bee::TokenType::" + op + ", _l, _r, " +
               std::to_string(e->line) + "); }())";
    }

    std::string logicalExpr(LogicalExpr* e) {
        std::string l = emitExpr(e->left.get());
        std::string r = emitExpr(e->right.get());
        if (e->op == TokenType::OR)
            return "([&]{ bee::Value _l=(" + l + "); return _l.truthy() ? _l : (" + r + "); }())";
        return "([&]{ bee::Value _l=(" + l + "); return _l.truthy() ? (" + r + ") : _l; }())";
    }

    std::string unaryExpr(UnaryExpr* e) {
        std::string r = emitExpr(e->right.get());
        int ln = e->line;
        if (e->op == TokenType::MINUS)   return "bee::aot::neg(I, (" + r + "), " + std::to_string(ln) + ")";
        if (e->op == TokenType::BIT_NOT) return "bee::aot::bnot(I, (" + r + "), " + std::to_string(ln) + ")";
        if (e->op == TokenType::NOT)     return "bee::aot::lnot(" + r + ")";
        err(ln, "unsupported unary operator");
        return "bee::Value()";
    }

    std::string argList(const std::vector<ExprPtr>& args, const std::vector<bool>& spread) {
        std::string s = "{";
        for (size_t i = 0; i < args.size(); ++i) {
            if (i) s += ", ";
            bool sp = i < spread.size() && spread[i];
            s += "{ (" + emitExpr(args[i].get()) + "), " + (sp ? "true" : "false") + " }";
        }
        s += "}";
        return s;
    }

    std::string callExpr(CallExpr* e) {
        std::string callee = emitExpr(e->callee.get());
        std::string args = argList(e->args, e->spread);
        int ln = e->line;
        return "([&]{ bee::Value _c=(" + callee + "); std::vector<bee::Value> _a=bee::aot::flatten(I, " +
               args + ", " + std::to_string(ln) + "); return bee::aot::call(I, _c, std::move(_a), " +
               std::to_string(ln) + "); }())";
    }

    std::string getExpr(GetExpr* e) {
        std::string o = emitExpr(e->object.get());
        return "bee::aot::getProp(I, (" + o + "), " + cstr(e->name) + ", " + std::to_string(e->line) + ")";
    }

    std::string thisExpr(ThisExpr* e) {
        if (curThisCell_.empty()) { err(e->line, "'this' used outside of a method"); return "bee::Value()"; }
        return "bee::aot::load(" + curThisCell_ + ")";
    }

    std::string superExpr(SuperExpr* e) {
        if (curThisCell_.empty() || curSuperName_.empty()) {
            err(e->line, "'super' used outside of a subclass method");
            return "bee::Value()";
        }
        // `super.m` -> the method m of this class's superclass, bound to `this`.
        return "I.superMethod(" + nameValue(curSuperName_, e->line) + ", bee::aot::load(" +
               curThisCell_ + "), " + cstr(e->method) + ", " + std::to_string(e->line) + ")";
    }

    std::string setExpr(SetExpr* e) {
        std::string o = emitExpr(e->object.get());
        std::string v = emitExpr(e->value.get());
        int ln = e->line;
        if (e->op == TokenType::ASSIGN)
            return "([&]{ bee::Value _o=(" + o + "); bee::Value _v=(" + v +
                   "); return bee::aot::setProp(I, _o, " + cstr(e->name) + ", _v, " +
                   std::to_string(ln) + "); }())";
        const char* op = opName(e->op);
        if (!op) { err(ln, "unsupported compound property assignment"); op = "PLUS"; }
        return "([&]{ bee::Value _o=(" + o + "); bee::Value _v=(" + v +
               "); bee::Value _nv=I.applyBinaryArith(bee::TokenType::" + std::string(op) +
               ", I.getProperty(_o, " + cstr(e->name) + ", " + std::to_string(ln) + "), _v, " +
               std::to_string(ln) + "); return bee::aot::setProp(I, _o, " + cstr(e->name) + ", _nv, " +
               std::to_string(ln) + "); }())";
    }

    std::string indexExpr(IndexExpr* e) {
        std::string o = emitExpr(e->object.get());
        std::string i = emitExpr(e->index.get());
        return "([&]{ bee::Value _o=(" + o + "); bee::Value _i=(" + i +
               "); return bee::aot::index(I, _o, _i, " + std::to_string(e->line) + "); }())";
    }

    std::string indexSetExpr(IndexSetExpr* e) {
        std::string o = emitExpr(e->object.get());
        std::string i = emitExpr(e->index.get());
        std::string v = emitExpr(e->value.get());
        int ln = e->line;
        std::string nv;
        if (e->op == TokenType::ASSIGN) {
            nv = "_rhs";
        } else {
            const char* op = opName(e->op);
            if (!op) { err(ln, "unsupported compound index assignment"); op = "PLUS"; }
            nv = "I.applyBinaryArith(bee::TokenType::" + std::string(op) +
                 ", I.indexGet(_o, _i, " + std::to_string(ln) + "), _rhs, " + std::to_string(ln) + ")";
        }
        return "([&]{ bee::Value _o=(" + o + "); bee::Value _i=(" + i + "); bee::Value _rhs=(" + v +
               "); bee::Value _nv=(" + nv + "); bee::aot::indexSet(I, _o, _i, _nv, " +
               std::to_string(ln) + "); return _nv; }())";
    }

    std::string sliceExpr(SliceExpr* e) {
        std::string o = emitExpr(e->object.get());
        std::string a = e->start ? emitExpr(e->start.get()) : std::string("bee::Value()");
        std::string b = e->end ? emitExpr(e->end.get()) : std::string("bee::Value()");
        return "([&]{ bee::Value _o=(" + o + "); bee::Value _a=(" + a + "); bee::Value _b=(" + b +
               "); return bee::aot::slice(I, _o, _a, _b, " + std::to_string(e->line) + "); }())";
    }

    std::string listExpr(ListLitExpr* e) {
        return "bee::aot::list(I, " + argList(e->elements, e->spread) + ", " +
               std::to_string(e->line) + ")";
    }

    std::string dictExpr(DictLitExpr* e) {
        std::string s = "bee::aot::dict(I, {";
        for (size_t i = 0; i < e->entries.size(); ++i) {
            if (i) s += ", ";
            s += "{ (" + emitExpr(e->entries[i].first.get()) + "), (" +
                 emitExpr(e->entries[i].second.get()) + ") }";
        }
        s += "})";
        return s;
    }

    std::string ternaryExpr(TernaryExpr* e) {
        std::string c = emitCond(e->cond.get());
        std::string t = emitExpr(e->thenBranch.get());
        std::string f = emitExpr(e->elseBranch.get());
        return "([&]{ return (" + c + ") ? (" + t + ") : (" + f + "); }())";
    }

    std::string listCompExpr(ListCompExpr* e) {
        std::string it = emitExpr(e->iterable.get());
        int ln = e->line;
        std::ostringstream s;
        s << "([&]{ auto _acc=std::make_shared<bee::ValueList>(); "
          << "auto _seq=bee::aot::iterate(I, (" << it << "), " << ln << "); ";
        pushScope();
        std::string cvar = declare(e->name);
        std::string idx = fresh("i");
        s << "auto " << cvar << "=bee::aot::cell(); ";
        s << "for (size_t " << idx << "=0; " << idx << "<_seq->size(); ++" << idx << ") { *"
          << cvar << "=(*_seq)[" << idx << "]; ";
        std::string elem = emitExpr(e->elem.get());
        if (e->cond) {
            std::string cond = emitExpr(e->cond.get());
            s << "if ((" << cond << ").truthy()) _acc->push_back(" << elem << "); ";
        } else {
            s << "_acc->push_back(" << elem << "); ";
        }
        popScope();
        s << "} return bee::Value(_acc); }())";
        return s.str();
    }

    // A function body as a callable Value. Anonymous functions pass name "". A
    // method receives its receiver as args[0]; the parameters then start at
    // args[1], and `this` is bound to a cell the body reads via ThisExpr.
    std::string funcValue(FunctionStmt* fn, bool isMethod = false) {
        std::ostringstream* prev = pushOut();
        int savedIndent = indent_;
        indent_ = savedIndent + 1;
        std::string savedThis = curThisCell_;
        TypeAnn savedRet = curReturnType_;
        curReturnType_ = fn->returnType;
        bool savedNative = nativeLocalsOk_;
        // Native locals are safe only when nothing here can capture them by value.
        nativeLocalsOk_ = !fn->hasNestedFn;
        pushScope();   // function scope: params + locals are cells

        int off = isMethod ? 1 : 0;
        if (isMethod) {
            std::string tc = fresh("this");
            emit("auto " + tc + " = bee::aot::cell(args.size() > 0 ? args[0] : bee::Value());");
            curThisCell_ = tc;
        }

        int nparams = (int)fn->params.size();
        int required = 0;
        for (int i = 0; i < nparams; ++i) {
            if (i == fn->restParam) continue;
            bool hasDefault = i < (int)fn->defaults.size() && fn->defaults[i];
            if (!hasDefault) required++;
        }
        std::string nm = fn->name.empty() ? std::string("fn") : fn->name;
        emit("if ((int)args.size() < " + std::to_string(required + off) + ") I.error(" +
             cstr(nm + " expects at least " + std::to_string(required) + " argument(s)") + ", " +
             std::to_string(fn->line) + ");");
        if (fn->restParam < 0)
            emit("if ((int)args.size() > " + std::to_string(nparams + off) + ") I.error(" +
                 cstr(nm + " expects at most " + std::to_string(nparams) + " argument(s)") + ", " +
                 std::to_string(fn->line) + ");");

        for (int i = 0; i < nparams; ++i) {
            int ai = i + off;   // argument index for parameter i
            if (i == fn->restParam) {
                std::string c = declare(fn->params[i]);
                emit("auto " + c + " = bee::aot::cell(bee::Value(std::make_shared<bee::ValueList>("
                     "std::vector<bee::Value>(args.begin() + std::min((size_t)" + std::to_string(ai) +
                     ", args.size()), args.end()))));");
                continue;
            }
            std::string dflt = (i < (int)fn->defaults.size() && fn->defaults[i])
                                   ? emitExpr(fn->defaults[i].get())
                                   : std::string("bee::Value()");
            std::string c = declare(fn->params[i]);
            std::string pinit = "(int)args.size() > " + std::to_string(ai) + " ? args[" +
                                std::to_string(ai) + "] : (" + dflt + ")";
            if (i < (int)fn->paramTypes.size())   // `fn f(x: i8)` wraps the bound argument
                pinit = coerceIfSized(fn->paramTypes[i], "(" + pinit + ")");
            emit("auto " + c + " = bee::aot::cell((" + pinit + "));");
        }

        for (auto& st : fn->body) emitStmt(st.get());
        emit("return bee::Value();");

        popScope();
        indent_ = savedIndent;
        curThisCell_ = savedThis;
        curReturnType_ = savedRet;
        nativeLocalsOk_ = savedNative;
        std::string body = popOut(prev);
        return "bee::aot::makeFn(" + cstr(fn->name) +
               ", [=](bee::Interpreter& I, std::vector<bee::Value>& args) -> bee::Value {\n" +
               body + indentStr() + "})";
    }

    // ---- statements -------------------------------------------------------
    void emitStmt(Stmt* s) {
        switch (s->kind) {
            case Stmt::Kind::Expression: emit(emitExpr(static_cast<ExprStmt*>(s)->expr.get()) + ";"); break;
            case Stmt::Kind::Let:        letStmt(static_cast<LetStmt*>(s)); break;
            case Stmt::Kind::Block:      blockStmt(static_cast<BlockStmt*>(s)); break;
            case Stmt::Kind::If:         ifStmt(static_cast<IfStmt*>(s)); break;
            case Stmt::Kind::While:      whileStmt(static_cast<WhileStmt*>(s)); break;
            case Stmt::Kind::For:        forStmt(static_cast<ForStmt*>(s)); break;
            case Stmt::Kind::ForIn:      forInStmt(static_cast<ForInStmt*>(s)); break;
            case Stmt::Kind::Function:   funcStmt(static_cast<FunctionStmt*>(s)); break;
            case Stmt::Kind::Return:     returnStmt(static_cast<ReturnStmt*>(s)); break;
            case Stmt::Kind::Break:      emit("break;"); break;
            case Stmt::Kind::Continue:   emit("continue;"); break;
            case Stmt::Kind::Throw:      emit("bee::aot::doThrow(I, (" + emitExpr(static_cast<ThrowStmt*>(s)->value.get()) + "));"); break;
            case Stmt::Kind::Try:        tryStmt(static_cast<TryStmt*>(s)); break;
            case Stmt::Kind::Class:      classStmt(static_cast<ClassStmt*>(s)); break;
            case Stmt::Kind::Import:     importStmt(static_cast<ImportStmt*>(s)); break;
            case Stmt::Kind::Match:      matchStmt(static_cast<MatchStmt*>(s)); break;
        }
    }

    void letStmt(LetStmt* s) {
        if (s->isDestructure) { destructureLet(s); return; }
        // A non-escaping sized-numeric local becomes a native scalar (coerced to
        // its type on write): no cell/Value indirection in the hot path. Integer
        // types use an int64_t working value (integer-domain wrap, so arithmetic
        // and comparisons stay native ints); floats use a double.
        if (!scopes_.back().global && nativeLocalsOk_ && s->type.isSizedNum()) {
            if (isNativeIntTy(s->type.num)) {
                std::string iv = s->initializer ? nativeIntNum(s->initializer.get()) : std::string();
                std::string init = !iv.empty()
                    ? intCoerce(iv, s->type.num)
                    : (s->initializer
                        ? "(int64_t)bee::TypeAnn::coerce((" + asNativeNum(s->initializer.get()) + "), " +
                          numTyEnum(s->type.num) + ")"
                        : std::string("0"));
                std::string c = declareNativeInt(s->name, s->type.num);   // after init (no self-ref)
                emit("int64_t " + c + " = " + init + ";");
                return;
            }
            std::string initD = s->initializer ? asNativeNum(s->initializer.get()) : std::string("0.0");
            std::string c = declareNative(s->name, s->type.num);   // after initD (no self-ref)
            emit("double " + c + " = bee::TypeAnn::coerce((" + initD + "), " + numTyEnum(s->type.num) + ");");
            return;
        }
        std::string init = s->initializer ? emitExpr(s->initializer.get()) : std::string("bee::Value()");
        init = coerceIfSized(s->type, init);   // `let x: i8 = ...` wraps into range
        if (scopes_.back().global) {
            emit("bee::aot::defineIn(" + genv_ + ", " + cstr(s->name) + ", (" + init + "));");
        } else {
            std::string c = declare(s->name);
            emit("auto " + c + " = bee::aot::cell((" + init + "));");
        }
    }

    void blockStmt(BlockStmt* s) {
        emit("{");
        indent_++;
        pushScope();
        for (auto& st : s->statements) emitStmt(st.get());
        popScope();
        indent_--;
        emit("}");
    }

    void ifStmt(IfStmt* s) {
        emit("if (" + emitCond(s->condition.get()) + ") {");
        indent_++; pushScope(); emitStmt(s->thenBranch.get()); popScope(); indent_--;
        if (s->elseBranch) {
            emit("} else {");
            indent_++; pushScope(); emitStmt(s->elseBranch.get()); popScope(); indent_--;
        }
        emit("}");
    }

    void whileStmt(WhileStmt* s) {
        emit("while (" + emitCond(s->condition.get()) + ") {");
        indent_++; pushScope(); emitStmt(s->body.get()); popScope(); indent_--;
        emit("}");
    }

    void forStmt(ForStmt* s) {
        // The increment goes in a real C++ for-increment clause, not at the end
        // of the body -- otherwise a `continue` in the body would skip it.
        emit("{");
        indent_++;
        pushScope();
        if (s->init) emitStmt(s->init.get());   // declares the loop var cell
        std::string cond = s->condition ? emitCond(s->condition.get()) : std::string("true");
        std::string incr = s->increment ? ("(void)(" + emitExpr(s->increment.get()) + ")") : std::string();
        emit("for (; " + cond + "; " + incr + ") {");
        indent_++; pushScope(); emitStmt(s->body.get()); popScope(); indent_--;
        emit("}");
        popScope();
        indent_--;
        emit("}");
    }

    // `for x in range(a[,b[,step]])` with the built-in range, if any: return the
    // call, else nullptr. A local `range` in scope, a user-bound `range`, spread
    // args, or the wrong arity all decline.
    CallExpr* rangeCall(Expr* e) {
        if (!rangeIsBuiltin_ || e->kind != Expr::Kind::Call) return nullptr;
        auto* c = static_cast<CallExpr*>(e);
        if (c->callee->kind != Expr::Kind::Variable) return nullptr;
        if (static_cast<VariableExpr*>(c->callee.get())->name != "range") return nullptr;
        if (!resolve("range").empty()) return nullptr;              // shadowed by a local here
        if (c->args.empty() || c->args.size() > 3) return nullptr;
        for (bool sp : c->spread) if (sp) return nullptr;
        return c;
    }

    // A native counting loop for `for x in range(...)`: no N-element Value list
    // is materialised, and the induction variable is a native double. Argument
    // evaluation and coercion order, and every error, match the range built-in.
    void emitRangeLoop(ForInStmt* s, CallExpr* call) {
        emit("{");
        indent_++;
        pushScope();
        size_t n = call->args.size();
        std::string cl = std::to_string(call->line);   // the range() call site, for errors
        // Evaluate all args first (as the call would), then coerce in order.
        std::vector<std::string> a(n);
        for (size_t i = 0; i < n; ++i) {
            a[i] = fresh("ra");
            emit("bee::Value " + a[i] + " = (" + emitExpr(call->args[i].get()) + ");");
        }
        auto num = [&](const std::string& v) { return "bee::aot::rangeNum(I, " + v + ", " + cl + ")"; };
        std::string lo = fresh("lo"), hi = fresh("hi"), st = fresh("st");
        if (n == 1) {
            emit("double " + hi + " = " + num(a[0]) + ";");
            emit("double " + lo + " = 0, " + st + " = 1;");
        } else if (n == 2) {
            emit("double " + lo + " = " + num(a[0]) + ";");
            emit("double " + hi + " = " + num(a[1]) + "; double " + st + " = 1;");
        } else {
            emit("double " + lo + " = " + num(a[0]) + ";");
            emit("double " + hi + " = " + num(a[1]) + ";");
            emit("double " + st + " = " + num(a[2]) + ";");
        }
        emit("if (" + st + " == 0) I.error(\"range: step cannot be zero\", " + cl + ");");
        std::string c = declare(s->name);
        std::string x = fresh("x");
        emit("auto " + c + " = bee::aot::cell();");
        emit("for (double " + x + " = " + lo + "; " + st + " > 0 ? " + x + " < " + hi + " : " + x +
             " > " + hi + "; " + x + " += " + st + ") {");
        indent_++;
        emit("*" + c + " = bee::Value(" + x + ");");
        pushScope();
        emitStmt(s->body.get());
        popScope();
        indent_--;
        emit("}");
        popScope();
        indent_--;
        emit("}");
    }

    void forInStmt(ForInStmt* s) {
        if (CallExpr* call = rangeCall(s->iterable.get())) { emitRangeLoop(s, call); return; }
        emit("{");
        indent_++;
        pushScope();
        std::string seq = fresh("seq");
        emit("auto " + seq + " = bee::aot::iterate(I, (" + emitExpr(s->iterable.get()) + "), " +
             std::to_string(s->line) + ");");
        std::string c = declare(s->name);
        std::string idx = fresh("i");
        emit("auto " + c + " = bee::aot::cell();");
        emit("for (size_t " + idx + " = 0; " + idx + " < " + seq + "->size(); ++" + idx + ") {");
        indent_++;
        emit("*" + c + " = (*" + seq + ")[" + idx + "];");
        pushScope();
        emitStmt(s->body.get());
        popScope();
        indent_--;
        emit("}");
        popScope();
        indent_--;
        emit("}");
    }

    void funcStmt(FunctionStmt* s) {
        if (scopes_.back().global) {
            emit("bee::aot::defineIn(" + genv_ + ", " + cstr(s->name) + ", " + funcValue(s) + ");");
        } else {
            std::string c = declare(s->name);
            emit("auto " + c + " = bee::aot::cell();");
            emit("bee::aot::store(" + c + ", " + funcValue(s) + ");");
        }
    }

    void returnStmt(ReturnStmt* s) {
        if (s->value) emit("return (" + coerceIfSized(curReturnType_, emitExpr(s->value.get())) + ");");
        else emit("return bee::Value();");
    }

    void classStmt(ClassStmt* s) {
        std::string sup = s->superclassName.empty()
                              ? std::string("bee::Value()")
                              : nameValue(s->superclassName, s->line);
        // Emit each method with `super` resolving to this class's superclass.
        std::string savedSuper = curSuperName_;
        curSuperName_ = s->superclassName;
        std::string methods = "{";
        for (size_t i = 0; i < s->methods.size(); ++i) {
            if (i) methods += ", ";
            methods += "{ " + cstr(s->methods[i]->name) + ", " +
                       funcValue(s->methods[i].get(), /*isMethod=*/true) + " }";
        }
        methods += "}";
        curSuperName_ = savedSuper;

        std::string classVal = "bee::aot::makeClass(I, " + cstr(s->name) + ", (" + sup + "), " +
                               methods + ", " + std::to_string(s->line) + ")";
        if (scopes_.back().global) {
            emit("bee::aot::defineIn(" + genv_ + ", " + cstr(s->name) + ", " + classVal + ");");
        } else {
            std::string c = declare(s->name);
            emit("auto " + c + " = bee::aot::cell(" + classVal + ");");
        }
    }

    void matchStmt(MatchStmt* s) {
        emit("{");
        indent_++;
        std::string subj = fresh("subj");
        emit("bee::Value " + subj + " = (" + emitExpr(s->subject.get()) + ");");
        bool first = true;
        for (auto& c : s->cases) {
            std::string test;
            for (size_t i = 0; i < c.values.size(); ++i) {
                if (i) test += " || ";
                test += "I.valuesEqual(" + subj + ", (" + emitExpr(c.values[i].get()) + "))";
            }
            if (test.empty()) test = "false";
            emit(std::string(first ? "if (" : "} else if (") + test + ") {");
            indent_++; pushScope(); emitStmt(c.body.get()); popScope(); indent_--;
            first = false;
        }
        if (s->hasDefault) {
            emit(first ? "{" : "} else {");
            indent_++; pushScope(); emitStmt(s->defaultBody.get()); popScope(); indent_--;
            emit("}");
        } else if (!first) {
            emit("}");
        }
        indent_--;
        emit("}");
    }

    void importStmt(ImportStmt* s) {
        auto it = modByName_.find(s->moduleName);
        if (it == modByName_.end()) {
            err(s->line, "cannot resolve module '" + s->moduleName +
                         "' for AOT (only sibling .bee files and a lib/ folder are searched, "
                         "and native/hive packages are not yet supported)");
            return;
        }
        std::string modval = "bee_module_" + it->second->id + "(I)";
        int ln = s->line;
        if (!s->isFrom) {
            std::string bind = s->alias.empty() ? s->bindName : s->alias;
            emit("bee::aot::defineIn(" + genv_ + ", " + cstr(bind) + ", " + modval + ");");
            return;
        }
        emit("{");
        indent_++;
        emit("bee::Value _m = " + modval + ";");
        if (s->importAll) {
            for (const auto& n : it->second->publicNames)
                emit("bee::aot::defineIn(" + genv_ + ", " + cstr(n) + ", I.getProperty(_m, " +
                     cstr(n) + ", " + std::to_string(ln) + "));");
        } else {
            for (const auto& pr : s->names) {
                std::string alias = pr.second.empty() ? pr.first : pr.second;
                emit("bee::aot::defineIn(" + genv_ + ", " + cstr(alias) + ", I.getProperty(_m, " +
                     cstr(pr.first) + ", " + std::to_string(ln) + "));");
            }
        }
        indent_--;
        emit("}");
    }

    void destructureLet(LetStmt* s) {
        std::string init = s->initializer ? emitExpr(s->initializer.get()) : std::string("bee::Value()");
        std::string tmp = fresh("dt");
        emit("bee::Value " + tmp + " = (" + init + ");");
        for (size_t i = 0; i < s->names.size(); ++i) {
            std::string idx = s->destructureDict
                ? ("bee::Value(std::string(" + cstr(s->names[i]) + "))")
                : ("bee::Value((double)" + std::to_string(i) + ")");
            std::string val = "I.indexGet(" + tmp + ", " + idx + ", " + std::to_string(s->line) + ")";
            if (scopes_.back().global) {
                emit("bee::aot::defineIn(" + genv_ + ", " + cstr(s->names[i]) + ", " + val + ");");
            } else {
                std::string c = declare(s->names[i]);
                emit("auto " + c + " = bee::aot::cell(" + val + ");");
            }
        }
    }

    void tryStmt(TryStmt* s) {
        emit("{");
        indent_++;
        if (s->hasFinally) {
            emit("bee::aot::Finally _fin([&]{");
            indent_++; pushScope(); emitStmt(s->finallyBody.get()); popScope(); indent_--;
            emit("});");
        }
        emit("try {");
        indent_++; pushScope(); emitStmt(s->body.get()); popScope(); indent_--;
        emit("} catch (bee::BeeThrow& _t) {");
        indent_++; emitCatchBody(s, "_t.value"); indent_--;
        emit("} catch (bee::TracedError& _e) {");
        indent_++; emitCatchBody(s, "bee::Value(_e.brief())"); indent_--;
        emit("}");
        indent_--;
        emit("}");
    }

    void emitCatchBody(TryStmt* s, const std::string& boundExpr) {
        if (!s->hasCatch) { emit("throw;"); return; }
        pushScope();
        if (!s->catchName.empty()) {
            std::string c = declare(s->catchName);
            emit("auto " + c + " = bee::aot::cell(" + boundExpr + ");");
        } else {
            emit("(void)(" + boundExpr + ");");
        }
        if (s->catchBody) emitStmt(s->catchBody.get());
        popScope();
    }
};

} // namespace

std::string aotGenerate(const Program& program, const std::string& sourceName,
                        const std::vector<AotModule>& modules,
                        std::vector<AotError>& errors) {
    Codegen cg(errors);
    return cg.run(program, sourceName, modules);
}

} // namespace bee
