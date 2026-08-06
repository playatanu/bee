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

int Resolver::declare(const std::string& name) {
    Scope& s = scopes.back();
    if (s.named) return -1;
    int slot = s.count++;
    s.names[name] = slot;
    return slot;
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
            }
            break;
        }

        case Stmt::Kind::Block: {
            auto* bs = static_cast<BlockStmt*>(s);
            if (blockNeedsScope(bs->statements)) {
                bs->transparent = false;
                beginScope(false);
                resolveStatements(bs->statements);
                bs->slotCount = endScope();
            } else {
                bs->transparent = true;
                bs->slotCount = 0;
                for (auto& st : bs->statements) resolveStmt(st.get());
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
            beginScope(false);
            if (fs->init) resolveStmt(fs->init.get());
            if (fs->condition) resolveExpr(fs->condition.get());
            if (fs->increment) resolveExpr(fs->increment.get());
            resolveStmt(fs->body.get()); // body is a Block => gets its own (possibly transparent) scope
            fs->slotCount = endScope();
            break;
        }

        case Stmt::Kind::ForIn: {
            auto* fs = static_cast<ForInStmt*>(s);
            resolveExpr(fs->iterable.get()); // evaluated in the outer scope
            beginScope(false);
            fs->varSlot = declare(fs->name);
            resolveStmt(fs->body.get());
            fs->slotCount = endScope();
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
                beginScope(false);
                if (!ts->catchName.empty()) ts->catchSlot = declare(ts->catchName);
                resolveStmt(ts->catchBody.get());
                ts->catchScopeSlots = endScope();
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
    beginScope(false);
    if (isMethod) declare("this");    // slot 0
    if (hasSuper) declare("@super");  // slot 1
    fn->paramStart = scopes.back().count;
    for (auto& p : fn->params) declare(p);
    for (auto& d : fn->defaults)      // default exprs may reference params / outer scope
        if (d) resolveExpr(d.get());
    resolveStatements(fn->body);      // params and body share the one frame scope
    fn->frameSlots = endScope();
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
            beginScope(false);
            c->varSlot = declare(c->name);
            if (c->cond) resolveExpr(c->cond.get());
            resolveExpr(c->elem.get());
            c->slotCount = endScope();
            break;
        }
    }
}

} // namespace bee
