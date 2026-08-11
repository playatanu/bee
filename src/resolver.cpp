#include "resolver.hpp"
#include <stdexcept>

namespace bee {

void Resolver::markConst(const std::string& name) {
    if (isNamedScope()) globalConsts.insert(name);
    else scopes.back().consts.insert(name);
}

bool Resolver::isConstBinding(const std::string& name) {
    for (int i = (int)scopes.size() - 1; i >= 0; --i) {
        if (scopes[i].named) break;
        if (scopes[i].names.count(name)) return scopes[i].consts.count(name) > 0;
    }
    return globalConsts.count(name) > 0;
}

// Does a block introduce any bindings of its own? If not, it needs no runtime
// environment and can execute directly in its parent scope.
static bool blockNeedsScope(const std::vector<StmtPtr>& stmts) {
    for (auto& s : stmts) {
        switch (s->kind) {
            case Stmt::Kind::Let:
            case Stmt::Kind::Function:
            case Stmt::Kind::Class:
                return true;
            default:
                break;
        }
    }
    return false;
}

// ---- closure detection ----------------------------------------------------
// Merging a scope into the enclosing frame is observable only through a closure:
// a function created inside a loop body captures that body's environment, and
// with a fresh environment per iteration each closure sees its own copy of the
// loop's variables. So a subtree that creates one keeps its own scope.

static bool exprCreatesClosure(const Expr* e);

static bool stmtCreatesClosure(const Stmt* s) {
    if (!s) return false;
    switch (s->kind) {
        case Stmt::Kind::Function:
        case Stmt::Kind::Class:
            return true;
        case Stmt::Kind::Expression:
            return exprCreatesClosure(static_cast<const ExprStmt*>(s)->expr.get());
        case Stmt::Kind::Let: {
            auto* l = static_cast<const LetStmt*>(s);
            return exprCreatesClosure(l->initializer.get());
        }
        case Stmt::Kind::Block:
            for (auto& st : static_cast<const BlockStmt*>(s)->statements)
                if (stmtCreatesClosure(st.get())) return true;
            return false;
        case Stmt::Kind::If: {
            auto* i = static_cast<const IfStmt*>(s);
            return exprCreatesClosure(i->condition.get()) ||
                   stmtCreatesClosure(i->thenBranch.get()) ||
                   stmtCreatesClosure(i->elseBranch.get());
        }
        case Stmt::Kind::While: {
            auto* w = static_cast<const WhileStmt*>(s);
            return exprCreatesClosure(w->condition.get()) || stmtCreatesClosure(w->body.get());
        }
        case Stmt::Kind::For: {
            auto* f = static_cast<const ForStmt*>(s);
            return stmtCreatesClosure(f->init.get()) || exprCreatesClosure(f->condition.get()) ||
                   exprCreatesClosure(f->increment.get()) || stmtCreatesClosure(f->body.get());
        }
        case Stmt::Kind::ForIn: {
            auto* f = static_cast<const ForInStmt*>(s);
            return exprCreatesClosure(f->iterable.get()) || stmtCreatesClosure(f->body.get());
        }
        case Stmt::Kind::Return:
            return exprCreatesClosure(static_cast<const ReturnStmt*>(s)->value.get());
        case Stmt::Kind::Throw:
            return exprCreatesClosure(static_cast<const ThrowStmt*>(s)->value.get());
        case Stmt::Kind::Try: {
            auto* t = static_cast<const TryStmt*>(s);
            return stmtCreatesClosure(t->body.get()) || stmtCreatesClosure(t->catchBody.get()) ||
                   stmtCreatesClosure(t->finallyBody.get());
        }
        case Stmt::Kind::Match: {
            auto* m = static_cast<const MatchStmt*>(s);
            if (exprCreatesClosure(m->subject.get())) return true;
            for (auto& c : m->cases) {
                for (auto& v : c.values) if (exprCreatesClosure(v.get())) return true;
                if (stmtCreatesClosure(c.body.get())) return true;
            }
            return stmtCreatesClosure(m->defaultBody.get());
        }
        case Stmt::Kind::Import:
        case Stmt::Kind::Break:
        case Stmt::Kind::Continue:
            return false;
    }
    return false;
}

static bool exprCreatesClosure(const Expr* e) {
    if (!e) return false;
    switch (e->kind) {
        case Expr::Kind::Function:
            return true;
        case Expr::Kind::ListLit:
            for (auto& el : static_cast<const ListLitExpr*>(e)->elements)
                if (exprCreatesClosure(el.get())) return true;
            return false;
        case Expr::Kind::DictLit:
            for (auto& kv : static_cast<const DictLitExpr*>(e)->entries)
                if (exprCreatesClosure(kv.first.get()) || exprCreatesClosure(kv.second.get()))
                    return true;
            return false;
        case Expr::Kind::Assign:
            return exprCreatesClosure(static_cast<const AssignExpr*>(e)->value.get());
        case Expr::Kind::Binary: {
            auto* b = static_cast<const BinaryExpr*>(e);
            return exprCreatesClosure(b->left.get()) || exprCreatesClosure(b->right.get());
        }
        case Expr::Kind::Logical: {
            auto* l = static_cast<const LogicalExpr*>(e);
            return exprCreatesClosure(l->left.get()) || exprCreatesClosure(l->right.get());
        }
        case Expr::Kind::Unary:
            return exprCreatesClosure(static_cast<const UnaryExpr*>(e)->right.get());
        case Expr::Kind::Call: {
            auto* c = static_cast<const CallExpr*>(e);
            if (exprCreatesClosure(c->callee.get())) return true;
            for (auto& a : c->args) if (exprCreatesClosure(a.get())) return true;
            return false;
        }
        case Expr::Kind::Get:
            return exprCreatesClosure(static_cast<const GetExpr*>(e)->object.get());
        case Expr::Kind::Set: {
            auto* s = static_cast<const SetExpr*>(e);
            return exprCreatesClosure(s->object.get()) || exprCreatesClosure(s->value.get());
        }
        case Expr::Kind::Index: {
            auto* i = static_cast<const IndexExpr*>(e);
            return exprCreatesClosure(i->object.get()) || exprCreatesClosure(i->index.get());
        }
        case Expr::Kind::Slice: {
            auto* s = static_cast<const SliceExpr*>(e);
            return exprCreatesClosure(s->object.get()) || exprCreatesClosure(s->start.get()) ||
                   exprCreatesClosure(s->end.get());
        }
        case Expr::Kind::IndexSet: {
            auto* i = static_cast<const IndexSetExpr*>(e);
            return exprCreatesClosure(i->object.get()) || exprCreatesClosure(i->index.get()) ||
                   exprCreatesClosure(i->value.get());
        }
        case Expr::Kind::Grouping:
            return exprCreatesClosure(static_cast<const GroupingExpr*>(e)->inner.get());
        case Expr::Kind::Ternary: {
            auto* t = static_cast<const TernaryExpr*>(e);
            return exprCreatesClosure(t->cond.get()) || exprCreatesClosure(t->thenBranch.get()) ||
                   exprCreatesClosure(t->elseBranch.get());
        }
        case Expr::Kind::ListComp: {
            auto* c = static_cast<const ListCompExpr*>(e);
            return exprCreatesClosure(c->iterable.get()) || exprCreatesClosure(c->cond.get()) ||
                   exprCreatesClosure(c->elem.get());
        }
        case Expr::Kind::Literal:
        case Expr::Kind::Variable:
        case Expr::Kind::This:
        case Expr::Kind::Super:
            return false;
    }
    return false;
}

bool Resolver::canMerge(const Stmt* subtree) const {
    if (scopes.empty() || scopes.back().named) return false;  // no frame to merge into
    return !stmtCreatesClosure(subtree);
}

void Resolver::beginMerged() {
    Scope& s = scopes.back();
    marks.push_back(Mark{s.names, s.consts, s.declTypes, s.count});
}

void Resolver::endMerged() {
    Scope& s = scopes.back();
    Mark& m = marks.back();
    s.names = std::move(m.names);      // the merged scope's names go out of scope
    s.consts = std::move(m.consts);
    s.declTypes = std::move(m.declTypes);
    s.count = m.count;                 // slots are reusable; maxCount keeps the frame size
    marks.pop_back();
}

int Resolver::declare(const std::string& name) {
    Scope& s = scopes.back();
    if (s.named) return -1;
    int slot = s.count++;
    if (s.count > s.maxCount) s.maxCount = s.count;
    s.names[name] = slot;
    return slot;
}

void Resolver::recordType(const std::string& name, const TypeAnn& t) {
    if (!t.declared()) return;
    if (isNamedScope()) globalTypes[name] = t;
    else scopes.back().declTypes[name] = t;
}

TypeAnn Resolver::declaredTypeOf(const std::string& name) const {
    for (int i = (int)scopes.size() - 1; i >= 0; --i) {
        if (scopes[i].named) break;
        auto it = scopes[i].declTypes.find(name);
        if (it != scopes[i].declTypes.end()) return it->second;
        if (scopes[i].names.count(name)) return TypeAnn{};   // shadowed, unannotated
    }
    auto g = globalTypes.find(name);
    return g != globalTypes.end() ? g->second : TypeAnn{};
}

void Resolver::resolveNameUse(const std::string& name, int& depth, int& slot, bool& global) {
    int top = (int)scopes.size() - 1;
    for (int i = top; i >= 0; --i) {
        if (scopes[i].named) break; // reached the global/module scope => name-based
        auto it = scopes[i].names.find(name);
        if (it != scopes[i].names.end()) {
            depth = top - i;
            slot = it->second;
            global = false;
            return;
        }
    }
    depth = 0;
    slot = -1;
    global = true;
}

void Resolver::resolve(Program& program) {
    beginScope(/*named*/true); // global / module top-level
    resolveStatements(program);
    endScope();
}

void Resolver::resolveStatements(std::vector<StmtPtr>& stmts) {
    // Hoist function declarations so mutually-recursive local functions resolve.
    if (!isNamedScope()) {
        for (auto& sp : stmts) {
            if (sp->kind == Stmt::Kind::Function) {
                auto* fn = static_cast<FunctionStmt*>(sp.get());
                fn->nameSlot = declare(fn->name);
                fn->nameGlobal = false;
            }
        }
    }
    for (auto& sp : stmts) resolveStmt(sp.get());
}

void Resolver::resolveStmt(Stmt* s) {
    switch (s->kind) {
        case Stmt::Kind::Expression:
            resolveExpr(static_cast<ExprStmt*>(s)->expr.get());
            break;

        case Stmt::Kind::Let: {
            auto* ls = static_cast<LetStmt*>(s);
            if (ls->initializer) resolveExpr(ls->initializer.get()); // RHS sees the old binding
            if (ls->isDestructure) {
                ls->global = isNamedScope();
                for (auto& nm : ls->names) {
                    ls->nameSlots.push_back(declare(nm));
                    if (ls->isConst) markConst(nm);
                }
            } else {
                ls->slot = declare(ls->name);
                ls->global = (ls->slot < 0);
                if (ls->isConst) markConst(ls->name);
                recordType(ls->name, ls->type);
                if (ls->type.isSizedNum() && curFn_) curFn_->usesSized = true;
            }
            break;
        }

        case Stmt::Kind::Block: {
            auto* bs = static_cast<BlockStmt*>(s);
            if (!blockNeedsScope(bs->statements)) {
                bs->transparent = true;      // declares nothing: nothing to allocate
                bs->slotCount = 0;
                for (auto& st : bs->statements) resolveStmt(st.get());
            } else if (canMerge(s)) {
                // Declares names, but nothing here can capture them: put them in
                // the enclosing frame and skip the per-entry allocation. In a
                // loop body this is the difference between one allocation and
                // one per iteration.
                bs->transparent = true;
                bs->slotCount = 0;
                beginMerged();
                resolveStatements(bs->statements);
                endMerged();
            } else {
                bs->transparent = false;
                beginScope(false);
                resolveStatements(bs->statements);
                bs->slotCount = endScope();
            }
            break;
        }

        case Stmt::Kind::If: {
            auto* is = static_cast<IfStmt*>(s);
            resolveExpr(is->condition.get());
            resolveStmt(is->thenBranch.get());
            if (is->elseBranch) resolveStmt(is->elseBranch.get());
            break;
        }

        case Stmt::Kind::While: {
            auto* ws = static_cast<WhileStmt*>(s);
            resolveExpr(ws->condition.get());
            resolveStmt(ws->body.get());
            break;
        }

        case Stmt::Kind::For: {
            auto* fs = static_cast<ForStmt*>(s);
            fs->ownScope = !canMerge(s);
            if (!fs->ownScope) beginMerged(); else beginScope(false);
            if (fs->init) resolveStmt(fs->init.get());
            if (fs->condition) resolveExpr(fs->condition.get());
            if (fs->increment) resolveExpr(fs->increment.get());
            resolveStmt(fs->body.get()); // body is a Block => gets its own (possibly transparent) scope
            if (!fs->ownScope) { endMerged(); fs->slotCount = 0; }
            else fs->slotCount = endScope();
            break;
        }

        case Stmt::Kind::ForIn: {
            auto* fs = static_cast<ForInStmt*>(s);
            resolveExpr(fs->iterable.get()); // evaluated in the outer scope
            fs->ownScope = !canMerge(s);
            if (!fs->ownScope) beginMerged(); else beginScope(false);
            fs->varSlot = declare(fs->name);
            resolveStmt(fs->body.get());
            if (!fs->ownScope) { endMerged(); fs->slotCount = 0; }
            else fs->slotCount = endScope();
            break;
        }

        case Stmt::Kind::Function: {
            auto* fn = static_cast<FunctionStmt*>(s);
            if (isNamedScope()) { fn->nameSlot = -1; fn->nameGlobal = true; }
            // (in a slotted scope the name was already hoisted by resolveStatements)
            resolveFunction(fn, /*isMethod*/false, /*hasSuper*/false);
            break;
        }

        case Stmt::Kind::Return: {
            auto* rs = static_cast<ReturnStmt*>(s);
            if (rs->value) resolveExpr(rs->value.get());
            break;
        }

        case Stmt::Kind::Class:
            resolveClass(static_cast<ClassStmt*>(s));
            break;

        case Stmt::Kind::Import:
            // Import bindings stay name-based; nothing to resolve.
            break;

        case Stmt::Kind::Try: {
            auto* ts = static_cast<TryStmt*>(s);
            resolveStmt(ts->body.get());
            if (ts->hasCatch) {
                ts->catchOwnScope = !canMerge(ts->catchBody.get());
                if (!ts->catchOwnScope) beginMerged(); else beginScope(false);
                if (!ts->catchName.empty()) ts->catchSlot = declare(ts->catchName);
                resolveStmt(ts->catchBody.get());
                if (!ts->catchOwnScope) { endMerged(); ts->catchScopeSlots = 0; }
                else ts->catchScopeSlots = endScope();
            }
            if (ts->hasFinally) resolveStmt(ts->finallyBody.get());
            break;
        }

        case Stmt::Kind::Throw:
            resolveExpr(static_cast<ThrowStmt*>(s)->value.get());
            break;

        case Stmt::Kind::Match: {
            auto* ms = static_cast<MatchStmt*>(s);
            resolveExpr(ms->subject.get());
            for (auto& c : ms->cases) {
                for (auto& v : c.values) resolveExpr(v.get());
                resolveStmt(c.body.get());
            }
            if (ms->hasDefault) resolveStmt(ms->defaultBody.get());
            break;
        }

        case Stmt::Kind::Break:
        case Stmt::Kind::Continue:
            break;
    }
}

void Resolver::resolveFunction(FunctionStmt* fn, bool isMethod, bool hasSuper) {
    FunctionStmt* savedFn = curFn_;
    curFn_ = fn;
    // A sized numeric type anywhere in the signature (or, via the body handlers
    // below, inside it) makes the VM and JIT decline this function so the
    // tree-walker's wrapping semantics apply.
    fn->usesSized = fn->returnType.isSizedNum();
    for (const auto& t : fn->paramTypes) if (t.isSizedNum()) fn->usesSized = true;
    beginScope(false);
    if (isMethod) declare("this");    // slot 0
    if (hasSuper) declare("@super");  // slot 1
    fn->paramStart = scopes.back().count;
    for (size_t i = 0; i < fn->params.size(); ++i) {
        declare(fn->params[i]);
        if (i < fn->paramTypes.size()) recordType(fn->params[i], fn->paramTypes[i]);
    }
    for (auto& d : fn->defaults)      // default exprs may reference params / outer scope
        if (d) resolveExpr(d.get());
    resolveStatements(fn->body);      // params and body share the one frame scope
    fn->frameSlots = endScope();
    curFn_ = savedFn;
}

void Resolver::resolveClass(ClassStmt* c) {
    c->nameSlot = declare(c->name);
    c->nameGlobal = (c->nameSlot < 0);

    bool hasSuper = !c->superclassName.empty();
    if (hasSuper)
        resolveNameUse(c->superclassName, c->superDepth, c->superSlot, c->superGlobal);

    for (auto& m : c->methods)
        resolveFunction(m.get(), /*isMethod*/true, hasSuper);
}

void Resolver::resolveExpr(Expr* e) {
    switch (e->kind) {
        case Expr::Kind::Literal:
            break;

        case Expr::Kind::ListLit:
            for (auto& el : static_cast<ListLitExpr*>(e)->elements) resolveExpr(el.get());
            break;

        case Expr::Kind::DictLit:
            for (auto& kv : static_cast<DictLitExpr*>(e)->entries) {
                resolveExpr(kv.first.get());
                resolveExpr(kv.second.get());
            }
            break;

        case Expr::Kind::Variable: {
            auto* v = static_cast<VariableExpr*>(e);
            resolveNameUse(v->name, v->depth, v->slot, v->global);
            break;
        }

        case Expr::Kind::Assign: {
            auto* a = static_cast<AssignExpr*>(e);
            if (a->value) resolveExpr(a->value.get());
            if (isConstBinding(a->name))
                throw std::runtime_error("Resolve error (line " + std::to_string(a->line) +
                                         "): cannot assign to const '" + a->name + "'");
            resolveNameUse(a->name, a->depth, a->slot, a->global);
            a->declaredType = declaredTypeOf(a->name);
            if (a->declaredType.isSizedNum() && curFn_) curFn_->usesSized = true;
            break;
        }

        case Expr::Kind::Binary: {
            auto* b = static_cast<BinaryExpr*>(e);
            resolveExpr(b->left.get());
            resolveExpr(b->right.get());
            break;
        }

        case Expr::Kind::Logical: {
            auto* b = static_cast<LogicalExpr*>(e);
            resolveExpr(b->left.get());
            resolveExpr(b->right.get());
            break;
        }

        case Expr::Kind::Unary:
            resolveExpr(static_cast<UnaryExpr*>(e)->right.get());
            break;

        case Expr::Kind::Call: {
            auto* c = static_cast<CallExpr*>(e);
            resolveExpr(c->callee.get());
            for (auto& a : c->args) resolveExpr(a.get());
            break;
        }

        case Expr::Kind::Get:
            resolveExpr(static_cast<GetExpr*>(e)->object.get());
            break;

        case Expr::Kind::Set: {
            auto* st = static_cast<SetExpr*>(e);
            resolveExpr(st->object.get());
            resolveExpr(st->value.get());
            break;
        }

        case Expr::Kind::Index: {
            auto* ix = static_cast<IndexExpr*>(e);
            resolveExpr(ix->object.get());
            resolveExpr(ix->index.get());
            break;
        }
        case Expr::Kind::Slice: {
            auto* sl = static_cast<SliceExpr*>(e);
            resolveExpr(sl->object.get());
            if (sl->start) resolveExpr(sl->start.get());   // absent means "from 0"
            if (sl->end) resolveExpr(sl->end.get());       // absent means "to the end"
            break;
        }

        case Expr::Kind::IndexSet: {
            auto* ix = static_cast<IndexSetExpr*>(e);
            resolveExpr(ix->object.get());
            resolveExpr(ix->index.get());
            resolveExpr(ix->value.get());
            break;
        }

        case Expr::Kind::This: {
            auto* t = static_cast<ThisExpr*>(e);
            int slot; bool global;
            resolveNameUse("this", t->depth, slot, global);
            if (global) t->depth = -1; // used outside any method
            break;
        }

        case Expr::Kind::Super: {
            auto* su = static_cast<SuperExpr*>(e);
            int slot; bool global;
            resolveNameUse("@super", su->depth, slot, global);
            if (global) su->depth = -1;
            break;
        }

        case Expr::Kind::Grouping:
            resolveExpr(static_cast<GroupingExpr*>(e)->inner.get());
            break;

        case Expr::Kind::Ternary: {
            auto* t = static_cast<TernaryExpr*>(e);
            resolveExpr(t->cond.get());
            resolveExpr(t->thenBranch.get());
            resolveExpr(t->elseBranch.get());
            break;
        }

        case Expr::Kind::Function:
            resolveFunction(static_cast<FunctionExpr*>(e)->fn.get(), /*isMethod*/false, /*hasSuper*/false);
            break;

        case Expr::Kind::ListComp: {
            auto* c = static_cast<ListCompExpr*>(e);
            resolveExpr(c->iterable.get());  // evaluated in the outer scope
            c->ownScope = scopes.back().named ||
                          exprCreatesClosure(c->cond.get()) || exprCreatesClosure(c->elem.get());
            if (!c->ownScope) beginMerged(); else beginScope(false);
            c->varSlot = declare(c->name);
            if (c->cond) resolveExpr(c->cond.get());
            resolveExpr(c->elem.get());
            if (!c->ownScope) { endMerged(); c->slotCount = 0; }
            else c->slotCount = endScope();
            break;
        }
    }
}

} // namespace bee
