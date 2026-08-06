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

using namespace llvm;

namespace bee {

// A statically-typed SSA value produced by codegen.
enum class Ty { NUMBER, BOOL };
struct TVal { llvm::Value* v = nullptr; Ty ty = Ty::NUMBER; };

// Thrown internally to abort compilation of a function that leaves the subset.
struct JitBail {};

// ---- static candidate pre-filter (no LLVM) -------------------------------
bool jitCandidate(const FunctionStmt* fn) {
    return fn->restParam < 0 && fn->paramStart == 0;
}

// ---- ORC JIT engine (one per interpreter) --------------------------------
struct Jit::Impl {
    std::unique_ptr<orc::LLJIT> jit;
    unsigned counter = 0;
    Impl() {
        static bool inited = false;
        if (!inited) {
            InitializeNativeTarget();
            InitializeNativeTargetAsmPrinter();
            inited = true;
        }
        auto j = orc::LLJITBuilder().create();
        if (j) jit = std::move(*j);
    }
};

Jit::Jit() : impl(std::make_unique<Impl>()) {}
Jit::~Jit() = default;

// ---- per-function code generator -----------------------------------------
namespace {

struct Scope {
    const void* owner;                       // AST node that owns this scope
    std::vector<AllocaInst*> allocas;        // per slot (null until declared)
    std::vector<Ty> types;                   // per slot
    Scope(const void* o, int n) : owner(o), allocas(n, nullptr), types(n, Ty::NUMBER) {}
};

class Codegen {
public:
    Codegen(LLVMContext& ctx, llvm::Module& mod, const FunctionStmt* fn, const std::string& name)
        : ctx_(ctx), mod_(mod), b_(ctx), fn_(fn), name_(name) {}

    llvm::Function* emit() {
        // double f(double* args, i32 argc, i8* interp, i32* bail)
        Type* dbl = b_.getDoubleTy();
        Type* ptr = PointerType::get(ctx_, 0);
        Type* i32 = b_.getInt32Ty();
        FunctionType* ft = FunctionType::get(dbl, {ptr, i32, ptr, ptr}, false);
        func_ = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, name_, mod_);

        argsArg_ = func_->getArg(0);
        interpArg_ = func_->getArg(2);
        bailArg_ = func_->getArg(3);

        entry_ = BasicBlock::Create(ctx_, "entry", func_);
        b_.SetInsertPoint(entry_);

        // Frame scope: load each parameter from args[] into an alloca.
        pushScope(fn_, fn_->frameSlots);
        size_t np = fn_->params.size();
        for (size_t i = 0; i < np; ++i) {
            AllocaInst* a = mkAlloca(dbl);
            llvm::Value* gep = b_.CreateGEP(dbl, argsArg_, b_.getInt32((uint32_t)i));
            b_.CreateStore(b_.CreateLoad(dbl, gep), a);
            int slot = fn_->paramStart + (int)i;
            scopes_.back().allocas[slot] = a;
            scopes_.back().types[slot] = Ty::NUMBER;
        }

        // Scratch array for self-recursive calls (reused across call sites).
        if (np > 0)
            scratch_ = mkAlloca(ArrayType::get(dbl, np));

        for (auto& s : fn_->body) {
            if (terminated()) break;
            stmt(s.get());
        }

        // Fall off the end (or an un-returned path) => the Bee function would
        // yield nil, which isn't a number. Bail so the interpreter reproduces
        // the correct result. Safe: the subset has no side effects.
        if (!terminated()) emitBail();

        popScope();
        if (verifyFunction(*func_, &errs())) throw JitBail{};
        return func_;
    }

private:
    LLVMContext& ctx_;
    llvm::Module& mod_;
    IRBuilder<> b_;
    const FunctionStmt* fn_;
    std::string name_;
    llvm::Function* func_ = nullptr;
    BasicBlock* entry_ = nullptr;
    llvm::Value* argsArg_ = nullptr;
    llvm::Value* interpArg_ = nullptr;
    llvm::Value* bailArg_ = nullptr;
    AllocaInst* scratch_ = nullptr;
    BasicBlock* bailBB_ = nullptr;

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
        pushScope(s, s->slotCount);
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
        popScope();
    }

    void sReturn(ReturnStmt* s) {
        if (!s->value) { emitBail(); return; }   // `return` (nil) isn't numeric
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
        if (e->global || e->slot < 0) throw JitBail{};
        Scope& sc = scopeAt(e->depth);
        AllocaInst* a = sc.allocas[e->slot];
        if (!a) throw JitBail{};
        Ty ty = sc.types[e->slot];
        Type* lt = (ty == Ty::BOOL) ? (Type*)b_.getInt1Ty() : (Type*)b_.getDoubleTy();
        return {b_.CreateLoad(lt, a), ty};
    }

    TVal eAssign(AssignExpr* e) {
        if (e->global || e->slot < 0 || !e->value) throw JitBail{};
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

    // Only direct self-recursion is supported in v1.
    TVal eCall(CallExpr* e) {
        if (e->callee->kind != Expr::Kind::Variable) throw JitBail{};
        auto* callee = static_cast<VariableExpr*>(e->callee.get());
        if (!callee->global || callee->name != fn_->name) throw JitBail{};
        size_t np = fn_->params.size();
        if (e->args.size() != np) throw JitBail{};
        for (bool sp : e->spread) if (sp) throw JitBail{};

        Type* dbl = b_.getDoubleTy();
        ArrayType* arrTy = ArrayType::get(dbl, np);
        for (size_t i = 0; i < np; ++i) {
            TVal a = expr(e->args[i].get());
            llvm::Value* gep = b_.CreateGEP(arrTy, scratch_,
                                            {b_.getInt32(0), b_.getInt32((uint32_t)i)});
            b_.CreateStore(toNum(a), gep);
        }
        llvm::Value* argsPtr = b_.CreateGEP(arrTy, scratch_,
                                            {b_.getInt32(0), b_.getInt32(0)});
        llvm::Value* ret = b_.CreateCall(func_, {argsPtr, b_.getInt32((uint32_t)np),
                                                 interpArg_, bailArg_});
        // Propagate a bail from the recursive call.
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
                llvm::Function* trunc = Intrinsic::getDeclaration(&mod_, Intrinsic::trunc, {b_.getDoubleTy()});
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

JitFn Jit::getCompiled(const FunctionStmt* fn, Interpreter& /*interp*/) {
    auto it = cache_.find(fn);
    if (it != cache_.end()) return it->second;
    if (tried_[fn]) return nullptr;
    tried_[fn] = true;

    if (!impl->jit || !jitCandidate(fn)) { cache_[fn] = nullptr; return nullptr; }

    JitFn result = nullptr;
    try {
        auto ctx = std::make_unique<LLVMContext>();
        auto mod = std::make_unique<llvm::Module>("bee_jit", *ctx);
        std::string name = "bee_fn_" + std::to_string(impl->counter++);

        Codegen cg(*ctx, *mod, fn, name);
        cg.emit();                       // throws JitBail if outside the subset
        optimize(*mod);

        if (impl->jit->addIRModule(orc::ThreadSafeModule(std::move(mod), std::move(ctx))))
            throw JitBail{};
        auto sym = impl->jit->lookup(name);
        if (!sym) throw JitBail{};
        result = sym->toPtr<JitFn>();
    } catch (JitBail&) {
        result = nullptr;
    }

    cache_[fn] = result;
    return result;
}

} // namespace bee

#endif // BEE_JIT
