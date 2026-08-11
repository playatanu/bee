//
// AST -> register bytecode. See chunk.hpp for the instruction format and
// compiler.hpp for what "cannot compile" means (it is never an error).
//
#include "compiler.hpp"
#include "ast.hpp"
#include <map>

namespace bee {
namespace {

// Thrown internally to abandon a function that uses something unsupported.
struct CompileBail {};

class Compiler {
public:
    explicit Compiler(Chunk& ch) : ch_(ch) {}

    void compile(const FunctionStmt* fn) {
        // Locals occupy registers 0 .. frameSlots-1; temporaries live above.
        nextReg_ = fn->frameSlots;
        maxReg_ = nextReg_;
        if (nextReg_ > kMaxOperand) throw CompileBail{};
        line_ = fn->line;

        // A sized-numeric return type needs wrapping the VM doesn't do; the
        // tree-walker coerces at return, so hand the whole function to it.
        // (Sized param/let/assign types bail via fromAnnotation below.)
        if (fn->returnType.isSizedNum()) throw CompileBail{};

        // Declared parameters seed the register types. They are checked on
        // entry and on every assignment, so the annotation holds throughout.
        for (size_t i = 0; i < fn->paramTypes.size(); ++i)
            setType((uint16_t)(fn->paramStart + (int)i), fromAnnotation(fn->paramTypes[i]));

        for (auto& s : fn->body) stmt(s.get());
        emit(Op::RETURN_NIL);

        if (maxReg_ > kMaxFrameRegs) throw CompileBail{};   // must fit one block
        ch_.decl = fn;
        ch_.numRegs = maxReg_;
        ch_.paramStart = fn->paramStart;
        ch_.numParams = (int)fn->params.size();
    }

private:
    Chunk& ch_;
    int nextReg_ = 0, maxReg_ = 0;
    int line_ = 0;
    std::map<std::string, uint16_t> nameIndex_;

    // What a register is statically known to hold.
    //
    // Only two sources are trusted. A *declared* local or parameter, because
    // the annotation is enforced everywhere the binding is written -- entry and
    // every assignment -- so it holds for the whole function including across
    // loop back-edges. And a temporary, whose type is fixed by the one
    // instruction that writes it.
    //
    // An *undeclared* local is deliberately left Unknown even when its
    // initialiser is obviously a number: propagating that forward is only sound
    // with a fixpoint over the control-flow graph, since a later assignment in a
    // loop body reaches uses earlier in the loop. Getting that wrong would emit
    // an unchecked numeric op against a string. Annotate the variable and the
    // typed path opens up.
    enum class RT : uint8_t { Unknown, Num, Bool, Buffer };
    std::vector<RT> regType_;

    RT typeAt(uint16_t r) const { return r < regType_.size() ? regType_[r] : RT::Unknown; }
    void setType(uint16_t r, RT t) {
        if (regType_.size() <= r) regType_.resize((size_t)r + 1, RT::Unknown);
        regType_[r] = t;
    }
    static RT fromAnnotation(const TypeAnn& t) {
        // Sized numeric types (i8..u64, f16/f32/f64) carry wrapping/rounding
        // semantics the register VM doesn't implement yet, so abandon the
        // function and let the tree-walker (which does) run it.
        if (t.isSizedNum()) throw CompileBail{};
        switch (t.kind) {
            case TypeAnn::Kind::Num:    return RT::Num;
            case TypeAnn::Kind::Bool:   return RT::Bool;
            case TypeAnn::Kind::Buffer: return RT::Buffer;
            default:                    return RT::Unknown;
        }
    }

    struct Loop {
        std::vector<size_t> breaks;      // JUMPs to patch to the loop exit
        std::vector<size_t> continues;   // JUMPs to patch to the next-iteration point
    };
    std::vector<Loop> loops_;

    // ---- emitting -------------------------------------------------------
    size_t emit(Op op, uint16_t a = 0, uint16_t b = 0, uint16_t c = 0) {
        ch_.code.emplace_back(op, a, b, c);
        ch_.lines.push_back(line_);
        if (ch_.code.size() > kMaxOperand) throw CompileBail{};  // jumps are 16-bit
        return ch_.code.size() - 1;
    }
    size_t here() const { return ch_.code.size(); }
    void patch(size_t at, size_t target) { ch_.code[at].b = (uint16_t)target; }
    void patchAll(std::vector<size_t>& sites, size_t target) {
        for (size_t s : sites) patch(s, target);
    }

    uint16_t constant(const Value& v) {
        ch_.constants.push_back(v);
        if (ch_.constants.size() > kMaxOperand) throw CompileBail{};
        return (uint16_t)(ch_.constants.size() - 1);
    }
    uint16_t nameSlot(const std::string& n) {
        auto it = nameIndex_.find(n);
        if (it != nameIndex_.end()) return it->second;
        ch_.names.push_back(n);
        if (ch_.names.size() > kMaxOperand) throw CompileBail{};
        uint16_t i = (uint16_t)(ch_.names.size() - 1);
        nameIndex_[n] = i;
        return i;
    }
    uint16_t cacheSlot() {
        ch_.globalCache.push_back(nullptr);
        if (ch_.globalCache.size() > kMaxOperand) throw CompileBail{};
        return (uint16_t)(ch_.globalCache.size() - 1);
    }
    // One inline-cache entry per property or method site. Per *site*, not per
    // name: two `p.x` in different places see different shapes at different
    // times, and sharing an entry would make them evict each other.
    uint16_t typeCheckSite(const TypeAnn& t, const std::string& name, int line) {
        ch_.typeChecks.push_back(TypeCheck{ &t, name, line });
        if (ch_.typeChecks.size() > kMaxOperand) throw CompileBail{};
        return (uint16_t)(ch_.typeChecks.size() - 1);
    }
    uint16_t propSite(const std::string& name) {
        PropSite s;
        s.name = nameSlot(name);
        ch_.sites.push_back(std::move(s));
        if (ch_.sites.size() > kMaxOperand) throw CompileBail{};
        return (uint16_t)(ch_.sites.size() - 1);
    }

    // ---- registers ------------------------------------------------------
    // A simple stack discipline: temporaries are allocated above the locals and
    // released in bulk once the expression that needed them is done.
    uint16_t alloc() {
        if (nextReg_ >= kMaxOperand) throw CompileBail{};
        uint16_t r = (uint16_t)nextReg_++;
        if (nextReg_ > maxReg_) maxReg_ = nextReg_;
        setType(r, RT::Unknown);   // a fresh temporary knows nothing yet
        return r;
    }
    int mark() const { return nextReg_; }
    void release(int to) { nextReg_ = to; }

    // ---- statements -----------------------------------------------------
    void stmt(Stmt* s) {
        line_ = s->line ? s->line : line_;
        switch (s->kind) {
            case Stmt::Kind::Expression: {
                int m = mark();
                expr(static_cast<ExprStmt*>(s)->expr.get());
                release(m);
                break;
            }
            case Stmt::Kind::Let: {
                auto* l = static_cast<LetStmt*>(s);
                if (l->isDestructure || l->global || l->slot < 0) throw CompileBail{};
                int m = mark();
                if (l->initializer) exprTo(l->initializer.get(), (uint16_t)l->slot);
                else emit(Op::LOAD_NIL, (uint16_t)l->slot);
                if (l->type.declared()) {
                    // Elided when the initialiser is already known to have the
                    // declared type -- `let x: num = a + b` with a, b declared.
                    RT want = fromAnnotation(l->type);
                    if (want == RT::Unknown || typeAt((uint16_t)l->slot) != want ||
                        !l->initializer)
                        emit(Op::CHECK_TYPE, (uint16_t)l->slot,
                             typeCheckSite(l->type, l->name, l->line));
                    setType((uint16_t)l->slot, want);
                } else {
                    setType((uint16_t)l->slot, RT::Unknown);   // see regType_
                }
                release(m);
                break;
            }
            case Stmt::Kind::Block: {
                auto* b = static_cast<BlockStmt*>(s);
                // A block needing its own environment implies a closure inside,
                // which we do not compile.
                if (!b->transparent) throw CompileBail{};
                for (auto& st : b->statements) stmt(st.get());
                break;
            }
            case Stmt::Kind::If:     sIf(static_cast<IfStmt*>(s)); break;
            case Stmt::Kind::While:  sWhile(static_cast<WhileStmt*>(s)); break;
            case Stmt::Kind::For:    sFor(static_cast<ForStmt*>(s)); break;
            case Stmt::Kind::ForIn:  sForIn(static_cast<ForInStmt*>(s)); break;
            case Stmt::Kind::Match:  sMatch(static_cast<MatchStmt*>(s)); break;
            case Stmt::Kind::Return: {
                auto* r = static_cast<ReturnStmt*>(s);
                if (!r->value) { emit(Op::RETURN_NIL); break; }
                int m = mark();
                uint16_t v = expr(r->value.get());
                emit(Op::RETURN, v);
                release(m);
                break;
            }
            case Stmt::Kind::Break:
                if (loops_.empty()) throw CompileBail{};
                loops_.back().breaks.push_back(emit(Op::JUMP));
                break;
            case Stmt::Kind::Continue:
                if (loops_.empty()) throw CompileBail{};
                loops_.back().continues.push_back(emit(Op::JUMP));
                break;
            default:
                // Function, Class, Import, Try, Throw: not compiled yet.
                throw CompileBail{};
        }
    }

    void sIf(IfStmt* s) {
        int m = mark();
        uint16_t c = expr(s->condition.get());
        size_t jf = emit(Op::JUMP_IF_FALSE, c);
        release(m);
        stmt(s->thenBranch.get());
        if (s->elseBranch) {
            size_t jend = emit(Op::JUMP);
            patch(jf, here());
            stmt(s->elseBranch.get());
            patch(jend, here());
        } else {
            patch(jf, here());
        }
    }

    void sWhile(WhileStmt* s) {
        size_t top = here();
        int m = mark();
        uint16_t c = expr(s->condition.get());
        size_t jf = emit(Op::JUMP_IF_FALSE, c);
        release(m);
        loops_.push_back({});
        stmt(s->body.get());
        Loop lp = std::move(loops_.back());
        loops_.pop_back();
        patchAll(lp.continues, top);      // `continue` re-tests the condition
        emit(Op::JUMP, 0, (uint16_t)top);
        patch(jf, here());
        patchAll(lp.breaks, here());
    }

    void sFor(ForStmt* s) {
        // An own scope means something captured it, i.e. a closure -- not compiled.
        if (s->ownScope) throw CompileBail{};
        if (s->init) stmt(s->init.get());

        size_t top = here();
        size_t jf = SIZE_MAX;
        if (s->condition) {
            int m = mark();
            uint16_t c = expr(s->condition.get());
            jf = emit(Op::JUMP_IF_FALSE, c);
            release(m);
        }
        loops_.push_back({});
        stmt(s->body.get());
        Loop lp = std::move(loops_.back());
        loops_.pop_back();

        patchAll(lp.continues, here());   // `continue` runs the increment
        if (s->increment) {
            int m = mark();
            expr(s->increment.get());
            release(m);
        }
        emit(Op::JUMP, 0, (uint16_t)top);
        if (jf != SIZE_MAX) patch(jf, here());
        patchAll(lp.breaks, here());
    }

    void sForIn(ForInStmt* s) {
        if (s->ownScope) throw CompileBail{};   // captured by a closure
        // The loop variable takes whatever the iterable yields. It has to be
        // reset explicitly: merged sibling scopes reuse slots, so this slot may
        // still be marked `num` from an annotated `let` in an earlier block,
        // and inheriting that would emit unchecked arithmetic against a string.
        setType((uint16_t)s->varSlot, RT::Unknown);
        int m = mark();
        uint16_t src = expr(s->iterable.get());
        uint16_t it = alloc();            // the normalised list
        uint16_t idx = alloc();           // and the cursor, which must follow it
        if (idx != it + 1) throw CompileBail{};
        emit(Op::ITER_PREP, it, src);

        size_t top = here();
        size_t jend = emit(Op::ITER_NEXT, it, 0, (uint16_t)s->varSlot);
        loops_.push_back({});
        stmt(s->body.get());
        Loop lp = std::move(loops_.back());
        loops_.pop_back();
        patchAll(lp.continues, top);
        emit(Op::JUMP, 0, (uint16_t)top);
        patch(jend, here());
        patchAll(lp.breaks, here());
        release(m);
    }

    void sMatch(MatchStmt* s) {
        int m = mark();
        uint16_t subj = expr(s->subject.get());
        std::vector<size_t> toEnd;
        std::vector<size_t> pendingNext;   // jumps to the next case's test

        for (auto& c : s->cases) {
            patchAll(pendingNext, here());
            pendingNext.clear();
            std::vector<size_t> intoBody;
            for (size_t i = 0; i < c.values.size(); ++i) {
                int mm = mark();
                uint16_t v = expr(c.values[i].get());
                uint16_t eq = alloc();
                emit(Op::EQ, eq, subj, v);
                intoBody.push_back(emit(Op::JUMP_IF_TRUE, eq));
                release(mm);
            }
            size_t skip = emit(Op::JUMP);      // no value matched: next case
            pendingNext.push_back(skip);
            patchAll(intoBody, here());
            stmt(c.body.get());
            toEnd.push_back(emit(Op::JUMP));
        }
        patchAll(pendingNext, here());
        if (s->hasDefault) stmt(s->defaultBody.get());
        patchAll(toEnd, here());
        release(m);
    }

    // ---- expressions ----------------------------------------------------
    // expr() returns *some* register holding the value -- possibly a local slot,
    // which the caller must not overwrite. exprTo() puts it in a given register.
    void exprTo(Expr* e, uint16_t dst) {
        // Compute straight into the destination where that is free: this is what
        // turns `s = s + 1` into a single ADDK writing its own slot.
        switch (e->kind) {
            case Expr::Kind::Binary: {
                if (emitBinary(static_cast<BinaryExpr*>(e), dst)) return;
                break;
            }
            case Expr::Kind::Literal:
                loadConst(static_cast<LiteralExpr*>(e)->value, dst);
                return;
            default: break;
        }
        int m = mark();
        uint16_t r = expr(e);
        release(m);
        if (r != dst) emit(Op::MOVE, dst, r);
        setType(dst, typeAt(r));
    }

    void loadConst(const Value& v, uint16_t dst) {
        setType(dst, v.isNumber() ? RT::Num : v.isBool() ? RT::Bool : RT::Unknown);
        if (v.isNil()) { emit(Op::LOAD_NIL, dst); return; }
        if (v.isBool()) { emit(Op::LOAD_BOOL, dst, v.asBool() ? 1 : 0); return; }
        emit(Op::LOAD_CONST, dst, constant(v));
    }

    uint16_t expr(Expr* e) {
        line_ = e->line ? e->line : line_;
        switch (e->kind) {
            case Expr::Kind::Literal: {
                uint16_t r = alloc();
                loadConst(static_cast<LiteralExpr*>(e)->value, r);
                return r;
            }
            case Expr::Kind::Variable: {
                auto* v = static_cast<VariableExpr*>(e);
                if (v->global) {
                    uint16_t r = alloc();
                    emit(Op::GET_GLOBAL, r, nameSlot(v->name), cacheSlot());
                    return r;
                }
                if (v->slot < 0) throw CompileBail{};
                if (v->depth == 0) return (uint16_t)v->slot;   // it is already a register
                uint16_t r = alloc();
                emit(Op::GET_ENV, r, (uint16_t)v->depth, (uint16_t)v->slot);
                return r;
            }
            case Expr::Kind::Assign: {
                auto* a = static_cast<AssignExpr*>(e);
                if (!a->value) throw CompileBail{};
                if (!a->global && a->depth == 0) {
                    if (a->slot < 0) throw CompileBail{};
                    RT want = fromAnnotation(a->declaredType);
                    exprTo(a->value.get(), (uint16_t)a->slot);
                    // An annotation binds the name for its whole life, so an
                    // assignment has to satisfy it too -- unless the value is
                    // already known to.
                    if (a->declaredType.declared() &&
                        (want == RT::Unknown || typeAt((uint16_t)a->slot) != want))
                        emit(Op::CHECK_TYPE, (uint16_t)a->slot,
                             typeCheckSite(a->declaredType, a->name, a->line));
                    setType((uint16_t)a->slot, want);   // Unknown when undeclared
                    return (uint16_t)a->slot;
                }
                uint16_t v = expr(a->value.get());
                if (a->global) emit(Op::SET_GLOBAL, v, nameSlot(a->name), cacheSlot());
                else emit(Op::SET_ENV, v, (uint16_t)a->depth, (uint16_t)a->slot);
                if (a->declaredType.declared())
                    emit(Op::CHECK_TYPE, v, typeCheckSite(a->declaredType, a->name, a->line));
                return v;
            }
            case Expr::Kind::Binary: {
                int m = mark();
                uint16_t dst;
                {
                    auto* b = static_cast<BinaryExpr*>(e);
                    // Operands first, then a destination -- which may reuse an
                    // operand's register, since an instruction reads before it
                    // writes.
                    uint16_t lr = expr(b->left.get());
                    RT lt = typeAt(lr);          // before alloc() can reset it
                    if (isConstFold(b)) {
                        release(m);
                        dst = alloc();
                        emitConstOp(b, dst, lr, lt);
                        return dst;
                    }
                    uint16_t rr = expr(b->right.get());
                    RT rt = typeAt(rr);
                    release(m);
                    dst = alloc();
                    emitBinOp(binOp(b->op), dst, lr, rr, lt, rt);
                }
                return dst;
            }
            case Expr::Kind::Logical: {
                auto* l = static_cast<LogicalExpr*>(e);
                uint16_t dst = alloc();
                int m = mark();
                exprTo(l->left.get(), dst);
                release(m);
                // Bee yields the operand itself, not a boolean, so the result
                // register already holds the answer when we short-circuit.
                size_t jmp = emit(l->op == TokenType::OR ? Op::JUMP_IF_TRUE : Op::JUMP_IF_FALSE,
                                  dst);
                m = mark();
                exprTo(l->right.get(), dst);
                release(m);
                patch(jmp, here());
                return dst;
            }
            case Expr::Kind::Unary: {
                auto* u = static_cast<UnaryExpr*>(e);
                int m = mark();
                uint16_t r = expr(u->right.get());
                release(m);
                uint16_t dst = alloc();
                Op op = u->op == TokenType::MINUS ? Op::NEG
                      : u->op == TokenType::NOT ? Op::NOT
                      : u->op == TokenType::BIT_NOT ? Op::BNOT
                      : Op::HALT;
                if (op == Op::HALT) throw CompileBail{};
                emit(op, dst, r);
                return dst;
            }
            case Expr::Kind::Grouping:
                return expr(static_cast<GroupingExpr*>(e)->inner.get());

            case Expr::Kind::Ternary: {
                auto* t = static_cast<TernaryExpr*>(e);
                uint16_t dst = alloc();
                int m = mark();
                uint16_t c = expr(t->cond.get());
                release(m);
                size_t jf = emit(Op::JUMP_IF_FALSE, c);
                m = mark();
                exprTo(t->thenBranch.get(), dst);
                release(m);
                size_t jend = emit(Op::JUMP);
                patch(jf, here());
                m = mark();
                exprTo(t->elseBranch.get(), dst);
                release(m);
                patch(jend, here());
                return dst;
            }
            case Expr::Kind::Call:      return eCall(static_cast<CallExpr*>(e));
            case Expr::Kind::Get: {
                auto* g = static_cast<GetExpr*>(e);
                int m = mark();
                uint16_t o = expr(g->object.get());
                release(m);
                uint16_t dst = alloc();
                emit(Op::GET_PROP, dst, o, propSite(g->name));
                return dst;
            }
            case Expr::Kind::Set: {
                auto* s = static_cast<SetExpr*>(e);
                if (s->op != TokenType::ASSIGN) throw CompileBail{};  // exact error text
                int m = mark();
                uint16_t o = expr(s->object.get());
                uint16_t v = expr(s->value.get());
                emit(Op::SET_PROP, o, propSite(s->name), v);
                release(m);
                uint16_t dst = alloc();
                if (dst != v) emit(Op::MOVE, dst, v);
                return dst;
            }
            case Expr::Kind::Index: {
                auto* i = static_cast<IndexExpr*>(e);
                int m = mark();
                uint16_t o = expr(i->object.get());
                uint16_t x = expr(i->index.get());
                release(m);
                uint16_t dst = alloc();
                // A buffer holds numbers and nothing else, so a declared
                // `buffer` indexed by a known number needs no type tests and
                // yields a known number -- which then feeds the typed
                // arithmetic. This is the path meant to reach native speed.
                bool buf = typeAt(o) == RT::Buffer && typeAt(x) == RT::Num;
                emit(buf ? Op::INDEX_BUF : Op::INDEX, dst, o, x);
                setType(dst, buf ? RT::Num : RT::Unknown);
                return dst;
            }
            case Expr::Kind::IndexSet: {
                auto* i = static_cast<IndexSetExpr*>(e);
                int m = mark();
                uint16_t o = expr(i->object.get());
                uint16_t x = expr(i->index.get());
                uint16_t v = expr(i->value.get());
                if (i->op != TokenType::ASSIGN) {
                    // `xs[i] += k` is a read, the compound-assignment arithmetic,
                    // and a write -- exactly what the tree-walker does.
                    uint16_t cur = alloc();
                    emit(Op::INDEX, cur, o, x);
                    emit(arithOp(i->op), cur, cur, v);
                    v = cur;
                }
                bool buf = typeAt(o) == RT::Buffer && typeAt(x) == RT::Num &&
                           typeAt(v) == RT::Num;
                emit(buf ? Op::INDEX_SET_BUF : Op::INDEX_SET, o, x, v);
                release(m);
                uint16_t dst = alloc();
                if (dst != v) emit(Op::MOVE, dst, v);
                return dst;
            }
            case Expr::Kind::Super: {
                auto* s = static_cast<SuperExpr*>(e);
                if (s->depth < 0) throw CompileBail{};
                int m = mark();
                uint16_t self;                 // R[self] = this, R[self+1] = superclass
                if (s->depth == 0) {
                    self = 0;                  // already adjacent in this frame
                } else {
                    self = alloc();
                    uint16_t sup = alloc();
                    if (sup != self + 1) throw CompileBail{};
                    emit(Op::GET_ENV, self, (uint16_t)s->depth, 0);
                    emit(Op::GET_ENV, sup, (uint16_t)s->depth, 1);
                }
                release(m);
                uint16_t dst = alloc();
                emit(Op::SUPER, dst, nameSlot(s->method), self);
                return dst;
            }
            case Expr::Kind::ListComp: {
                auto* c = static_cast<ListCompExpr*>(e);
                if (c->ownScope) throw CompileBail{};   // captured by a closure
                setType((uint16_t)c->varSlot, RT::Unknown);   // see sForIn
                int m = mark();
                uint16_t out = alloc();        // allocated first, so it survives
                emit(Op::NEW_LIST, out, 0, 0);
                uint16_t src = expr(c->iterable.get());
                uint16_t it = alloc();
                uint16_t idx = alloc();
                if (idx != it + 1) throw CompileBail{};
                emit(Op::ITER_PREP, it, src);

                size_t top = here();
                size_t jend = emit(Op::ITER_NEXT, it, 0, (uint16_t)c->varSlot);
                size_t jskip = SIZE_MAX;
                if (c->cond) {
                    int mm = mark();
                    uint16_t cv = expr(c->cond.get());
                    jskip = emit(Op::JUMP_IF_FALSE, cv);
                    release(mm);
                }
                {
                    int mm = mark();
                    uint16_t ev = expr(c->elem.get());
                    emit(Op::LIST_PUSH, out, ev);
                    release(mm);
                }
                size_t back = emit(Op::JUMP, 0, (uint16_t)top);
                if (jskip != SIZE_MAX) patch(jskip, back);   // filtered out: next item
                patch(jend, here());
                release(m);
                uint16_t dst = alloc();
                if (dst != out) throw CompileBail{};
                return dst;
            }
            case Expr::Kind::Slice: {
                auto* s = static_cast<SliceExpr*>(e);
                int m = mark();
                uint16_t o = expr(s->object.get());
                uint16_t lo = alloc();
                if (s->start) exprTo(s->start.get(), lo); else emit(Op::LOAD_NIL, lo);
                uint16_t hi = alloc();
                if (hi != lo + 1) throw CompileBail{};
                if (s->end) exprTo(s->end.get(), hi); else emit(Op::LOAD_NIL, hi);
                release(m);
                uint16_t dst = alloc();
                emit(Op::SLICE, dst, o, lo);
                return dst;
            }
            case Expr::Kind::ListLit: {
                auto* l = static_cast<ListLitExpr*>(e);
                for (bool sp : l->spread) if (sp) throw CompileBail{};
                int m = mark();
                uint16_t base = 0;
                for (size_t i = 0; i < l->elements.size(); ++i) {
                    uint16_t r = alloc();
                    if (i == 0) base = r;
                    else if (r != base + i) throw CompileBail{};
                    int mm = mark();
                    exprTo(l->elements[i].get(), r);
                    release(mm);
                }
                if (l->elements.empty()) base = (uint16_t)mark();
                release(m);
                uint16_t dst = alloc();
                emit(Op::NEW_LIST, dst, base, (uint16_t)l->elements.size());
                return dst;
            }
            case Expr::Kind::DictLit: {
                auto* d = static_cast<DictLitExpr*>(e);
                int m = mark();
                uint16_t base = 0;
                size_t n = 0;
                for (auto& kv : d->entries) {
                    uint16_t k = alloc();
                    if (n == 0) base = k;
                    else if (k != base + 2 * n) throw CompileBail{};
                    int mm = mark();
                    exprTo(kv.first.get(), k);
                    release(mm);
                    uint16_t v = alloc();
                    if (v != k + 1) throw CompileBail{};
                    mm = mark();
                    exprTo(kv.second.get(), v);
                    release(mm);
                    ++n;
                }
                if (d->entries.empty()) base = (uint16_t)mark();
                release(m);
                uint16_t dst = alloc();
                emit(Op::NEW_DICT, dst, base, (uint16_t)n);
                return dst;
            }
            case Expr::Kind::This: {
                auto* t = static_cast<ThisExpr*>(e);
                if (t->depth < 0) throw CompileBail{};
                if (t->depth == 0) return 0;   // `this` is slot 0 of the frame
                uint16_t r = alloc();
                emit(Op::GET_ENV, r, (uint16_t)t->depth, 0);
                return r;
            }
            default:
                // Super, Function (a closure), ListComp: not compiled yet.
                throw CompileBail{};
        }
    }

    uint16_t eCall(CallExpr* e) {
        for (bool sp : e->spread) if (sp) throw CompileBail{};
        if (e->args.size() > 250) throw CompileBail{};

        // `obj.m(args)` is compiled as one instruction rather than a property
        // read followed by a call. Reading `obj.m` on its own has to *produce a
        // callable*, which for a method means allocating a bound copy of it on
        // every call; fusing the two lets the receiver be passed directly.
        GetExpr* method = e->callee->kind == Expr::Kind::Get
                        ? static_cast<GetExpr*>(e->callee.get()) : nullptr;

        int m = mark();
        uint16_t base = alloc();
        {
            int mm = mark();
            exprTo(method ? method->object.get() : e->callee.get(), base);
            release(mm);
        }
        for (size_t i = 0; i < e->args.size(); ++i) {
            uint16_t r = alloc();
            if (r != base + 1 + i) throw CompileBail{};
            int mm = mark();
            exprTo(e->args[i].get(), r);
            release(mm);      // free the argument's own temporaries, keeping r
        }
        release(m);
        uint16_t dst = alloc();
        if (dst != base) throw CompileBail{};   // the result lands in the callee slot
        if (method) emit(Op::CALL_METHOD, base, propSite(method->name),
                         (uint16_t)e->args.size());
        else        emit(Op::CALL, base, (uint16_t)e->args.size());
        return dst;
    }

    // ---- binary helpers -------------------------------------------------
    static Op binOp(TokenType t) {
        switch (t) {
            case TokenType::PLUS:    return Op::ADD;
            case TokenType::MINUS:   return Op::SUB;
            case TokenType::STAR:    return Op::MUL;
            case TokenType::SLASH:   return Op::DIV;
            case TokenType::PERCENT: return Op::MOD;
            case TokenType::BIT_AND: return Op::BAND;
            case TokenType::BIT_OR:  return Op::BOR;
            case TokenType::BIT_XOR: return Op::BXOR;
            case TokenType::SHL:     return Op::SHL;
            case TokenType::SHR:     return Op::SHR;
            case TokenType::LT:      return Op::LT;
            case TokenType::LE:      return Op::LE;
            case TokenType::GT:      return Op::GT;
            case TokenType::GE:      return Op::GE;
            case TokenType::EQ:      return Op::EQ;
            case TokenType::NEQ:     return Op::NE;
            default: throw CompileBail{};
        }
    }

    // Compound assignment uses the interpreter's applyBinaryArith, which is a
    // smaller operator set with its own error messages.
    static Op arithOp(TokenType t) {
        switch (t) {
            case TokenType::PLUS:  return Op::AADD;
            case TokenType::MINUS: return Op::ASUB;
            case TokenType::STAR:  return Op::AMUL;
            case TokenType::SLASH: return Op::ADIV;
            default: throw CompileBail{};
        }
    }

    // The K form of an operator, taking its right operand from the constant
    // table. `i + 1`, `i < n`, `x % 2 == 0` -- a literal right operand is most
    // of what a loop does, and folding it in saves a LOAD_CONST and a register
    // from every iteration.
    static Op binOpK(TokenType t) {
        switch (t) {
            case TokenType::PLUS:    return Op::ADDK;
            case TokenType::MINUS:   return Op::SUBK;
            case TokenType::STAR:    return Op::MULK;
            case TokenType::SLASH:   return Op::DIVK;
            case TokenType::PERCENT: return Op::MODK;
            case TokenType::LT:      return Op::LTK;
            case TokenType::LE:      return Op::LEK;
            case TokenType::GT:      return Op::GTK;
            case TokenType::GE:      return Op::GEK;
            case TokenType::EQ:      return Op::EQK;
            case TokenType::NEQ:     return Op::NEK;
            default:                 return Op::HALT;   // no K form
        }
    }
    static bool isConstFold(BinaryExpr* b) {
        return b->right->kind == Expr::Kind::Literal && binOpK(b->op) != Op::HALT;
    }

    // The typed form of an operator, used when both operands are known numbers.
    // The offset between an op and its _NUM twin is the same for every one of
    // them, which is what the opcode list's ordering is for.
    static Op numForm(Op base) {
        switch (base) {
            case Op::ADD:  return Op::ADD_NUM;   case Op::ADDK: return Op::ADDK_NUM;
            case Op::SUB:  return Op::SUB_NUM;   case Op::SUBK: return Op::SUBK_NUM;
            case Op::MUL:  return Op::MUL_NUM;   case Op::MULK: return Op::MULK_NUM;
            case Op::DIV:  return Op::DIV_NUM;   case Op::DIVK: return Op::DIVK_NUM;
            case Op::MOD:  return Op::MOD_NUM;   case Op::MODK: return Op::MODK_NUM;
            case Op::LT:   return Op::LT_NUM;    case Op::LTK:  return Op::LTK_NUM;
            case Op::LE:   return Op::LE_NUM;    case Op::LEK:  return Op::LEK_NUM;
            case Op::GT:   return Op::GT_NUM;    case Op::GTK:  return Op::GTK_NUM;
            case Op::GE:   return Op::GE_NUM;    case Op::GEK:  return Op::GEK_NUM;
            case Op::EQ:   return Op::EQ_NUM;    case Op::EQK:  return Op::EQK_NUM;
            case Op::NE:   return Op::NE_NUM;    case Op::NEK:  return Op::NEK_NUM;
            default: return base;
        }
    }
    // Does this operator yield a bool rather than a number?
    static bool isComparison(Op base) {
        switch (base) {
            case Op::LT: case Op::LTK: case Op::LE: case Op::LEK:
            case Op::GT: case Op::GTK: case Op::GE: case Op::GEK:
            case Op::EQ: case Op::EQK: case Op::NE: case Op::NEK: return true;
            default: return false;
        }
    }

    // Emit `dst = lhs op rhs`, choosing the typed form when both sides are known
    // numbers, and recording what the destination now holds.
    //
    // The operand types are passed in rather than looked up here, because the
    // destination register is frequently one of the operands (`s = s + x`
    // compiles to `ADD s, s, x`) and allocating it resets its type. Reading the
    // types after that produced the worst possible outcome: correct code that
    // silently declined every typed opcode *and* emitted a redundant check.
    void emitBinOp(Op base, uint16_t dst, uint16_t lhs, uint16_t rhs, RT lt, RT rt) {
        bool typed = lt == RT::Num && rt == RT::Num;
        emit(typed ? numForm(base) : base, dst, lhs, rhs);
        setType(dst, !typed      ? RT::Unknown
                   : isComparison(base) ? RT::Bool
                                        : RT::Num);
    }

    void emitConstOp(BinaryExpr* b, uint16_t dst, uint16_t lhs, RT lt) {
        const Value& k = static_cast<LiteralExpr*>(b->right.get())->value;
        emitBinOp(binOpK(b->op), dst, lhs, constant(k), lt,
                  k.isNumber() ? RT::Num : RT::Unknown);
    }

    // Write a binary expression straight into `dst` when that is safe -- it is,
    // because the VM reads both operands before writing the destination.
    bool emitBinary(BinaryExpr* b, uint16_t dst) {
        int m = mark();
        uint16_t lr = expr(b->left.get());
        RT lt = typeAt(lr);
        if (isConstFold(b)) {
            release(m);
            emitConstOp(b, dst, lr, lt);
            return true;
        }
        uint16_t rr = expr(b->right.get());
        RT rt = typeAt(rr);
        release(m);
        emitBinOp(binOp(b->op), dst, lr, rr, lt, rt);
        return true;
    }
};

} // namespace

std::unique_ptr<Chunk> compileFunction(const FunctionStmt* fn) {
    auto ch = std::make_unique<Chunk>();
    try {
        Compiler c(*ch);
        c.compile(fn);
    } catch (CompileBail&) {
        return nullptr;
    }
    return ch;
}

} // namespace bee
