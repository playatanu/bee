//
// LLVM ORCv2 codegen for Bee's numeric subset. See jit.hpp for the contract.
//
// Only built when BEE_JIT is defined (i.e. when the Makefile found llvm-config).
//
// NOTE: bee::Value / bee::Function / bee::Module collide with the LLVM types of
// the same name, and inside `namespace bee` the Bee types win. So those three
// LLVM types are always written fully qualified (llvm::Value, etc.); all other
// LLVM names come in via `using namespace llvm`.
//
#ifdef BEE_JIT

#include "jit.hpp"
#include "interpreter.hpp"

#include "llvm/Config/llvm-config.h"   // LLVM_VERSION_MAJOR, for API differences
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Passes/PassBuilder.h"

#include <vector>
#include <string>
#include <set>
#include <cstdlib>

using namespace llvm;

namespace bee {

// A statically-typed SSA value produced by codegen.
//
// BUFFER carries two values, the data pointer and the element count, because an
// indexed read needs both. A buffer only ever arrives as a parameter -- nothing
// in the compiled subset produces one -- so it lives in a slot and is read from,
// never assigned.
enum class Ty { NUMBER, BOOL, BUFFER };
struct TVal {
    llvm::Value* v = nullptr;      // the double, the i1, or the double* base
    Ty ty = Ty::NUMBER;
    llvm::Value* len = nullptr;    // element count, for BUFFER
};

// Thrown internally to abort compilation of a function that leaves the subset.
struct JitBail {};

// jitCandidate() is a trivial, LLVM-free pre-filter and lives inline in jit.hpp,
// shared with the front end.

// ---- ORC JIT engine + backend (one per interpreter) ----------------------
// The concrete JitBackend the shared object hands back from bee_jit_create().
// The front end (jit.cpp) owns the caches and the eligibility pre-check; this
// does the LLVM work: standing up an ORC session and generating code.
struct LlvmJitBackend : JitBackend {
    std::unique_ptr<orc::LLJIT> jit;
    unsigned counter = 0;
    bool started = false;

    // Built on first use, not when the backend is created. Initialising the
    // native target and standing up an ORC session costs milliseconds; a caller
    // that dlopen'd us may still turn out to have nothing compilable, so this
    // stays off the load path.
    orc::LLJIT* engine() {
        if (!started) {
            started = true;
            InitializeNativeTarget();
            InitializeNativeTargetAsmPrinter();
            auto j = orc::LLJITBuilder().create();
            if (j) jit = std::move(*j);
        }
        return jit.get();
    }

    JitFn compile(const FunctionStmt* fn, JitSig sig, Interpreter& interp,
                  std::vector<JitCacheEntry>& extra) override;
    void compileLoop(const Stmt* loop, Interpreter& interp, CompiledLoop& out) override;
};

// ---- per-function code generator -----------------------------------------
namespace {

struct Scope {
    const void* owner;                       // AST node that owns this scope
    std::vector<AllocaInst*> allocas;        // per slot (null until declared)
    std::vector<AllocaInst*> lenAllocas;     // per slot, for BUFFER slots
    std::vector<Ty> types;                   // per slot
    Scope(const void* o, int n)
        : owner(o), allocas(n, nullptr), lenAllocas(n, nullptr), types(n, Ty::NUMBER) {}
};

// Owns one LLVM module into which a whole numeric call graph is compiled.
//
// A JIT-compiled function may call *other* numeric functions (helpers, mutual
// recursion). Those callees are compiled into the same module so the calls are
// direct and LLVM can inline across them. `declare()` creates a function's
// signature on first reference and enqueues its body for codegen; the driver in
// getCompiled() drains the worklist until the graph is closed.
struct ModuleCompiler {
    LLVMContext& ctx;
    llvm::Module& mod;
    Interpreter& interp;
    unsigned& counter;
    std::map<const FunctionStmt*, llvm::Function*> declared;
    std::vector<const FunctionStmt*> worklist;

    // The shared ABI (see jit.hpp):
    //   double f(double* nums, double** bufs, i64* bufLens, i8* interp, i32* bail)
    llvm::Function* declare(const FunctionStmt* fn) {
        auto it = declared.find(fn);
        if (it != declared.end()) return it->second;
        IRBuilder<> b(ctx);
        Type* dbl = b.getDoubleTy();
        Type* ptr = PointerType::get(ctx, 0);
        FunctionType* ft = FunctionType::get(dbl, {ptr, ptr, ptr, ptr, ptr}, false);
        std::string name = "bee_fn_" + std::to_string(counter++);
        auto* f = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, name, mod);
        declared[fn] = f;
        worklist.push_back(fn);
        return f;
    }

    // Resolve a called global name to a compilable target function, or null.
    // The binding is read once at compile time; like the existing direct-
    // recursion path, this assumes numeric functions are not reassigned.
    const FunctionStmt* resolveTarget(const std::string& name) {
        if (!interp.globals) return nullptr;
        Value out;
        if (!interp.globals->tryGetName(name, out)) return nullptr;
        if (!out.isFunction()) return nullptr;
        auto f = out.asFunction();
        if (!f->decl || f->boundThis || f->definingClass) return nullptr;
        if (!jitCandidate(f->decl)) return nullptr;
        return f->decl;
    }
};

class Codegen {
public:
    Codegen(ModuleCompiler& mc, const FunctionStmt* fn, llvm::Function* func, JitSig sig = 0)
        : mc_(mc), ctx_(mc.ctx), mod_(mc.mod), b_(mc.ctx), fn_(fn), func_(func), sig_(sig) {}

    llvm::Function* emit() {
        Type* dbl = b_.getDoubleTy();
        Type* ptr = PointerType::get(ctx_, 0);
        Type* i64 = b_.getInt64Ty();
        argsArg_ = func_->getArg(0);
        bufsArg_ = func_->getArg(1);
        bufLensArg_ = func_->getArg(2);
        interpArg_ = func_->getArg(3);
        bailArg_ = func_->getArg(4);

        entry_ = BasicBlock::Create(ctx_, "entry", func_);
        b_.SetInsertPoint(entry_);

        // Frame scope: bring each parameter in, as the signature describes it.
        pushScope(fn_, fn_->frameSlots);
        size_t np = fn_->params.size();
        for (size_t i = 0; i < np; ++i) {
            int slot = fn_->paramStart + (int)i;
            if (jitSigAt(sig_, i) == ArgKind::BufF64) {
                // A buffer arrives as a base pointer and a count, both loop
                // invariant, so every later index is a bounds check and a load.
                AllocaInst* pa = mkAlloca(ptr);
                AllocaInst* la = mkAlloca(i64);
                b_.CreateStore(b_.CreateLoad(ptr, b_.CreateGEP(ptr, bufsArg_,
                                                               b_.getInt32((uint32_t)i))), pa);
                b_.CreateStore(b_.CreateLoad(i64, b_.CreateGEP(i64, bufLensArg_,
                                                               b_.getInt32((uint32_t)i))), la);
                scopes_.back().allocas[slot] = pa;
                scopes_.back().lenAllocas[slot] = la;
                scopes_.back().types[slot] = Ty::BUFFER;
                continue;
            }
            AllocaInst* a = mkAlloca(dbl);
            llvm::Value* gep = b_.CreateGEP(dbl, argsArg_, b_.getInt32((uint32_t)i));
            b_.CreateStore(b_.CreateLoad(dbl, gep), a);
            scopes_.back().allocas[slot] = a;
            scopes_.back().types[slot] = Ty::NUMBER;
        }

        for (auto& s : fn_->body) {
            if (terminated()) break;
            stmt(s.get());
        }

        // Fall off the end (or an un-returned path) => the Bee function yields
        // nil. Signal a nil completion (bail=2) rather than a hard bail, so a
        // side-effect-free numeric function with no `return` still runs natively
        // instead of being re-executed by the interpreter.
        if (!terminated()) emitNilReturn();

        popScope();
        if (verifyFunction(*func_, &errs())) throw JitBail{};
        return func_;
    }

    // Compile a top-level while/for loop. ABI: double f(double* vars, i32 nvars,
    // i8* interp, i32* bail). The numeric globals in `globals` are loaded from
    // vars[] on entry and written back on clean completion.
    void emitLoop(const Stmt* loop, const std::vector<std::string>& globals) {
        loopRegion_ = true;
        Type* dbl = b_.getDoubleTy();
        argsArg_ = func_->getArg(0);   // double* vars (in/out)
        interpArg_ = func_->getArg(2);
        bailArg_ = func_->getArg(3);

        entry_ = BasicBlock::Create(ctx_, "entry", func_);
        b_.SetInsertPoint(entry_);

        std::vector<AllocaInst*> gallocas(globals.size());
        for (size_t i = 0; i < globals.size(); ++i) {
            AllocaInst* a = mkAlloca(dbl);
            llvm::Value* gep = b_.CreateGEP(dbl, argsArg_, b_.getInt32((uint32_t)i));
            b_.CreateStore(b_.CreateLoad(dbl, gep), a);
            gallocas[i] = a;
            loopGlobalAllocas_[globals[i]] = a;
        }

        stmt(const_cast<Stmt*>(loop));  // the while/for; break falls through below

        // Clean completion: flush the (possibly updated) globals back to vars[].
        if (!terminated()) {
            for (size_t i = 0; i < globals.size(); ++i) {
                llvm::Value* gep = b_.CreateGEP(dbl, argsArg_, b_.getInt32((uint32_t)i));
                b_.CreateStore(b_.CreateLoad(dbl, gallocas[i]), gep);
            }
            b_.CreateRet(ConstantFP::get(dbl, 0.0));
        }
        if (verifyFunction(*func_, &errs())) throw JitBail{};
    }

private:
    ModuleCompiler& mc_;
    LLVMContext& ctx_;
    llvm::Module& mod_;
    IRBuilder<> b_;
    const FunctionStmt* fn_;
    llvm::Function* func_ = nullptr;
    JitSig sig_ = 0;
    BasicBlock* entry_ = nullptr;
    llvm::Value* argsArg_ = nullptr;
    llvm::Value* bufsArg_ = nullptr;
    llvm::Value* bufLensArg_ = nullptr;
    llvm::Value* interpArg_ = nullptr;
    llvm::Value* bailArg_ = nullptr;
    BasicBlock* bailBB_ = nullptr;
    BasicBlock* nilBB_ = nullptr;

    // Loop-region codegen (fn_ == nullptr): the numeric globals the loop
    // touches, each backed by an alloca mirroring a slot of the in/out vars[].
    bool loopRegion_ = false;
    std::map<std::string, AllocaInst*> loopGlobalAllocas_;

    std::vector<Scope> scopes_;
    struct Loop { BasicBlock* brk; BasicBlock* cont; };
    std::vector<Loop> loops_;

    // ---- helpers ----
    bool terminated() { return b_.GetInsertBlock()->getTerminator() != nullptr; }

    AllocaInst* mkAlloca(Type* t) {
        IRBuilder<> ab(entry_, entry_->begin());
        return ab.CreateAlloca(t, nullptr);
    }

    void pushScope(const void* owner, int n) { scopes_.emplace_back(owner, n); }
    void popScope() { scopes_.pop_back(); }

    Scope& scopeAt(int depth) {
        int idx = (int)scopes_.size() - 1 - depth;
        if (idx < 0 || idx >= (int)scopes_.size()) throw JitBail{};
        return scopes_[idx];
    }

    // The shared "bail and return 0.0" block.
    BasicBlock* bailBlock() {
        if (!bailBB_) {
            bailBB_ = BasicBlock::Create(ctx_, "bail", func_);
            IRBuilder<> bb(bailBB_);
            bb.CreateStore(bb.getInt32(1), bailArg_);
            bb.CreateRet(ConstantFP::get(bb.getDoubleTy(), 0.0));
        }
        return bailBB_;
    }
    void emitBail() { b_.CreateBr(bailBlock()); }
    void emitBailOn(llvm::Value* cond, BasicBlock* contBB) {
        b_.CreateCondBr(cond, bailBlock(), contBB);
    }

    // "Completed with a nil result" (bail flag = 2): a value-less `return` or
    // falling off the end. Distinct from a genuine bail (=1) so the caller keeps
    // the native run instead of re-executing the whole function interpreted.
    BasicBlock* nilBlock() {
        if (!nilBB_) {
            nilBB_ = BasicBlock::Create(ctx_, "retnil", func_);
            IRBuilder<> nb(nilBB_);
            nb.CreateStore(nb.getInt32(2), bailArg_);
            nb.CreateRet(ConstantFP::get(nb.getDoubleTy(), 0.0));
        }
        return nilBB_;
    }
    void emitNilReturn() { b_.CreateBr(nilBlock()); }

    // Truthiness of a typed value as an i1. Numbers are always truthy in Bee
    // (even 0), so a NUMBER condition is a compile-time `true`.
    llvm::Value* truthy(const TVal& t) {
        if (t.ty == Ty::BOOL) return t.v;
        return b_.getInt1(true);
    }

    llvm::Value* toNum(const TVal& t) { if (t.ty != Ty::NUMBER) throw JitBail{}; return t.v; }
    llvm::Value* toBool(const TVal& t) { if (t.ty != Ty::BOOL) throw JitBail{}; return t.v; }

    llvm::Value* d2i(llvm::Value* d) { return b_.CreateFPToSI(d, b_.getInt64Ty()); }
    llvm::Value* i2d(llvm::Value* i) { return b_.CreateSIToFP(i, b_.getDoubleTy()); }

    // ---- statements ----
    void stmt(Stmt* s) {
        switch (s->kind) {
            case Stmt::Kind::Expression:
                expr(static_cast<ExprStmt*>(s)->expr.get());
                break;
            case Stmt::Kind::Let:    sLet(static_cast<LetStmt*>(s)); break;
            case Stmt::Kind::Block:  sBlock(static_cast<BlockStmt*>(s)); break;
            case Stmt::Kind::If:     sIf(static_cast<IfStmt*>(s)); break;
            case Stmt::Kind::While:  sWhile(static_cast<WhileStmt*>(s)); break;
            case Stmt::Kind::For:    sFor(static_cast<ForStmt*>(s)); break;
            case Stmt::Kind::Return: sReturn(static_cast<ReturnStmt*>(s)); break;
            case Stmt::Kind::Break:
                if (loops_.empty()) throw JitBail{};
                b_.CreateBr(loops_.back().brk);
                break;
            case Stmt::Kind::Continue:
                if (loops_.empty()) throw JitBail{};
                b_.CreateBr(loops_.back().cont);
                break;
            default: throw JitBail{}; // ForIn, Try, Throw, Class, Import, Match, Function
        }
    }

    void sLet(LetStmt* s) {
        if (s->global || s->isDestructure || s->slot < 0) throw JitBail{};
        if (!s->initializer) throw JitBail{}; // `let x` => nil, not numeric
        TVal init = expr(s->initializer.get());
        AllocaInst* a = mkAlloca(init.ty == Ty::BOOL ? (Type*)b_.getInt1Ty()
                                                     : (Type*)b_.getDoubleTy());
        b_.CreateStore(init.v, a);
        scopes_.back().allocas[s->slot] = a;
        scopes_.back().types[s->slot] = init.ty;
    }

    void sBlock(BlockStmt* s) {
        if (s->transparent) {
            for (auto& st : s->statements) { if (terminated()) break; stmt(st.get()); }
        } else {
            pushScope(s, s->slotCount);
            for (auto& st : s->statements) { if (terminated()) break; stmt(st.get()); }
            popScope();
        }
    }

    void sIf(IfStmt* s) {
        llvm::Value* c = truthy(expr(s->condition.get()));
        BasicBlock* thenBB = BasicBlock::Create(ctx_, "then", func_);
        BasicBlock* elseBB = BasicBlock::Create(ctx_, "else", func_);
        BasicBlock* endBB  = BasicBlock::Create(ctx_, "endif", func_);
        b_.CreateCondBr(c, thenBB, elseBB);

        b_.SetInsertPoint(thenBB);
        stmt(s->thenBranch.get());
        if (!terminated()) b_.CreateBr(endBB);

        b_.SetInsertPoint(elseBB);
        if (s->elseBranch) stmt(s->elseBranch.get());
        if (!terminated()) b_.CreateBr(endBB);

        b_.SetInsertPoint(endBB);
    }

    void sWhile(WhileStmt* s) {
        BasicBlock* condBB = BasicBlock::Create(ctx_, "wcond", func_);
        BasicBlock* bodyBB = BasicBlock::Create(ctx_, "wbody", func_);
        BasicBlock* endBB  = BasicBlock::Create(ctx_, "wend", func_);
        b_.CreateBr(condBB);

        b_.SetInsertPoint(condBB);
        llvm::Value* c = truthy(expr(s->condition.get()));
        b_.CreateCondBr(c, bodyBB, endBB);

        b_.SetInsertPoint(bodyBB);
        loops_.push_back({endBB, condBB});
        stmt(s->body.get());
        loops_.pop_back();
        if (!terminated()) b_.CreateBr(condBB);

        b_.SetInsertPoint(endBB);
    }

    void sFor(ForStmt* s) {
        // A merged loop scope declares into the enclosing frame, so there is no
        // scope to push here -- pushing one would shift every depth by one.
        if (s->ownScope) pushScope(s, s->slotCount);
        if (s->init) stmt(s->init.get());

        BasicBlock* condBB = BasicBlock::Create(ctx_, "fcond", func_);
        BasicBlock* bodyBB = BasicBlock::Create(ctx_, "fbody", func_);
        BasicBlock* incrBB = BasicBlock::Create(ctx_, "fincr", func_);
        BasicBlock* endBB  = BasicBlock::Create(ctx_, "fend", func_);
        b_.CreateBr(condBB);

        b_.SetInsertPoint(condBB);
        llvm::Value* c = s->condition ? truthy(expr(s->condition.get())) : b_.getInt1(true);
        b_.CreateCondBr(c, bodyBB, endBB);

        b_.SetInsertPoint(bodyBB);
        loops_.push_back({endBB, incrBB});   // `continue` targets the increment
        stmt(s->body.get());
        loops_.pop_back();
        if (!terminated()) b_.CreateBr(incrBB);

        b_.SetInsertPoint(incrBB);
        if (s->increment) expr(s->increment.get());
        b_.CreateBr(condBB);

        b_.SetInsertPoint(endBB);
        if (s->ownScope) popScope();
    }

    void sReturn(ReturnStmt* s) {
        // A `return` inside a top-level loop region would end the whole program,
        // not just the loop, and would skip the global write-back. Bail.
        if (loopRegion_) throw JitBail{};
        if (!s->value) { emitNilReturn(); return; }   // `return` with no value => nil
        TVal v = expr(s->value.get());
        b_.CreateRet(toNum(v));                  // ABI returns a double
    }

    // ---- expressions ----
    TVal expr(Expr* e) {
        switch (e->kind) {
            case Expr::Kind::Literal:  return eLiteral(static_cast<LiteralExpr*>(e));
            case Expr::Kind::Variable: return eVar(static_cast<VariableExpr*>(e));
            case Expr::Kind::Assign:   return eAssign(static_cast<AssignExpr*>(e));
            case Expr::Kind::Binary:   return eBinary(static_cast<BinaryExpr*>(e));
            case Expr::Kind::Logical:  return eLogical(static_cast<LogicalExpr*>(e));
            case Expr::Kind::Unary:    return eUnary(static_cast<UnaryExpr*>(e));
            case Expr::Kind::Grouping: return expr(static_cast<GroupingExpr*>(e)->inner.get());
            case Expr::Kind::Ternary:  return eTernary(static_cast<TernaryExpr*>(e));
            case Expr::Kind::Call:     return eCall(static_cast<CallExpr*>(e));
            case Expr::Kind::Index:    return eIndex(static_cast<IndexExpr*>(e));
            default: throw JitBail{}; // strings/lists/dicts/get/set/index/this/super/...
        }
    }

    TVal eLiteral(LiteralExpr* e) {
        if (e->value.isNumber())
            return {ConstantFP::get(b_.getDoubleTy(), e->value.asNumber()), Ty::NUMBER};
        if (e->value.isBool())
            return {b_.getInt1(e->value.asBool()), Ty::BOOL};
        throw JitBail{}; // nil / string
    }

    TVal eVar(VariableExpr* e) {
        if (e->global) {
            // In a loop region, referenced numeric globals are backed by allocas
            // loaded from the in/out vars[] array. Anything else bails.
            auto it = loopGlobalAllocas_.find(e->name);
            if (it == loopGlobalAllocas_.end()) throw JitBail{};
            return {b_.CreateLoad(b_.getDoubleTy(), it->second), Ty::NUMBER};
        }
        if (e->slot < 0) throw JitBail{};
        Scope& sc = scopeAt(e->depth);
        AllocaInst* a = sc.allocas[e->slot];
        if (!a) throw JitBail{};
        Ty ty = sc.types[e->slot];
        if (ty == Ty::BUFFER) {
            Type* ptr = PointerType::get(ctx_, 0);
            return {b_.CreateLoad(ptr, a), Ty::BUFFER,
                    b_.CreateLoad(b_.getInt64Ty(), sc.lenAllocas[e->slot])};
        }
        Type* lt = (ty == Ty::BOOL) ? (Type*)b_.getInt1Ty() : (Type*)b_.getDoubleTy();
        return {b_.CreateLoad(lt, a), ty};
    }

    // buf[i] on an f64 buffer: a bounds check and a load. Out of range bails to
    // the interpreter, which reproduces the error with the right message and
    // line -- safe to re-run, because the compiled subset writes nothing.
    TVal eIndex(IndexExpr* e) {
        TVal obj = expr(e->object.get());
        if (obj.ty != Ty::BUFFER) throw JitBail{};
        llvm::Value* idx = d2i(toNum(expr(e->index.get())));

        // Negative indices count from the end, as everywhere else in Bee.
        llvm::Value* neg = b_.CreateICmpSLT(idx, b_.getInt64(0));
        idx = b_.CreateSelect(neg, b_.CreateAdd(idx, obj.len), idx);

        llvm::Value* low = b_.CreateICmpSLT(idx, b_.getInt64(0));
        llvm::Value* high = b_.CreateICmpSGE(idx, obj.len);
        BasicBlock* okBB = BasicBlock::Create(ctx_, "inbounds", func_);
        emitBailOn(b_.CreateOr(low, high), okBB);
        b_.SetInsertPoint(okBB);

        Type* dbl = b_.getDoubleTy();
        return {b_.CreateLoad(dbl, b_.CreateGEP(dbl, obj.v, idx)), Ty::NUMBER};
    }

    TVal eAssign(AssignExpr* e) {
        if (!e->value) throw JitBail{};
        if (e->global) {
            auto it = loopGlobalAllocas_.find(e->name);
            if (it == loopGlobalAllocas_.end()) throw JitBail{};
            TVal v = expr(e->value.get());
            b_.CreateStore(toNum(v), it->second); // loop globals are numeric
            return v;
        }
        if (e->slot < 0) throw JitBail{};
        TVal v = expr(e->value.get());
        Scope& sc = scopeAt(e->depth);
        AllocaInst* a = sc.allocas[e->slot];
        if (!a || sc.types[e->slot] != v.ty) throw JitBail{}; // type must be stable
        b_.CreateStore(v.v, a);
        return v;
    }

    TVal eUnary(UnaryExpr* e) {
        TVal r = expr(e->right.get());
        if (e->op == TokenType::MINUS)   return {b_.CreateFNeg(toNum(r)), Ty::NUMBER};
        if (e->op == TokenType::BIT_NOT) return {i2d(b_.CreateNot(d2i(toNum(r)))), Ty::NUMBER};
        if (e->op == TokenType::NOT) {   // !truthy: number => false, bool => negate
            if (r.ty == Ty::NUMBER) return {b_.getInt1(false), Ty::BOOL};
            return {b_.CreateNot(r.v), Ty::BOOL};
        }
        throw JitBail{};
    }

    TVal eLogical(LogicalExpr* e) {
        // Short-circuit. Restricted to BOOL operands so the i1 result matches
        // Bee's truthiness of the returned operand exactly.
        TVal l = expr(e->left.get());
        llvm::Value* lb = toBool(l);
        BasicBlock* startBB = b_.GetInsertBlock();
        BasicBlock* rhsBB = BasicBlock::Create(ctx_, "logrhs", func_);
        BasicBlock* endBB = BasicBlock::Create(ctx_, "logend", func_);
        if (e->op == TokenType::OR) b_.CreateCondBr(lb, endBB, rhsBB);
        else                        b_.CreateCondBr(lb, rhsBB, endBB); // AND

        b_.SetInsertPoint(rhsBB);
        TVal r = expr(e->right.get());
        llvm::Value* rb = toBool(r);
        BasicBlock* rhsEnd = b_.GetInsertBlock();
        b_.CreateBr(endBB);

        b_.SetInsertPoint(endBB);
        PHINode* phi = b_.CreatePHI(b_.getInt1Ty(), 2);
        phi->addIncoming(lb, startBB);
        phi->addIncoming(rb, rhsEnd);
        return {phi, Ty::BOOL};
    }

    TVal eTernary(TernaryExpr* e) {
        llvm::Value* c = truthy(expr(e->cond.get()));
        BasicBlock* thenBB = BasicBlock::Create(ctx_, "tthen", func_);
        BasicBlock* elseBB = BasicBlock::Create(ctx_, "telse", func_);
        BasicBlock* endBB  = BasicBlock::Create(ctx_, "tend", func_);
        b_.CreateCondBr(c, thenBB, elseBB);

        b_.SetInsertPoint(thenBB);
        TVal t = expr(e->thenBranch.get());
        BasicBlock* thenEnd = b_.GetInsertBlock(); b_.CreateBr(endBB);

        b_.SetInsertPoint(elseBB);
        TVal f = expr(e->elseBranch.get());
        BasicBlock* elseEnd = b_.GetInsertBlock(); b_.CreateBr(endBB);

        if (t.ty != f.ty) throw JitBail{};
        b_.SetInsertPoint(endBB);
        Type* lt = (t.ty == Ty::BOOL) ? (Type*)b_.getInt1Ty() : (Type*)b_.getDoubleTy();
        PHINode* phi = b_.CreatePHI(lt, 2);
        phi->addIncoming(t.v, thenEnd);
        phi->addIncoming(f.v, elseEnd);
        return {phi, t.ty};
    }

    // Direct self-recursion and calls to other numeric functions (helpers,
    // mutual recursion). Non-numeric or unresolvable callees bail the caller.
    TVal eCall(CallExpr* e) {
        if (e->callee->kind != Expr::Kind::Variable) throw JitBail{};
        auto* callee = static_cast<VariableExpr*>(e->callee.get());
        if (!callee->global) throw JitBail{};

        const FunctionStmt* target;
        llvm::Function* calleeFn;
        if (fn_ && callee->name == fn_->name) {   // direct self-recursion
            target = fn_;
            calleeFn = func_;
        } else {                                  // call to another function (or from a loop region)
            target = mc_.resolveTarget(callee->name);
            if (!target) throw JitBail{};
            calleeFn = mc_.declare(target);       // compiled into this module too
        }

        size_t np = target->params.size();
        if (e->args.size() != np) throw JitBail{};
        for (bool sp : e->spread) if (sp) throw JitBail{};

        Type* dbl = b_.getDoubleTy();
        ArrayType* arrTy = ArrayType::get(dbl, np ? np : 1);
        AllocaInst* argsBuf = mkAlloca(arrTy);    // one buffer per call site
        for (size_t i = 0; i < np; ++i) {
            TVal a = expr(e->args[i].get());
            llvm::Value* gep = b_.CreateGEP(arrTy, argsBuf,
                                            {b_.getInt32(0), b_.getInt32((uint32_t)i)});
            b_.CreateStore(toNum(a), gep);
        }
        llvm::Value* argsPtr = b_.CreateGEP(arrTy, argsBuf,
                                            {b_.getInt32(0), b_.getInt32(0)});
        llvm::Value* nullPtr = ConstantPointerNull::get(PointerType::get(ctx_, 0));
        llvm::Value* ret = b_.CreateCall(calleeFn, {argsPtr, nullPtr, nullPtr,
                                                    interpArg_, bailArg_});
        // Propagate a bail from the callee.
        llvm::Value* bv = b_.CreateLoad(b_.getInt32Ty(), bailArg_);
        llvm::Value* nz = b_.CreateICmpNE(bv, b_.getInt32(0));
        BasicBlock* contBB = BasicBlock::Create(ctx_, "callok", func_);
        emitBailOn(nz, contBB);
        b_.SetInsertPoint(contBB);
        return {ret, Ty::NUMBER};
    }

    TVal eBinary(BinaryExpr* e) {
        TVal l = expr(e->left.get());
        TVal r = expr(e->right.get());
        switch (e->op) {
            case TokenType::PLUS:  return {b_.CreateFAdd(toNum(l), toNum(r)), Ty::NUMBER};
            case TokenType::MINUS: return {b_.CreateFSub(toNum(l), toNum(r)), Ty::NUMBER};
            case TokenType::STAR:  return {b_.CreateFMul(toNum(l), toNum(r)), Ty::NUMBER};
            case TokenType::SLASH: {
                llvm::Value* rv = toNum(r);
                guardNonZero(rv);
                return {b_.CreateFDiv(toNum(l), rv), Ty::NUMBER};
            }
            case TokenType::PERCENT: {
                llvm::Value* a = toNum(l), *bb = toNum(r);
                guardNonZero(bb);
                // fmod(a,b) == a - b*trunc(a/b)  (matches std::fmod for finite operands)
                llvm::Value* q = b_.CreateFDiv(a, bb);
                // Renamed in LLVM 19; the old name is gone by LLVM 20. Linux CI
                // builds against 18, MSYS2 ships a newer LLVM, so support both.
#if LLVM_VERSION_MAJOR >= 19
                llvm::Function* trunc = Intrinsic::getOrInsertDeclaration(&mod_, Intrinsic::trunc, {b_.getDoubleTy()});
#else
                llvm::Function* trunc = Intrinsic::getDeclaration(&mod_, Intrinsic::trunc, {b_.getDoubleTy()});
#endif
                llvm::Value* qt = b_.CreateCall(trunc, {q});
                return {b_.CreateFSub(a, b_.CreateFMul(bb, qt)), Ty::NUMBER};
            }
            case TokenType::LT: return {b_.CreateFCmpOLT(toNum(l), toNum(r)), Ty::BOOL};
            case TokenType::GT: return {b_.CreateFCmpOGT(toNum(l), toNum(r)), Ty::BOOL};
            case TokenType::LE: return {b_.CreateFCmpOLE(toNum(l), toNum(r)), Ty::BOOL};
            case TokenType::GE: return {b_.CreateFCmpOGE(toNum(l), toNum(r)), Ty::BOOL};
            case TokenType::EQ:
            case TokenType::NEQ: {
                if (l.ty != r.ty) throw JitBail{};
                bool eq = (e->op == TokenType::EQ);
                if (l.ty == Ty::NUMBER)
                    return {eq ? b_.CreateFCmpOEQ(l.v, r.v) : b_.CreateFCmpONE(l.v, r.v), Ty::BOOL};
                return {eq ? b_.CreateICmpEQ(l.v, r.v) : b_.CreateICmpNE(l.v, r.v), Ty::BOOL};
            }
            case TokenType::BIT_AND: return {i2d(b_.CreateAnd(d2i(toNum(l)), d2i(toNum(r)))), Ty::NUMBER};
            case TokenType::BIT_OR:  return {i2d(b_.CreateOr (d2i(toNum(l)), d2i(toNum(r)))), Ty::NUMBER};
            case TokenType::BIT_XOR: return {i2d(b_.CreateXor(d2i(toNum(l)), d2i(toNum(r)))), Ty::NUMBER};
            case TokenType::SHL:     return {i2d(b_.CreateShl (d2i(toNum(l)), d2i(toNum(r)))), Ty::NUMBER};
            case TokenType::SHR:     return {i2d(b_.CreateAShr(d2i(toNum(l)), d2i(toNum(r)))), Ty::NUMBER};
            default: throw JitBail{};
        }
    }

    // A Bee runtime error (division/modulo by zero) => bail to the interpreter,
    // which reproduces the error with the correct message and line number.
    void guardNonZero(llvm::Value* divisor) {
        llvm::Value* isZero = b_.CreateFCmpOEQ(divisor, ConstantFP::get(b_.getDoubleTy(), 0.0));
        BasicBlock* contBB = BasicBlock::Create(ctx_, "nz", func_);
        emitBailOn(isZero, contBB);
        b_.SetInsertPoint(contBB);
    }
};

// ---- loop-region global collection ---------------------------------------
//
// Gather the names of global variables a loop reads or writes. Call *targets*
// (a global in callee position) are excluded -- they are resolved as functions,
// not passed as numeric vars. Under-collecting is safe (a missed global just
// makes codegen bail); over-collecting a non-numeric global makes the runtime
// prefilter bail. So this need not be exhaustive over the whole language, only
// consistent -- the returned order also defines the vars[] layout.
struct GlobalCollector {
    std::vector<std::string> out;
    std::set<std::string> seen;

    void add(const std::string& n) { if (seen.insert(n).second) out.push_back(n); }

    void expr(const Expr* e) {
        if (!e) return;
        switch (e->kind) {
            case Expr::Kind::Variable: {
                auto* v = static_cast<const VariableExpr*>(e);
                if (v->global) add(v->name);
                break;
            }
            case Expr::Kind::Assign: {
                auto* a = static_cast<const AssignExpr*>(e);
                if (a->global) add(a->name);
                expr(a->value.get());
                break;
            }
            case Expr::Kind::Binary: {
                auto* b = static_cast<const BinaryExpr*>(e);
                expr(b->left.get()); expr(b->right.get()); break;
            }
            case Expr::Kind::Logical: {
                auto* l = static_cast<const LogicalExpr*>(e);
                expr(l->left.get()); expr(l->right.get()); break;
            }
            case Expr::Kind::Unary: expr(static_cast<const UnaryExpr*>(e)->right.get()); break;
            case Expr::Kind::Grouping: expr(static_cast<const GroupingExpr*>(e)->inner.get()); break;
            case Expr::Kind::Ternary: {
                auto* t = static_cast<const TernaryExpr*>(e);
                expr(t->cond.get()); expr(t->thenBranch.get()); expr(t->elseBranch.get()); break;
            }
            case Expr::Kind::Call: {
                auto* c = static_cast<const CallExpr*>(e);
                // Skip a plain global callee: it is a call target, not a var.
                if (c->callee->kind != Expr::Kind::Variable) expr(c->callee.get());
                for (auto& a : c->args) expr(a.get());
                break;
            }
            default: break; // other exprs make codegen bail; nothing to collect
        }
    }

    void stmt(const Stmt* s) {
        if (!s) return;
        switch (s->kind) {
            case Stmt::Kind::Expression: expr(static_cast<const ExprStmt*>(s)->expr.get()); break;
            case Stmt::Kind::Let: expr(static_cast<const LetStmt*>(s)->initializer.get()); break;
            case Stmt::Kind::Block:
                for (auto& st : static_cast<const BlockStmt*>(s)->statements) stmt(st.get());
                break;
            case Stmt::Kind::If: {
                auto* i = static_cast<const IfStmt*>(s);
                expr(i->condition.get()); stmt(i->thenBranch.get()); stmt(i->elseBranch.get()); break;
            }
            case Stmt::Kind::While: {
                auto* w = static_cast<const WhileStmt*>(s);
                expr(w->condition.get()); stmt(w->body.get()); break;
            }
            case Stmt::Kind::For: {
                auto* f = static_cast<const ForStmt*>(s);
                stmt(f->init.get()); expr(f->condition.get());
                expr(f->increment.get()); stmt(f->body.get()); break;
            }
            case Stmt::Kind::Return: expr(static_cast<const ReturnStmt*>(s)->value.get()); break;
            default: break; // other stmts make codegen bail; nothing to collect
        }
    }
};

std::vector<std::string> collectLoopGlobals(const Stmt* loop) {
    GlobalCollector gc;
    gc.stmt(loop);
    return gc.out;
}

// Run the standard -O2 pipeline over a freshly built module.
void optimize(llvm::Module& mod) {
    LoopAnalysisManager LAM;
    FunctionAnalysisManager FAM;
    CGSCCAnalysisManager CGAM;
    ModuleAnalysisManager MAM;
    PassBuilder PB;
    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
    ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(OptimizationLevel::O2);
    MPM.run(mod, MAM);
}

} // anonymous namespace

// The front end (jit.cpp) has already applied BEE_NO_JIT, the caches, and the
// jitCandidate() pre-filter; this just does the LLVM work.
JitFn LlvmJitBackend::compile(const FunctionStmt* fn, JitSig sig, Interpreter& interp,
                              std::vector<JitCacheEntry>& extra) {
    if (!engine()) return nullptr;

    JitFn result = nullptr;
    // (FunctionStmt, symbol name) for every function compiled into this module.
    std::vector<std::pair<const FunctionStmt*, std::string>> compiled;
    try {
        auto ctx = std::make_unique<LLVMContext>();
        auto mod = std::make_unique<llvm::Module>("bee_jit", *ctx);
        ModuleCompiler mc{*ctx, *mod, interp, counter, {}, {}};

        mc.declare(fn);                  // seed the worklist with the entry
        // Drain: compiling a body may enqueue the callees it references. Only
        // the entry is specialised; a callee is compiled for all-numeric
        // arguments, which is what a call inside the subset can pass.
        for (size_t i = 0; i < mc.worklist.size(); ++i) {
            const FunctionStmt* cur = mc.worklist[i];
            Codegen cg(mc, cur, mc.declared[cur], cur == fn ? sig : 0);
            cg.emit();                   // throws JitBail if outside the subset
        }
        optimize(*mod);

        for (auto& kv : mc.declared)
            compiled.push_back({kv.first, kv.second->getName().str()});

        if (engine()->addIRModule(orc::ThreadSafeModule(std::move(mod), std::move(ctx))))
            throw JitBail{};

        // Resolve every function's entry point; hand the callees back so a later
        // direct call reuses this native code instead of recompiling.
        for (auto& [stmt, symName] : compiled) {
            auto sym = engine()->lookup(symName);
            if (!sym) { if (stmt == fn) throw JitBail{}; continue; }
            JitFn p = sym->toPtr<JitFn>();
            if (stmt == fn) result = p;
            else extra.push_back({stmt, 0, p});   // callees took the all-numeric form
        }
        if (!result) throw JitBail{};
    } catch (JitBail&) {
        result = nullptr;                // other signatures stay retryable
    }
    return result;
}

void LlvmJitBackend::compileLoop(const Stmt* loop, Interpreter& interp, CompiledLoop& out) {
    bool isLoop = loop->kind == Stmt::Kind::While || loop->kind == Stmt::Kind::For;
    if (!isLoop || !engine()) return;   // out.fn stays nullptr == "cannot compile"

    std::vector<std::string> globals = collectLoopGlobals(loop);
    try {
        auto ctx = std::make_unique<LLVMContext>();
        auto mod = std::make_unique<llvm::Module>("bee_loop", *ctx);
        ModuleCompiler mc{*ctx, *mod, interp, counter, {}, {}};

        // The loop entry has the JitLoopFn ABI; it is not keyed to a
        // FunctionStmt, so create it directly (callees still go via mc).
        IRBuilder<> b(*ctx);
        Type* dbl = b.getDoubleTy();
        Type* ptr = PointerType::get(*ctx, 0);
        Type* i32 = b.getInt32Ty();
        // Matches JitLoopFn: (double* vars, int nvars, void* interp, int* bail).
        // The loop reads interp/bail from args 2/3; building five params here left
        // bail pointing at an uninitialised register, crashing any hot top-level
        // loop that calls a function or hits a bail (e.g. division by zero).
        FunctionType* ft = FunctionType::get(dbl, {ptr, i32, ptr, ptr}, false);
        std::string entryName = "bee_loop_" + std::to_string(counter++);
        auto* entryF = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
                                              entryName, *mod);

        Codegen cg(mc, /*fn*/nullptr, entryF);
        cg.emitLoop(loop, globals);   // may enqueue called functions into mc

        // Drain: compile any functions the loop calls (cross-calls).
        for (size_t i = 0; i < mc.worklist.size(); ++i) {
            const FunctionStmt* cur = mc.worklist[i];
            Codegen c2(mc, cur, mc.declared[cur]);
            c2.emit();
        }
        optimize(*mod);

        if (engine()->addIRModule(orc::ThreadSafeModule(std::move(mod), std::move(ctx))))
            throw JitBail{};
        auto sym = engine()->lookup(entryName);
        if (!sym) throw JitBail{};
        out.fn = sym->toPtr<JitLoopFn>();
        out.globals = std::move(globals);
    } catch (JitBail&) {
        out.fn = nullptr;
    }
}

} // namespace bee

// The one symbol the shared object exports: the front end resolves it by name
// (see jit.cpp) and calls it once to obtain the backend. On Windows the DLL is
// linked with --exclude-all-symbols (MinGW would otherwise auto-export every
// global, and with LLVM folded in statically that overflows the 65535-entry PE
// export table -- "export ordinal too large"). dllexport keeps this one symbol
// exported anyway; everywhere else the attribute is a no-op.
#if defined(_WIN32)
#  define BEE_JIT_EXPORT __declspec(dllexport)
#else
#  define BEE_JIT_EXPORT
#endif
extern "C" BEE_JIT_EXPORT bee::JitBackend* bee_jit_create() { return new bee::LlvmJitBackend(); }

#endif // BEE_JIT
