#include "interpreter.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "resolver.hpp"

#include <iostream>
#include <fstream>
#include <sstream>
#include <cmath>
#include <iomanip>
#include <cctype>

namespace bee {

// ------------------------------------------------------------------
// File-scope helpers
// ------------------------------------------------------------------

static std::string numToStr(double d) {
    if (std::isnan(d)) return "nan";
    if (std::isinf(d)) return d < 0 ? "-inf" : "inf";
    if (d == 0) d = 0; // normalise -0
    double r = std::floor(d);
    if (d == r && std::fabs(d) < 1e15) {
        long long i = (long long)d;
        return std::to_string(i);
    }
    std::ostringstream os;
    os << std::setprecision(15) << d;
    return os.str();
}

static std::string dirOf(const std::string& path) {
    size_t p = path.find_last_of("/\\");
    if (p == std::string::npos) return ".";
    return path.substr(0, p);
}

static bool readFileContents(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

static double numArg(const Value& v, const std::string& who) {
    if (!v.isNumber()) throw RuntimeError(who + ": expected a number");
    return v.asNumber();
}

static Value nativeMethod(const std::string& name, int arity,
                          std::function<Value(Interpreter&, std::vector<Value>&)> f) {
    auto b = std::make_shared<Builtin>();
    b->name = name;
    b->arity = arity;
    b->fn = std::move(f);
    return Value(b);
}

// ------------------------------------------------------------------
// Construction / errors
// ------------------------------------------------------------------

Interpreter::Interpreter() {
    globals = std::make_shared<Environment>();
    rng.seed(std::random_device{}());
    defineBuiltins();
    defineSystemBuiltins();
    defineExtraBuiltins();
}

void Interpreter::joinAllThreads() {
    std::vector<std::shared_ptr<ThreadRec>> recs;
    for (auto& kv : threads) recs.push_back(kv.second);
    threads.clear();
    gilRelease();
    for (auto& r : recs)
        if (r->th.joinable()) r->th.join();
    gilAcquire();
}

void Interpreter::error(const std::string& msg, int line) {
    if (line > 0)
        throw RuntimeError("Runtime error (line " + std::to_string(line) + "): " + msg);
    throw RuntimeError("Runtime error: " + msg);
}

std::string Interpreter::keyString(const Value& v) {
    return v.isString() ? v.asString() : stringify(v);
}

// ------------------------------------------------------------------
// Display
// ------------------------------------------------------------------

std::string Interpreter::stringify(const Value& v) {
    if (v.isNil())    return "nil";
    if (v.isBool())   return v.asBool() ? "true" : "false";
    if (v.isNumber()) return numToStr(v.asNumber());
    if (v.isString()) return v.asString();
    if (v.isList()) {
        auto l = v.asList();
        std::string s = "[";
        for (size_t i = 0; i < l->size(); ++i) {
            if (i) s += ", ";
            s += reprString((*l)[i]);
        }
        return s + "]";
    }
    if (v.isDict()) {
        auto d = v.asDict();
        std::string s = "{";
        bool first = true;
        for (auto& kv : *d) {
            if (!first) s += ", ";
            first = false;
            s += reprString(Value(kv.first));
            s += ": ";
            s += reprString(kv.second);
        }
        return s + "}";
    }
    if (v.isFunction()) {
        auto f = v.asFunction();
        std::string n = !f->name.empty() ? f->name : (f->decl ? f->decl->name : "");
        return "<fn " + n + ">";
    }
    if (v.isBuiltin()) return "<builtin " + v.asBuiltin()->name + ">";
    if (v.isClass())   return "<class " + v.asClass()->name + ">";
    if (v.isInstance()) {
        auto inst = v.asInstance();
        auto m = inst->klass->findMethod("str");
        if (m) {
            auto bound = bindMethod(m, inst, inst->klass);
            std::vector<Value> noargs;
            Value r = callFunction(bound, noargs, 0);
            if (r.isString()) return r.asString();
        }
        return "<" + inst->klass->name + " instance>";
    }
    if (v.isModule()) return "<module " + v.asModule()->name + ">";
    return "?";
}

std::string Interpreter::reprString(const Value& v) {
    if (v.isString()) {
        std::string s = "\"";
        for (char c : v.asString()) {
            switch (c) {
                case '\\': s += "\\\\"; break;
                case '"':  s += "\\\""; break;
                case '\n': s += "\\n"; break;
                case '\t': s += "\\t"; break;
                case '\r': s += "\\r"; break;
                default:   s += c;
            }
        }
        return s + "\"";
    }
    return stringify(v);
}

// ------------------------------------------------------------------
// Equality
// ------------------------------------------------------------------

bool Interpreter::valuesEqual(const Value& a, const Value& b) {
    if (a.isNil() && b.isNil())       return true;
    if (a.isBool() && b.isBool())     return a.asBool() == b.asBool();
    if (a.isNumber() && b.isNumber()) return a.asNumber() == b.asNumber();
    if (a.isString() && b.isString()) return a.asString() == b.asString();
    if (a.isList() && b.isList()) {
        auto x = a.asList(), y = b.asList();
        if (x == y) return true;
        if (x->size() != y->size()) return false;
        for (size_t i = 0; i < x->size(); ++i)
            if (!valuesEqual((*x)[i], (*y)[i])) return false;
        return true;
    }
    if (a.isDict() && b.isDict()) {
        auto x = a.asDict(), y = b.asDict();
        if (x == y) return true;
        if (x->size() != y->size()) return false;
        for (auto& kv : *x) {
            auto it = y->find(kv.first);
            if (it == y->end() || !valuesEqual(kv.second, it->second)) return false;
        }
        return true;
    }
    if (a.isInstance() && b.isInstance()) return a.asInstance() == b.asInstance();
    if (a.isClass() && b.isClass())       return a.asClass() == b.asClass();
    if (a.isFunction() && b.isFunction()) return a.asFunction() == b.asFunction();
    if (a.isBuiltin() && b.isBuiltin())   return a.asBuiltin() == b.asBuiltin();
    if (a.isModule() && b.isModule())     return a.asModule() == b.asModule();
    return false;
}

// ------------------------------------------------------------------
// Program / statement execution
// ------------------------------------------------------------------

void Interpreter::runFile(const std::string& path) {
    std::string src;
    if (!readFileContents(path, src))
        throw RuntimeError("cannot open file '" + path + "'");

    currentDir = dirOf(path);
    searchPaths.clear();
    searchPaths.push_back(currentDir);
    searchPaths.push_back(currentDir + "/lib");

    auto program = std::make_unique<Program>();
    {
        Lexer lx(src);
        auto toks = lx.tokenize();
        Parser ps(std::move(toks));
        *program = ps.parse();
    }
    Resolver().resolve(*program);
    Program* prog = program.get();
    programStore.push_back(std::move(program));

    // Hold the GIL for the whole main-thread run; spawned threads and blocking
    // built-ins hand it back and forth. `guard` guarantees we join outstanding
    // threads and release the lock however we leave this function.
    gil.lock();
    struct Guard {
        Interpreter* it;
        ~Guard() { it->joinAllThreads(); it->gil.unlock(); }
    } guard{ this };

    try {
        execProgram(*prog, globals);
    } catch (ReturnSignal&) {
        // A top-level `return` just ends the program.
    } catch (BreakSignal&) {
        throw RuntimeError("Runtime error: 'break' used outside of a loop");
    } catch (ContinueSignal&) {
        throw RuntimeError("Runtime error: 'continue' used outside of a loop");
    } catch (BeeThrow& t) {
        throw RuntimeError("Uncaught: " + stringify(t.value));
    }
}

void Interpreter::execProgram(const Program& program, std::shared_ptr<Environment> env) {
    for (auto& s : program) execute(s.get(), env);
}

void Interpreter::execBlock(const std::vector<StmtPtr>& stmts, std::shared_ptr<Environment> env) {
    for (auto& s : stmts) execute(s.get(), env);
}

bool Interpreter::tryJitLoop(Stmt* stmt, std::shared_ptr<Environment>& env) {
    // Only top-level loops (whose outer variables are named globals) qualify;
    // inside functions, loop variables are slots and the function-level JIT
    // already applies.
    if (env.get() != globals.get()) return false;

    const CompiledLoop& cl = jit.getCompiledLoop(stmt, *this);
    if (!cl.fn) return false;

    // Gather the current values of the numeric globals the loop touches. If any
    // isn't a number right now, interpret instead.
    size_t n = cl.globals.size();
    std::vector<double> vars(n ? n : 1);
    std::vector<Value*> slots(n);
    for (size_t i = 0; i < n; ++i) {
        Value* p = globals->findNameSlot(cl.globals[i]);
        if (!p || !p->isNumber()) return false;
        slots[i] = p;
        vars[i] = p->asNumber();
    }

    int bail = 0;
    cl.fn(vars.data(), (int)n, this, &bail);
    if (bail) return false;   // native code gave up: re-run from the original state

    for (size_t i = 0; i < n; ++i) *slots[i] = Value(vars[i]);
    return true;
}

Value* Interpreter::selfStringAppend(AssignExpr* e, std::shared_ptr<Environment>& env) {
    if (!e->value || e->value->kind != Expr::Kind::Binary) return nullptr;
    auto* bin = static_cast<BinaryExpr*>(e->value.get());
    if (bin->op != TokenType::PLUS || bin->left->kind != Expr::Kind::Variable) return nullptr;
    auto* lv = static_cast<VariableExpr*>(bin->left.get());

    Value* slot = nullptr;
    if (e->global) {
        if (!lv->global || lv->name != e->name) return nullptr;
        slot = env->findNameSlot(e->name);
    } else {
        if (lv->global || lv->depth != e->depth || lv->slot != e->slot) return nullptr;
        slot = &env->ancestor(e->depth)->slots[(size_t)e->slot];
    }
    if (!slot || !slot->isString()) return nullptr;
    return slot;
}

void Interpreter::execute(Stmt* stmt, std::shared_ptr<Environment>& env) {
    switch (stmt->kind) {
        case Stmt::Kind::Expression: {
            auto* s = static_cast<ExprStmt*>(stmt);
            evaluate(s->expr.get(), env);
            break;
        }
        case Stmt::Kind::Let: {
            auto* s = static_cast<LetStmt*>(stmt);
            Value v = s->initializer ? evaluate(s->initializer.get(), env) : Value();
            if (!s->isDestructure) {
                if (s->global) env->define(s->name, v);
                else env->slots[(size_t)s->slot] = v;
            } else if (s->destructureDict) {
                if (!v.isDict()) error("dict destructuring requires a dict", s->line);
                auto d = v.asDict();
                for (size_t i = 0; i < s->names.size(); ++i) {
                    auto it = d->find(s->names[i]);
                    Value val = it != d->end() ? it->second : Value();
                    if (s->global) env->define(s->names[i], val);
                    else env->slots[(size_t)s->nameSlots[i]] = val;
                }
            } else {
                if (!v.isList()) error("list destructuring requires a list", s->line);
                auto l = v.asList();
                for (size_t i = 0; i < s->names.size(); ++i) {
                    Value val = i < l->size() ? (*l)[i] : Value();
                    if (s->global) env->define(s->names[i], val);
                    else env->slots[(size_t)s->nameSlots[i]] = val;
                }
            }
            break;
        }
        case Stmt::Kind::Block: {
            auto* s = static_cast<BlockStmt*>(stmt);
            if (s->transparent) {
                execBlock(s->statements, env);
            } else {
                auto child = std::make_shared<Environment>(env, s->slotCount);
                execBlock(s->statements, child);
            }
            break;
        }
        case Stmt::Kind::If: {
            auto* s = static_cast<IfStmt*>(stmt);
            if (evaluate(s->condition.get(), env).truthy())
                execute(s->thenBranch.get(), env);
            else if (s->elseBranch)
                execute(s->elseBranch.get(), env);
            break;
        }
        case Stmt::Kind::While: {
            auto* s = static_cast<WhileStmt*>(stmt);
            if (tryJitLoop(stmt, env)) break;
            while (evaluate(s->condition.get(), env).truthy()) {
                try {
                    execute(s->body.get(), env);
                } catch (BreakSignal&) {
                    break;
                } catch (ContinueSignal&) {
                    // fall through to re-test condition
                }
            }
            break;
        }
        case Stmt::Kind::For: {
            auto* s = static_cast<ForStmt*>(stmt);
            if (tryJitLoop(stmt, env)) break;
            auto loopEnv = std::make_shared<Environment>(env, s->slotCount);
            if (s->init) execute(s->init.get(), loopEnv);
            while (s->condition ? evaluate(s->condition.get(), loopEnv).truthy() : true) {
                try {
                    execute(s->body.get(), loopEnv);
                } catch (BreakSignal&) {
                    break;
                } catch (ContinueSignal&) {
                    // fall through to increment
                }
                if (s->increment) evaluate(s->increment.get(), loopEnv);
            }
            break;
        }
        case Stmt::Kind::ForIn: {
            auto* s = static_cast<ForInStmt*>(stmt);
            Value iter = evaluate(s->iterable.get(), env);
            auto loopEnv = std::make_shared<Environment>(env, s->slotCount);

            auto runOne = [&](const Value& item) -> bool {
                loopEnv->slots[(size_t)s->varSlot] = item;
                try {
                    execute(s->body.get(), loopEnv);
                } catch (BreakSignal&) {
                    return false;
                } catch (ContinueSignal&) {
                }
                return true;
            };

            if (iter.isList()) {
                auto l = iter.asList();
                for (size_t i = 0; i < l->size(); ++i)
                    if (!runOne((*l)[i])) break;
            } else if (iter.isString()) {
                const std::string& str = iter.asString();
                for (char c : str)
                    if (!runOne(Value(std::string(1, c)))) break;
            } else if (iter.isDict()) {
                auto d = iter.asDict();
                for (auto& kv : *d)
                    if (!runOne(Value(kv.first))) break;
            } else {
                error("value is not iterable", s->line);
            }
            break;
        }
        case Stmt::Kind::Function: {
            auto* s = static_cast<FunctionStmt*>(stmt);
            auto fn = std::make_shared<Function>();
            fn->decl = s;
            fn->closure = env;
            fn->name = s->name;
            if (s->nameGlobal) env->define(s->name, Value(fn));
            else env->slots[(size_t)s->nameSlot] = Value(fn);
            break;
        }
        case Stmt::Kind::Return: {
            auto* s = static_cast<ReturnStmt*>(stmt);
            Value v = s->value ? evaluate(s->value.get(), env) : Value();
            throw ReturnSignal{v};
        }
        case Stmt::Kind::Class:
            execClass(static_cast<ClassStmt*>(stmt), env);
            break;
        case Stmt::Kind::Import:
            execImport(static_cast<ImportStmt*>(stmt), env);
            break;
        case Stmt::Kind::Match: {
            auto* s = static_cast<MatchStmt*>(stmt);
            Value subj = evaluate(s->subject.get(), env);
            bool matched = false;
            for (auto& c : s->cases) {
                for (auto& v : c.values) {
                    if (valuesEqual(subj, evaluate(v.get(), env))) {
                        execute(c.body.get(), env);
                        matched = true;
                        break;
                    }
                }
                if (matched) break;
            }
            if (!matched && s->hasDefault) execute(s->defaultBody.get(), env);
            break;
        }
        case Stmt::Kind::Try:
            execTry(static_cast<TryStmt*>(stmt), env);
            break;
        case Stmt::Kind::Throw: {
            auto* s = static_cast<ThrowStmt*>(stmt);
            throw BeeThrow{ evaluate(s->value.get(), env) };
        }
        case Stmt::Kind::Break:
            throw BreakSignal{};
        case Stmt::Kind::Continue:
            throw ContinueSignal{};
    }
}

void Interpreter::runCatch(TryStmt* s, const Value& err, std::shared_ptr<Environment>& env) {
    auto catchEnv = std::make_shared<Environment>(env, s->catchScopeSlots);
    if (!s->catchName.empty())
        catchEnv->slots[(size_t)s->catchSlot] = err;
    execute(s->catchBody.get(), catchEnv);
}

void Interpreter::execTry(TryStmt* s, std::shared_ptr<Environment>& env) {
    // `finally` must run on every exit path: normal completion, a handled or
    // rethrown Bee exception, or a return/break/continue tunnelling through.
    try {
        try {
            execute(s->body.get(), env);
        } catch (BeeThrow& t) {
            if (!s->hasCatch) throw;
            runCatch(s, t.value, env);
        } catch (RuntimeError& e) {
            if (!s->hasCatch) throw;
            runCatch(s, Value(std::string(e.what())), env); // built-in errors caught as their message string
        }
    } catch (...) {
        if (s->hasFinally) execute(s->finallyBody.get(), env);
        throw;
    }
    if (s->hasFinally) execute(s->finallyBody.get(), env);
}

void Interpreter::execClass(ClassStmt* c, std::shared_ptr<Environment>& env) {
    std::shared_ptr<Class> superclass;
    if (!c->superclassName.empty()) {
        Value sv;
        if (c->superGlobal) {
            if (!env->tryGetName(c->superclassName, sv))
                error("superclass '" + c->superclassName + "' is not defined", c->line);
        } else {
            sv = env->getAt(c->superDepth, c->superSlot);
        }
        if (!sv.isClass())
            error("superclass '" + c->superclassName + "' is not a class", c->line);
        superclass = sv.asClass();
    }

    auto klass = std::make_shared<Class>();
    klass->name = c->name;
    klass->superclass = superclass;

    // Methods close over the class's defining environment. `this` and (when a
    // superclass exists) `super` are reserved as hidden frame slots, populated
    // per call -- so `super` always refers to *this* class's superclass,
    // regardless of the runtime type of the receiver.
    for (auto& m : c->methods) {
        auto fn = std::make_shared<Function>();
        fn->decl = m.get();
        fn->closure = env;
        fn->name = m->name;
        fn->isInitializer = (m->name == "init");
        fn->definingClass = klass;
        klass->methods[m->name] = fn;
    }

    if (c->nameGlobal) env->define(c->name, Value(klass));
    else env->slots[(size_t)c->nameSlot] = Value(klass);
}

void Interpreter::execImport(ImportStmt* s, std::shared_ptr<Environment>& env) {
    auto mod = loadModule(s->moduleName, s->line);

    if (s->isFrom) {
        if (s->importAll) {
            for (auto& kv : mod->env->values) {
                if (!kv.first.empty() && kv.first[0] == '_') continue; // skip privates
                env->define(kv.first, kv.second);
            }
        } else {
            for (auto& [name, alias] : s->names) {
                Value v;
                if (!mod->env->tryGetName(name, v))
                    error("module '" + s->moduleName + "' has no member '" + name + "'", s->line);
                env->define(alias, v);
            }
        }
    } else {
        env->define(s->bindName, Value(mod));
    }
}

// ------------------------------------------------------------------
// Modules
// ------------------------------------------------------------------

std::string Interpreter::resolveModulePath(const std::string& moduleName) {
    std::vector<std::string> candidates;
    auto add = [&](const std::string& base) {
        candidates.push_back(base + "/" + moduleName + ".bee");
        candidates.push_back(base + "/" + moduleName + ".be");
        candidates.push_back(base + "/" + moduleName);
    };
    add(currentDir);
    for (auto& sp : searchPaths) add(sp);
    candidates.push_back(moduleName + ".bee");
    candidates.push_back(moduleName + ".be");
    candidates.push_back(moduleName);

    for (auto& c : candidates) {
        std::ifstream f(c);
        if (f.good()) return c;
    }
    return "";
}

std::shared_ptr<Module> Interpreter::loadModule(const std::string& moduleName, int line) {
    std::string path = resolveModulePath(moduleName);
    if (path.empty())
        error("cannot find module '" + moduleName + "'", line);

    auto cached = moduleCache.find(path);
    if (cached != moduleCache.end()) return cached->second;

    std::string src;
    if (!readFileContents(path, src))
        error("cannot read module '" + moduleName + "'", line);

    auto program = std::make_unique<Program>();
    try {
        Lexer lx(src);
        auto toks = lx.tokenize();
        Parser ps(std::move(toks));
        *program = ps.parse();
    } catch (const std::exception& e) {
        error("while loading module '" + moduleName + "': " + e.what(), line);
    }
    Resolver().resolve(*program);

    auto modEnv = std::make_shared<Environment>(globals);
    auto mod = std::make_shared<Module>();
    mod->name = moduleName;
    mod->path = path;
    mod->env = modEnv;
    moduleCache[path] = mod; // cache early so cyclic imports see a partial module

    Program* prog = program.get();
    programStore.push_back(std::move(program));

    std::string savedDir = currentDir;
    currentDir = dirOf(path);
    try {
        execProgram(*prog, modEnv);
    } catch (...) {
        currentDir = savedDir;
        throw;
    }
    currentDir = savedDir;
    return mod;
}

// ------------------------------------------------------------------
// Expression evaluation
// ------------------------------------------------------------------

Value Interpreter::evaluate(Expr* expr, std::shared_ptr<Environment>& env) {
    switch (expr->kind) {
        case Expr::Kind::Literal:
            return static_cast<LiteralExpr*>(expr)->value;

        case Expr::Kind::ListLit: {
            auto* e = static_cast<ListLitExpr*>(expr);
            auto list = std::make_shared<ValueList>();
            list->reserve(e->elements.size());
            for (size_t i = 0; i < e->elements.size(); ++i) {
                Value v = evaluate(e->elements[i].get(), env);
                if (i < e->spread.size() && e->spread[i]) {
                    if (!v.isList()) error("spread element (...) must be a list", e->line);
                    for (auto& x : *v.asList()) list->push_back(x);
                } else {
                    list->push_back(v);
                }
            }
            return Value(list);
        }

        case Expr::Kind::DictLit: {
            auto* e = static_cast<DictLitExpr*>(expr);
            auto dict = std::make_shared<ValueDict>();
            for (auto& entry : e->entries) {
                Value k = evaluate(entry.first.get(), env);
                Value v = evaluate(entry.second.get(), env);
                (*dict)[keyString(k)] = v;
            }
            return Value(dict);
        }

        case Expr::Kind::Variable: {
            auto* e = static_cast<VariableExpr*>(expr);
            if (!e->global) return env->getAt(e->depth, e->slot);
            // Inline cache: re-resolve only when the base environment changes.
            Environment* base = env.get();
            if (e->cacheEnv != base) {
                Value* slot = base->findNameSlot(e->name);
                if (!slot) error("undefined variable '" + e->name + "'", e->line);
                e->cacheEnv = base;
                e->cacheSlot = slot;
            }
            return *e->cacheSlot;
        }

        case Expr::Kind::Assign: {
            auto* e = static_cast<AssignExpr*>(expr);

            // Fast path: `x = x + rhs` (x a string). Capture x's buffer, then
            // append rhs. If x's string is uniquely owned once its own slot is
            // released, grow it in place; otherwise (an alias like `let a = x`
            // exists) fall back to allocating a fresh string, preserving value
            // semantics. This is O(1) amortized instead of O(n) per iteration.
            if (Value* slot = selfStringAppend(e, env)) {
                auto* bin = static_cast<BinaryExpr*>(e->value.get());
                auto sp = std::get<std::shared_ptr<std::string>>(slot->data); // old value
                std::string add;
                { Value rv = evaluate(bin->right.get(), env); add = stringify(rv); }
                *slot = Value();                        // drop the slot's own reference
                if (sp.use_count() == 1) { *sp += add; slot->data = sp; }   // unique: in place
                else slot->data = std::make_shared<std::string>(*sp + add); // shared: copy
                return *slot;
            }

            Value v = e->value ? evaluate(e->value.get(), env) : Value();
            if (!e->global) {
                env->setAt(e->depth, e->slot, v);
            } else {
                Environment* base = env.get();
                if (e->cacheEnv != base) {
                    Value* slot = base->findNameSlot(e->name);
                    if (!slot)
                        error("cannot assign to undefined variable '" + e->name + "'", e->line);
                    e->cacheEnv = base;
                    e->cacheSlot = slot;
                }
                *e->cacheSlot = v;
            }
            return v;
        }

        case Expr::Kind::Binary:
            return evalBinary(static_cast<BinaryExpr*>(expr), env);

        case Expr::Kind::Logical: {
            auto* e = static_cast<LogicalExpr*>(expr);
            Value l = evaluate(e->left.get(), env);
            if (e->op == TokenType::OR)
                return l.truthy() ? l : evaluate(e->right.get(), env);
            return l.truthy() ? evaluate(e->right.get(), env) : l;
        }

        case Expr::Kind::Unary: {
            auto* e = static_cast<UnaryExpr*>(expr);
            Value r = evaluate(e->right.get(), env);
            if (e->op == TokenType::MINUS) {
                if (!r.isNumber()) error("operand of unary '-' must be a number", e->line);
                return Value(-r.asNumber());
            }
            if (e->op == TokenType::BIT_NOT) {
                if (!r.isNumber()) error("operand of unary '~' must be a number", e->line);
                return Value((double)(~(long long)r.asNumber()));
            }
            return Value(!r.truthy()); // NOT
        }

        case Expr::Kind::Call:
            return evalCall(static_cast<CallExpr*>(expr), env);

        case Expr::Kind::Get:
            return evalGet(static_cast<GetExpr*>(expr), env);

        case Expr::Kind::Set: {
            auto* e = static_cast<SetExpr*>(expr);
            Value obj = evaluate(e->object.get(), env);
            if (!obj.isInstance())
                error("only instances have fields", e->line);
            auto inst = obj.asInstance();
            Value rhs = evaluate(e->value.get(), env);
            Value newVal;
            if (e->op == TokenType::ASSIGN) {
                newVal = rhs;
            } else {
                auto it = inst->fields.find(e->name);
                if (it == inst->fields.end())
                    error("compound assignment to undefined field '" + e->name + "'", e->line);
                newVal = applyBinaryArith(e->op, it->second, rhs, e->line);
            }
            inst->fields[e->name] = newVal;
            return newVal;
        }

        case Expr::Kind::Index:
            return evalIndex(static_cast<IndexExpr*>(expr), env);

        case Expr::Kind::IndexSet: {
            auto* e = static_cast<IndexSetExpr*>(expr);
            Value obj = evaluate(e->object.get(), env);
            Value idx = evaluate(e->index.get(), env);
            Value rhs = evaluate(e->value.get(), env);
            if (obj.isList()) {
                auto lst = obj.asList();
                if (!idx.isNumber()) error("list index must be a number", e->line);
                long long i = (long long)idx.asNumber();
                if (i < 0) i += (long long)lst->size();
                if (i < 0 || i >= (long long)lst->size())
                    error("list index out of range", e->line);
                Value newVal = (e->op == TokenType::ASSIGN)
                    ? rhs : applyBinaryArith(e->op, (*lst)[(size_t)i], rhs, e->line);
                (*lst)[(size_t)i] = newVal;
                return newVal;
            } else if (obj.isDict()) {
                auto d = obj.asDict();
                std::string key = keyString(idx);
                Value newVal;
                if (e->op == TokenType::ASSIGN) {
                    newVal = rhs;
                } else {
                    auto it = d->find(key);
                    Value cur = it != d->end() ? it->second : Value();
                    newVal = applyBinaryArith(e->op, cur, rhs, e->line);
                }
                (*d)[key] = newVal;
                return newVal;
            }
            error("cannot assign by index to this type", e->line);
        }

        case Expr::Kind::This: {
            auto* e = static_cast<ThisExpr*>(expr);
            if (e->depth < 0)
                error("'this' used outside of a method", expr->line);
            return env->getAt(e->depth, 0); // `this` is slot 0 of the method frame
        }

        case Expr::Kind::Super: {
            auto* e = static_cast<SuperExpr*>(expr);
            if (e->depth < 0)
                error("'super' used outside of a subclass method", e->line);
            Value superV = env->getAt(e->depth, 1); // superclass is slot 1
            Value thisV = env->getAt(e->depth, 0);  // `this` is slot 0
            auto superClass = superV.asClass();
            auto m = superClass->findMethod(e->method);
            if (!m)
                error("undefined method '" + e->method + "' on superclass", e->line);
            return Value(bindMethod(m, thisV.asInstance(), superClass));
        }

        case Expr::Kind::Grouping:
            return evaluate(static_cast<GroupingExpr*>(expr)->inner.get(), env);

        case Expr::Kind::Ternary: {
            auto* e = static_cast<TernaryExpr*>(expr);
            return evaluate(e->cond.get(), env).truthy()
                ? evaluate(e->thenBranch.get(), env)
                : evaluate(e->elseBranch.get(), env);
        }

        case Expr::Kind::Function: {
            auto* e = static_cast<FunctionExpr*>(expr);
            auto fn = std::make_shared<Function>();
            fn->decl = e->fn.get();
            fn->closure = env;
            fn->name = ""; // anonymous
            return Value(fn);
        }

        case Expr::Kind::ListComp: {
            auto* e = static_cast<ListCompExpr*>(expr);
            Value iter = evaluate(e->iterable.get(), env);
            auto out = std::make_shared<ValueList>();
            auto compEnv = std::make_shared<Environment>(env, e->slotCount);
            auto one = [&](const Value& item) {
                compEnv->slots[(size_t)e->varSlot] = item;
                if (!e->cond || evaluate(e->cond.get(), compEnv).truthy())
                    out->push_back(evaluate(e->elem.get(), compEnv));
            };
            if (iter.isList()) {
                for (auto& x : *iter.asList()) one(x);
            } else if (iter.isString()) {
                for (char c : iter.asString()) one(Value(std::string(1, c)));
            } else if (iter.isDict()) {
                for (auto& kv : *iter.asDict()) one(Value(kv.first));
            } else {
                error("value is not iterable", e->line);
            }
            return Value(out);
        }
    }
    error("unknown expression", expr->line);
}

Value Interpreter::applyBinaryArith(TokenType op, const Value& l, const Value& r, int line) {
    switch (op) {
        case TokenType::PLUS:
            if (l.isNumber() && r.isNumber()) return Value(l.asNumber() + r.asNumber());
            if (l.isList() && r.isList()) {
                auto out = std::make_shared<ValueList>(*l.asList());
                auto rr = r.asList();
                out->insert(out->end(), rr->begin(), rr->end());
                return Value(out);
            }
            if (l.isString() || r.isString()) return Value(stringify(l) + stringify(r));
            error("operands of '+' must be numbers, strings, or lists", line);
        case TokenType::MINUS:
            if (l.isNumber() && r.isNumber()) return Value(l.asNumber() - r.asNumber());
            error("operands of '-' must be numbers", line);
        case TokenType::STAR:
            if (l.isNumber() && r.isNumber()) return Value(l.asNumber() * r.asNumber());
            error("operands of '*' must be numbers", line);
        case TokenType::SLASH:
            if (!(l.isNumber() && r.isNumber())) error("operands of '/' must be numbers", line);
            if (r.asNumber() == 0) error("division by zero", line);
            return Value(l.asNumber() / r.asNumber());
        default:
            error("unsupported compound operator", line);
    }
}

Value Interpreter::evalBinary(BinaryExpr* e, std::shared_ptr<Environment>& env) {
    Value l = evaluate(e->left.get(), env);
    Value r = evaluate(e->right.get(), env);

    auto needNums = [&]() {
        if (!(l.isNumber() && r.isNumber()))
            error("operands must be numbers", e->line);
    };

    switch (e->op) {
        case TokenType::PLUS:
            if (l.isNumber() && r.isNumber()) return Value(l.asNumber() + r.asNumber());
            if (l.isList() && r.isList()) {
                auto out = std::make_shared<ValueList>(*l.asList());
                auto rr = r.asList();
                out->insert(out->end(), rr->begin(), rr->end());
                return Value(out);
            }
            if (l.isString() || r.isString())
                return Value(stringify(l) + stringify(r));
            error("operands of '+' must be numbers, strings, or lists", e->line);

        case TokenType::MINUS:
            needNums();
            return Value(l.asNumber() - r.asNumber());

        case TokenType::STAR:
            if (l.isNumber() && r.isNumber()) return Value(l.asNumber() * r.asNumber());
            if (l.isString() && r.isNumber()) {
                std::string out;
                long long n = (long long)r.asNumber();
                for (long long i = 0; i < n; ++i) out += l.asString();
                return Value(out);
            }
            if (l.isList() && r.isNumber()) {
                auto out = std::make_shared<ValueList>();
                long long n = (long long)r.asNumber();
                auto src = l.asList();
                for (long long i = 0; i < n; ++i)
                    out->insert(out->end(), src->begin(), src->end());
                return Value(out);
            }
            error("operands of '*' must be numbers (or string/list * number)", e->line);

        case TokenType::SLASH:
            needNums();
            if (r.asNumber() == 0) error("division by zero", e->line);
            return Value(l.asNumber() / r.asNumber());

        case TokenType::PERCENT:
            needNums();
            if (r.asNumber() == 0) error("modulo by zero", e->line);
            return Value(std::fmod(l.asNumber(), r.asNumber()));

        case TokenType::LT: case TokenType::GT:
        case TokenType::LE: case TokenType::GE: {
            int cmp;
            if (l.isNumber() && r.isNumber()) {
                double a = l.asNumber(), b = r.asNumber();
                cmp = (a < b) ? -1 : (a > b) ? 1 : 0;
            } else if (l.isString() && r.isString()) {
                int c = l.asString().compare(r.asString());
                cmp = (c < 0) ? -1 : (c > 0) ? 1 : 0;
            } else {
                error("comparison operands must both be numbers or both strings", e->line);
            }
            switch (e->op) {
                case TokenType::LT: return Value(cmp < 0);
                case TokenType::GT: return Value(cmp > 0);
                case TokenType::LE: return Value(cmp <= 0);
                default:            return Value(cmp >= 0); // GE
            }
        }

        case TokenType::EQ:  return Value(valuesEqual(l, r));
        case TokenType::NEQ: return Value(!valuesEqual(l, r));

        case TokenType::BIT_AND: case TokenType::BIT_OR: case TokenType::BIT_XOR:
        case TokenType::SHL: case TokenType::SHR: {
            needNums();
            long long a = (long long)l.asNumber();
            long long b = (long long)r.asNumber();
            switch (e->op) {
                case TokenType::BIT_AND: return Value((double)(a & b));
                case TokenType::BIT_OR:  return Value((double)(a | b));
                case TokenType::BIT_XOR: return Value((double)(a ^ b));
                case TokenType::SHL:     return Value((double)(a << b));
                default:                 return Value((double)(a >> b)); // SHR
            }
        }

        default:
            error("unknown binary operator", e->line);
    }
}

Value Interpreter::evalCall(CallExpr* e, std::shared_ptr<Environment>& env) {
    Value callee = evaluate(e->callee.get(), env);
    std::vector<Value> args;
    args.reserve(e->args.size());
    for (size_t i = 0; i < e->args.size(); ++i) {
        Value v = evaluate(e->args[i].get(), env);
        if (i < e->spread.size() && e->spread[i]) {
            if (!v.isList()) error("spread argument (...) must be a list", e->line);
            for (auto& x : *v.asList()) args.push_back(x);
        } else {
            args.push_back(v);
        }
    }
    return callValue(callee, args, e->line);
}

Value Interpreter::evalGet(GetExpr* e, std::shared_ptr<Environment>& env) {
    Value obj = evaluate(e->object.get(), env);
    return getProperty(obj, e->name, e->line);
}

Value Interpreter::evalIndex(IndexExpr* e, std::shared_ptr<Environment>& env) {
    Value obj = evaluate(e->object.get(), env);
    Value idx = evaluate(e->index.get(), env);

    if (obj.isList()) {
        auto lst = obj.asList();
        if (!idx.isNumber()) error("list index must be a number", e->line);
        long long i = (long long)idx.asNumber();
        if (i < 0) i += (long long)lst->size();
        if (i < 0 || i >= (long long)lst->size())
            error("list index out of range", e->line);
        return (*lst)[(size_t)i];
    }
    if (obj.isString()) {
        const std::string& s = obj.asString();
        if (!idx.isNumber()) error("string index must be a number", e->line);
        long long i = (long long)idx.asNumber();
        if (i < 0) i += (long long)s.size();
        if (i < 0 || i >= (long long)s.size())
            error("string index out of range", e->line);
        return Value(std::string(1, s[(size_t)i]));
    }
    if (obj.isDict()) {
        auto d = obj.asDict();
        auto it = d->find(keyString(idx));
        return it != d->end() ? it->second : Value();
    }
    error("cannot index this type", e->line);
}

// ------------------------------------------------------------------
// Calling
// ------------------------------------------------------------------

Value Interpreter::callValue(const Value& callee, std::vector<Value>& args, int line) {
    if (callee.isBuiltin()) {
        auto b = callee.asBuiltin();
        if (b->arity >= 0 && (int)args.size() != b->arity)
            error("'" + b->name + "' expects " + std::to_string(b->arity) +
                  " argument(s) but got " + std::to_string(args.size()), line);
        return b->fn(*this, args);
    }
    if (callee.isFunction())
        return callFunction(callee.asFunction(), args, line);
    if (callee.isClass()) {
        auto klass = callee.asClass();
        auto inst = std::make_shared<Instance>();
        inst->klass = klass;
        auto init = klass->findMethod("init");
        if (init) {
            auto bound = bindMethod(init, inst, klass);
            callFunction(bound, args, line);
        } else if (!args.empty()) {
            error("class '" + klass->name + "' has no constructor but got arguments", line);
        }
        return Value(inst);
    }
    error("value is not callable", line);
}

Value Interpreter::callFunction(std::shared_ptr<Function> fn, std::vector<Value>& args, int line) {
    const FunctionStmt* decl = fn->decl;
    std::string name = !fn->name.empty() ? fn->name : (decl->name.empty() ? "<anonymous>" : decl->name);
    size_t np = decl->params.size();
    size_t provided = args.size();
    int rest = decl->restParam;
    if (rest < 0 && provided > np)
        error("function '" + name + "' expects at most " + std::to_string(np) +
              " argument(s) but got " + std::to_string(provided), line);

    // Fast path: if this is a plain (non-method) function called with exactly
    // its numeric arguments, try the LLVM-compiled version. A `bail` means the
    // native code hit something it can't handle (e.g. division by zero); we
    // then fall through to the interpreter, which is safe because the JIT
    // subset has no side effects.
    if (!fn->boundThis && !fn->definingClass && rest < 0 && provided == np) {
        if (JitFn nf = jit.getCompiled(decl, *this)) {
            bool allNum = true;
            for (auto& a : args) if (!a.isNumber()) { allNum = false; break; }
            if (allNum) {
                std::vector<double> ds(np ? np : 1);
                for (size_t i = 0; i < np; ++i) ds[i] = args[i].asNumber();
                int bail = 0;
                double r = nf(ds.data(), (int)np, this, &bail);
                if (!bail) return Value(r);
            }
        }
    }

    // Frame layout (fixed by the resolver): [this?][super?][params...][locals...]
    auto frame = std::make_shared<Environment>(fn->closure, decl->frameSlots);
    if (fn->boundThis)
        frame->slots[0] = Value(fn->boundThis);
    if (fn->definingClass && fn->definingClass->superclass)
        frame->slots[1] = Value(fn->definingClass->superclass);
    int base = decl->paramStart;
    for (size_t i = 0; i < np; ++i) {
        if ((int)i == rest) {
            auto restList = std::make_shared<ValueList>();
            for (size_t j = i; j < provided; ++j) restList->push_back(args[j]);
            frame->slots[(size_t)base + i] = Value(restList);
        } else if (i < provided) {
            frame->slots[(size_t)base + i] = args[i];
        } else if (i < decl->defaults.size() && decl->defaults[i]) {
            frame->slots[(size_t)base + i] = evaluate(decl->defaults[i].get(), frame);
        } else {
            error("function '" + name + "' missing required argument '" + decl->params[i] + "'", line);
        }
    }

    try {
        for (auto& s : decl->body) execute(s.get(), frame);
    } catch (ReturnSignal& r) {
        if (fn->isInitializer && fn->boundThis) return Value(fn->boundThis);
        return r.value;
    }
    if (fn->isInitializer && fn->boundThis) return Value(fn->boundThis);
    return Value();
}

std::shared_ptr<Function> Interpreter::bindMethod(std::shared_ptr<Function> method,
                                                  std::shared_ptr<Instance> self,
                                                  std::shared_ptr<Class> definingClass) {
    // `this`/`super` are populated as frame slots at call time, so binding just
    // records the receiver. The method keeps its own definingClass (which drives
    // `super`), so we only fill it in as a fallback.
    auto bound = std::make_shared<Function>(*method);
    bound->boundThis = self;
    if (!bound->definingClass) bound->definingClass = definingClass;
    return bound;
}

// ------------------------------------------------------------------
// Property access (fields, methods, built-in type methods)
// ------------------------------------------------------------------

Value Interpreter::getProperty(const Value& object, const std::string& name, int line) {
    if (object.isInstance()) {
        auto inst = object.asInstance();
        auto it = inst->fields.find(name);
        if (it != inst->fields.end()) return it->second;
        auto m = inst->klass->findMethod(name);
        if (m) return Value(bindMethod(m, inst, inst->klass));
        error("undefined property '" + name + "'", line);
    }

    if (object.isModule()) {
        Value v;
        if (!object.asModule()->env->tryGetName(name, v))
            error("module '" + object.asModule()->name + "' has no member '" + name + "'", line);
        return v;
    }

    if (object.isString()) {
        const Value self = object;
        if (name == "len" || name == "length")
            return nativeMethod(name, 0, [self](Interpreter&, std::vector<Value>&) {
                return Value((double)self.asString().size());
            });
        if (name == "upper")
            return nativeMethod(name, 0, [self](Interpreter&, std::vector<Value>&) {
                std::string s = self.asString();
                for (auto& c : s) c = (char)std::toupper((unsigned char)c);
                return Value(s);
            });
        if (name == "lower")
            return nativeMethod(name, 0, [self](Interpreter&, std::vector<Value>&) {
                std::string s = self.asString();
                for (auto& c : s) c = (char)std::tolower((unsigned char)c);
                return Value(s);
            });
        if (name == "trim")
            return nativeMethod(name, 0, [self](Interpreter&, std::vector<Value>&) {
                const std::string& s = self.asString();
                size_t a = 0, b = s.size();
                while (a < b && std::isspace((unsigned char)s[a])) a++;
                while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
                return Value(s.substr(a, b - a));
            });
        if (name == "contains")
            return nativeMethod(name, 1, [self](Interpreter&, std::vector<Value>& a) {
                return Value(self.asString().find(a[0].asString()) != std::string::npos);
            });
        if (name == "starts_with")
            return nativeMethod(name, 1, [self](Interpreter&, std::vector<Value>& a) {
                const std::string& s = self.asString();
                const std::string& p = a[0].asString();
                return Value(s.size() >= p.size() && s.compare(0, p.size(), p) == 0);
            });
        if (name == "ends_with")
            return nativeMethod(name, 1, [self](Interpreter&, std::vector<Value>& a) {
                const std::string& s = self.asString();
                const std::string& p = a[0].asString();
                return Value(s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0);
            });
        if (name == "split")
            return nativeMethod(name, 1, [self](Interpreter&, std::vector<Value>& a) {
                const std::string& s = self.asString();
                std::string sep = a[0].asString();
                auto out = std::make_shared<ValueList>();
                if (sep.empty()) {
                    for (char c : s) out->push_back(Value(std::string(1, c)));
                } else {
                    size_t pos, prev = 0;
                    while ((pos = s.find(sep, prev)) != std::string::npos) {
                        out->push_back(Value(s.substr(prev, pos - prev)));
                        prev = pos + sep.size();
                    }
                    out->push_back(Value(s.substr(prev)));
                }
                return Value(out);
            });
        if (name == "replace")
            return nativeMethod(name, 2, [self](Interpreter&, std::vector<Value>& a) {
                const std::string& s = self.asString();
                std::string from = a[0].asString(), to = a[1].asString();
                if (from.empty()) return Value(s);
                std::string res;
                size_t pos, prev = 0;
                while ((pos = s.find(from, prev)) != std::string::npos) {
                    res += s.substr(prev, pos - prev);
                    res += to;
                    prev = pos + from.size();
                }
                res += s.substr(prev);
                return Value(res);
            });
        if (name == "substr")
            return nativeMethod(name, 2, [self](Interpreter&, std::vector<Value>& a) {
                const std::string& s = self.asString();
                long long start = (long long)numArg(a[0], "substr");
                long long len = (long long)numArg(a[1], "substr");
                if (start < 0) start += (long long)s.size();
                if (start < 0) start = 0;
                if (start > (long long)s.size()) start = (long long)s.size();
                if (len < 0) len = 0;
                return Value(s.substr((size_t)start, (size_t)len));
            });
        if (name == "to_num")
            return nativeMethod(name, 0, [self](Interpreter&, std::vector<Value>&) -> Value {
                try { return Value(std::stod(self.asString())); }
                catch (...) { throw RuntimeError("to_num: cannot parse '" + self.asString() + "'"); }
            });
        error("string has no property '" + name + "'", line);
    }

    if (object.isList()) {
        const Value self = object;
        if (name == "len" || name == "length")
            return nativeMethod(name, 0, [self](Interpreter&, std::vector<Value>&) {
                return Value((double)self.asList()->size());
            });
        if (name == "push" || name == "append")
            return nativeMethod(name, 1, [self](Interpreter&, std::vector<Value>& a) {
                self.asList()->push_back(a[0]);
                return self;
            });
        if (name == "pop")
            return nativeMethod(name, 0, [self](Interpreter&, std::vector<Value>&) -> Value {
                auto l = self.asList();
                if (l->empty()) throw RuntimeError("pop: list is empty");
                Value v = l->back();
                l->pop_back();
                return v;
            });
        if (name == "contains" || name == "includes")
            return nativeMethod(name, 1, [self](Interpreter& I, std::vector<Value>& a) {
                for (auto& x : *self.asList())
                    if (I.valuesEqual(x, a[0])) return Value(true);
                return Value(false);
            });
        if (name == "index_of")
            return nativeMethod(name, 1, [self](Interpreter& I, std::vector<Value>& a) {
                auto l = self.asList();
                for (size_t i = 0; i < l->size(); ++i)
                    if (I.valuesEqual((*l)[i], a[0])) return Value((double)i);
                return Value(-1.0);
            });
        if (name == "insert")
            return nativeMethod(name, 2, [self](Interpreter&, std::vector<Value>& a) -> Value {
                auto l = self.asList();
                long long i = (long long)numArg(a[0], "insert");
                if (i < 0) i += (long long)l->size();
                if (i < 0 || i > (long long)l->size()) throw RuntimeError("insert: index out of range");
                l->insert(l->begin() + i, a[1]);
                return self;
            });
        if (name == "remove_at")
            return nativeMethod(name, 1, [self](Interpreter&, std::vector<Value>& a) -> Value {
                auto l = self.asList();
                long long i = (long long)numArg(a[0], "remove_at");
                if (i < 0) i += (long long)l->size();
                if (i < 0 || i >= (long long)l->size()) throw RuntimeError("remove_at: index out of range");
                Value v = (*l)[(size_t)i];
                l->erase(l->begin() + i);
                return v;
            });
        if (name == "join")
            return nativeMethod(name, -1, [self](Interpreter& I, std::vector<Value>& a) -> Value {
                if (a.size() > 1) throw RuntimeError("join: expects () or (sep)");
                std::string sep = a.empty() ? "" : (a[0].isString() ? a[0].asString() : I.stringify(a[0]));
                std::string out;
                auto l = self.asList();
                for (size_t i = 0; i < l->size(); ++i) {
                    if (i) out += sep;
                    out += I.stringify((*l)[i]);
                }
                return Value(out);
            });
        error("list has no property '" + name + "'", line);
    }

    if (object.isDict()) {
        const Value self = object;
        if (name == "keys")
            return nativeMethod(name, 0, [self](Interpreter&, std::vector<Value>&) {
                auto out = std::make_shared<ValueList>();
                for (auto& kv : *self.asDict()) out->push_back(Value(kv.first));
                return Value(out);
            });
        if (name == "values")
            return nativeMethod(name, 0, [self](Interpreter&, std::vector<Value>&) {
                auto out = std::make_shared<ValueList>();
                for (auto& kv : *self.asDict()) out->push_back(kv.second);
                return Value(out);
            });
        if (name == "has")
            return nativeMethod(name, 1, [self](Interpreter& I, std::vector<Value>& a) {
                return Value(self.asDict()->count(I.keyString(a[0])) > 0);
            });
        if (name == "remove")
            return nativeMethod(name, 1, [self](Interpreter& I, std::vector<Value>& a) {
                self.asDict()->erase(I.keyString(a[0]));
                return self;
            });
        if (name == "len" || name == "length")
            return nativeMethod(name, 0, [self](Interpreter&, std::vector<Value>&) {
                return Value((double)self.asDict()->size());
            });
        if (name == "get")
            return nativeMethod(name, -1, [self](Interpreter& I, std::vector<Value>& a) -> Value {
                if (a.empty() || a.size() > 2) throw RuntimeError("get: expects (key) or (key, default)");
                auto d = self.asDict();
                auto it = d->find(I.keyString(a[0]));
                if (it != d->end()) return it->second;
                return a.size() == 2 ? a[1] : Value();
            });
        // Fall back to treating `.name` as a key lookup.
        auto d = object.asDict();
        auto it = d->find(name);
        if (it != d->end()) return it->second;
        error("dict has no key or method '" + name + "'", line);
    }

    error("value of this type has no property '" + name + "'", line);
}

// ------------------------------------------------------------------
// Built-in global functions
// ------------------------------------------------------------------

void Interpreter::defineBuiltins() {
    auto def = [&](const std::string& n, int arity,
                   std::function<Value(Interpreter&, std::vector<Value>&)> f) {
        auto b = std::make_shared<Builtin>();
        b->name = n;
        b->arity = arity;
        b->fn = std::move(f);
        globals->define(n, Value(b));
    };

    def("print", -1, [](Interpreter& I, std::vector<Value>& a) {
        for (size_t i = 0; i < a.size(); ++i) {
            if (i) std::cout << ' ';
            std::cout << I.stringify(a[i]);
        }
        std::cout << '\n';
        return Value();
    });

    def("write", -1, [](Interpreter& I, std::vector<Value>& a) {
        for (size_t i = 0; i < a.size(); ++i) {
            if (i) std::cout << ' ';
            std::cout << I.stringify(a[i]);
        }
        return Value();
    });

    def("len", 1, [](Interpreter&, std::vector<Value>& a) -> Value {
        const Value& v = a[0];
        if (v.isString()) return Value((double)v.asString().size());
        if (v.isList())   return Value((double)v.asList()->size());
        if (v.isDict())   return Value((double)v.asDict()->size());
        throw RuntimeError("len: expected a string, list, or dict");
    });

    def("type", 1, [](Interpreter&, std::vector<Value>& a) -> Value {
        const Value& v = a[0];
        if (v.isNil())     return Value(std::string("nil"));
        if (v.isBool())    return Value(std::string("bool"));
        if (v.isNumber())  return Value(std::string("number"));
        if (v.isString())  return Value(std::string("string"));
        if (v.isList())    return Value(std::string("list"));
        if (v.isDict())    return Value(std::string("dict"));
        if (v.isFunction() || v.isBuiltin()) return Value(std::string("function"));
        if (v.isClass())   return Value(std::string("class"));
        if (v.isInstance())return Value(v.asInstance()->klass->name);
        if (v.isModule())  return Value(std::string("module"));
        return Value(std::string("unknown"));
    });

    def("str", 1, [](Interpreter& I, std::vector<Value>& a) {
        return Value(I.stringify(a[0]));
    });

    def("repr", 1, [](Interpreter& I, std::vector<Value>& a) {
        return Value(I.reprString(a[0]));
    });

    def("num", 1, [](Interpreter&, std::vector<Value>& a) -> Value {
        const Value& v = a[0];
        if (v.isNumber()) return v;
        if (v.isBool())   return Value(v.asBool() ? 1.0 : 0.0);
        if (v.isString()) {
            try { return Value(std::stod(v.asString())); }
            catch (...) { throw RuntimeError("num: cannot parse '" + v.asString() + "'"); }
        }
        throw RuntimeError("num: cannot convert this type");
    });

    def("int", 1, [](Interpreter&, std::vector<Value>& a) -> Value {
        const Value& v = a[0];
        if (v.isNumber()) return Value(std::trunc(v.asNumber()));
        if (v.isString()) {
            try { return Value(std::trunc(std::stod(v.asString()))); }
            catch (...) { throw RuntimeError("int: cannot parse '" + v.asString() + "'"); }
        }
        throw RuntimeError("int: cannot convert this type");
    });

    def("bool", 1, [](Interpreter&, std::vector<Value>& a) {
        return Value(a[0].truthy());
    });

    def("abs", 1, [](Interpreter&, std::vector<Value>& a) {
        return Value(std::fabs(numArg(a[0], "abs")));
    });
    def("floor", 1, [](Interpreter&, std::vector<Value>& a) {
        return Value(std::floor(numArg(a[0], "floor")));
    });
    def("ceil", 1, [](Interpreter&, std::vector<Value>& a) {
        return Value(std::ceil(numArg(a[0], "ceil")));
    });
    def("round", 1, [](Interpreter&, std::vector<Value>& a) {
        return Value(std::round(numArg(a[0], "round")));
    });
    def("sqrt", 1, [](Interpreter&, std::vector<Value>& a) {
        return Value(std::sqrt(numArg(a[0], "sqrt")));
    });
    def("pow", 2, [](Interpreter&, std::vector<Value>& a) {
        return Value(std::pow(numArg(a[0], "pow"), numArg(a[1], "pow")));
    });

    def("min", -1, [](Interpreter&, std::vector<Value>& a) -> Value {
        std::vector<Value> items = (a.size() == 1 && a[0].isList()) ? *a[0].asList() : a;
        if (items.empty()) throw RuntimeError("min: needs at least one value");
        double best = numArg(items[0], "min");
        for (size_t i = 1; i < items.size(); ++i)
            best = std::min(best, numArg(items[i], "min"));
        return Value(best);
    });
    def("max", -1, [](Interpreter&, std::vector<Value>& a) -> Value {
        std::vector<Value> items = (a.size() == 1 && a[0].isList()) ? *a[0].asList() : a;
        if (items.empty()) throw RuntimeError("max: needs at least one value");
        double best = numArg(items[0], "max");
        for (size_t i = 1; i < items.size(); ++i)
            best = std::max(best, numArg(items[i], "max"));
        return Value(best);
    });

    def("range", -1, [](Interpreter&, std::vector<Value>& a) -> Value {
        double start = 0, stop = 0, step = 1;
        if (a.size() == 1) {
            stop = numArg(a[0], "range");
        } else if (a.size() == 2) {
            start = numArg(a[0], "range");
            stop = numArg(a[1], "range");
        } else if (a.size() == 3) {
            start = numArg(a[0], "range");
            stop = numArg(a[1], "range");
            step = numArg(a[2], "range");
        } else {
            throw RuntimeError("range: expects 1 to 3 arguments");
        }
        if (step == 0) throw RuntimeError("range: step cannot be zero");
        auto out = std::make_shared<ValueList>();
        if (step > 0) for (double x = start; x < stop; x += step) out->push_back(Value(x));
        else          for (double x = start; x > stop; x += step) out->push_back(Value(x));
        return Value(out);
    });

    def("push", 2, [](Interpreter&, std::vector<Value>& a) -> Value {
        if (!a[0].isList()) throw RuntimeError("push: first argument must be a list");
        a[0].asList()->push_back(a[1]);
        return a[0];
    });
    def("pop", 1, [](Interpreter&, std::vector<Value>& a) -> Value {
        if (!a[0].isList()) throw RuntimeError("pop: argument must be a list");
        auto l = a[0].asList();
        if (l->empty()) throw RuntimeError("pop: list is empty");
        Value v = l->back();
        l->pop_back();
        return v;
    });
    def("keys", 1, [](Interpreter&, std::vector<Value>& a) -> Value {
        if (!a[0].isDict()) throw RuntimeError("keys: argument must be a dict");
        auto out = std::make_shared<ValueList>();
        for (auto& kv : *a[0].asDict()) out->push_back(Value(kv.first));
        return Value(out);
    });
    def("values", 1, [](Interpreter&, std::vector<Value>& a) -> Value {
        if (!a[0].isDict()) throw RuntimeError("values: argument must be a dict");
        auto out = std::make_shared<ValueList>();
        for (auto& kv : *a[0].asDict()) out->push_back(kv.second);
        return Value(out);
    });

    def("ord", 1, [](Interpreter&, std::vector<Value>& a) -> Value {
        if (!a[0].isString() || a[0].asString().empty())
            throw RuntimeError("ord: expected a non-empty string");
        return Value((double)(unsigned char)a[0].asString()[0]);
    });
    def("chr", 1, [](Interpreter&, std::vector<Value>& a) {
        int n = (int)numArg(a[0], "chr");
        return Value(std::string(1, (char)n));
    });

    def("input", -1, [](Interpreter& I, std::vector<Value>& a) -> Value {
        if (!a.empty()) std::cout << I.stringify(a[0]);
        std::string line;
        if (!std::getline(std::cin, line)) return Value();
        return Value(line);
    });

    def("assert", -1, [](Interpreter& I, std::vector<Value>& a) -> Value {
        if (a.empty()) throw RuntimeError("assert: needs a condition");
        if (!a[0].truthy()) {
            std::string msg = a.size() > 1 ? I.stringify(a[1]) : "assertion failed";
            throw RuntimeError("assertion failed: " + msg);
        }
        return Value();
    });
}

} // namespace bee
