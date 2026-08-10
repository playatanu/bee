//
// The register VM's execution loop.
//
// Registers live in one flat, thread-local array; a frame is a contiguous window
// into it, so entering a function is a resize rather than an allocation. Nested
// Bee calls recurse into run() again, which keeps the C++ stack as the frame
// stack and needs no separate frame bookkeeping -- Bee's own call-depth limit
// bounds the recursion.
//
// Dispatch is by computed goto where the compiler supports it (GCC and Clang):
// each handler jumps straight to the next one, so the CPU sees one indirect
// branch per opcode with its own prediction history, rather than every opcode
// sharing the single branch at the top of a switch. The opcode list in
// chunk.hpp generates both the enum and the jump table, so they cannot drift.
//
#include "vm.hpp"
#include "compiler.hpp"
#include "interpreter.hpp"
#include <cmath>
#include <cstdlib>
#include <string>
#include <cstdio>

#if defined(__GNUC__) || defined(__clang__)
#define BEE_COMPUTED_GOTO 1
#endif

namespace bee {

// The register file: a stack of fixed-size blocks rather than one growable
// array.
//
// The reason is not speed, it is safety. Registers are handed to the rest of
// the interpreter by reference -- `callValue(R[a], ...)`, `applyBinary(R[b],
// R[c], ...)`, `stringify(R[c])` -- and any of those can run Bee code again: a
// comparator passed to sort(), a class's own str(). That re-entry pushes more
// frames, and with a std::vector that means a reallocation, which leaves every
// one of those references dangling. It showed up as a sort() result that was a
// denormal double, and as a crash once the list got big enough.
//
// Blocks are never resized or freed, so a register's address is stable for as
// long as its frame lives, and no amount of re-entry can move it. A frame never
// straddles a block, which is what kMaxFrameRegs guarantees.
namespace {
struct RegStack {
    static const size_t kBlock = (size_t)kMaxFrameRegs;
    std::vector<std::unique_ptr<Value[]>> blocks;
    size_t top = 0;                        // next free slot, flattened

    Value* push(size_t n, size_t& savedTop) {
        savedTop = top;
        size_t block = top / kBlock, off = top % kBlock;
        if (off + n > kBlock) { ++block; off = 0; top = block * kBlock; }  // keep it contiguous
        while (blocks.size() <= block) blocks.push_back(std::make_unique<Value[]>(kBlock));
        top += n;
        return blocks[block].get() + off;
    }
    void pop(Value* frame, size_t n, size_t savedTop) {
        for (size_t i = 0; i < n; ++i) frame[i] = Value();   // release references now
        top = savedTop;
    }
};
}  // namespace

static thread_local RegStack tlsRegs;

// BEE_DUMP_BYTECODE=1 prints each function as it is compiled. Worth having:
// "why is the annotated version slower" is not a question you can answer by
// reading the compiler, only by reading what it emitted.
static const char* const kOpNames[] = {
#define BEE_OP_NAME(n) #n,
    BEE_OPCODES(BEE_OP_NAME)
#undef BEE_OP_NAME
};

static void dumpChunk(const FunctionStmt* fn, const Chunk& ch) {
    std::fprintf(stderr, "=== %s  (%d regs, %d params)\n",
                 fn->name.empty() ? "<anonymous>" : fn->name.c_str(), ch.numRegs, ch.numParams);
    for (size_t i = 0; i < ch.code.size(); ++i) {
        const Instr& in = ch.code[i];
        std::fprintf(stderr, "  %4zu  %-14s %5u %5u %5u\n", i,
                     kOpNames[(size_t)in.op], in.a, in.b, in.c);
    }
}

Chunk* Vm::chunkFor(const FunctionStmt* fn) {
    // BEE_NO_VM=1 runs everything on the tree-walker. Kept because it is what
    // makes a differential test possible: the same program, both engines.
    static const bool disabled = [] {
        const char* v = std::getenv("BEE_NO_VM");
        return v && *v && std::string(v) != "0";
    }();
    if (disabled) return nullptr;

    auto it = chunks_.find(fn);
    if (it != chunks_.end()) return it->second.get();   // may be a cached null
    auto ch = compileFunction(fn);
    Chunk* raw = ch.get();
    static const bool dump = [] {
        const char* v = std::getenv("BEE_DUMP_BYTECODE");
        return v && *v && std::string(v) != "0";
    }();
    if (dump && raw) dumpChunk(fn, *raw);
    chunks_.emplace(fn, std::move(ch));
    return raw;
}

// A number pair for the arithmetic fast paths.
static inline bool bothNums(const Value& a, const Value& b) {
    return a.isNumber() && b.isNumber();
}

// `s = s + rhs` on a string. The compiler emits that as ADD with the destination
// and the left operand in the same register, so the pattern is visible here as
// `dst == lhs`. Growing the buffer in place makes the idiom O(n) instead of
// building a fresh string per iteration, which is O(n^2) -- the tree-walker has
// had this fast path since 0.1.1 and the VM has to match it.
//
// In place is only safe when nothing else holds the buffer: an alias such as
// `let a = s` must keep seeing the old value, so a shared string is copied.
static inline void appendInPlace(Interpreter& I, Value& dst, const Value& rhs) {
    std::string add = I.stringify(rhs);
    const std::shared_ptr<std::string>& sp = dst.strPtr();
    if (sp.use_count() == 1) *sp += add;                    // uniquely ours
    else dst = Value(std::make_shared<std::string>(*sp + add));
}

// ---- dispatch plumbing ----------------------------------------------------
#ifdef BEE_COMPUTED_GOTO
#define VM_LOOP_BEGIN   VM_GOTO();
#define VM_LOOP_END
#define VM_CASE(n)      L_##n:
#define VM_GOTO()       do { in = code[pc]; goto *kDispatch[(size_t)in.op]; } while (0)
#define VM_NEXT()       do { ++pc; VM_GOTO(); } while (0)
#else
#define VM_LOOP_BEGIN   for (;;) { in = code[pc]; switch (in.op) {
#define VM_LOOP_END     } ++pc; }
#define VM_CASE(n)      case Op::n:
#define VM_GOTO()       continue
#define VM_NEXT()       break
#endif

Value Vm::run(Interpreter& I, Chunk& ch, const std::shared_ptr<Function>& fn,
              const Value* args, size_t argc, int callLine, const Value* self) {
#ifdef BEE_COMPUTED_GOTO
    static void* const kDispatch[] = {
#define BEE_OP_LABEL(n) &&L_##n,
        BEE_OPCODES(BEE_OP_LABEL)
#undef BEE_OP_LABEL
    };
#endif

    size_t savedTop = 0;
    Value* const R = tlsRegs.push((size_t)ch.numRegs, savedTop);
    struct FrameGuard {                       // pop the frame on every exit path
        Value* frame; size_t n, savedTop;
        ~FrameGuard() { tlsRegs.pop(frame, n, savedTop); }
    } frameGuard{ R, (size_t)ch.numRegs, savedTop };

    // Frame layout, fixed by the resolver: [this?][super?][params...][locals...]
    if (self) R[0] = *self;
    else if (fn->boundThis) R[0] = Value(fn->boundThis);
    if (fn->definingClass && fn->definingClass->superclass)
        R[1] = Value(fn->definingClass->superclass);
    for (size_t i = 0; i < argc; ++i) R[ch.paramStart + (int)i] = args[i];

    // Declared parameter types are checked here, so every route into a compiled
    // function -- callFunction, a direct call, a fused method call -- enforces
    // them the same way. Unannotated parameters cost one flag test.
    if (ch.decl && ch.decl->typed) {
        for (size_t i = 0; i < argc; ++i) I.checkParamType(*ch.decl, i, args[i], callLine);
    }

    // The global inline caches are resolved against this function's closure. A
    // nested function has a different closure per outer call, so the caches are
    // dropped when the owner changes rather than being trusted blindly.
    Environment* owner = fn->closure.get();
    if (ch.cacheOwner != owner) {
        for (auto& p : ch.globalCache) p = nullptr;
        ch.cacheOwner = owner;
    }

    const Instr* code = ch.code.data();
    size_t pc = 0;
    Instr in;

    auto lineAt = [&]() { return pc < ch.lines.size() ? ch.lines[pc] : 0; };

    // Resolve (and cache) a global binding for the current GET_GLOBAL/SET_GLOBAL.
    auto globalSlot = [&](bool forWrite) -> Value* {
        Value* p = ch.globalCache[in.c];
        if (p) return p;
        p = fn->closure ? fn->closure->findNameSlot(ch.names[in.b]) : nullptr;
        if (!p) {
            I.error((forWrite ? "cannot assign to undefined variable '" : "undefined variable '") +
                    ch.names[in.b] + "'", lineAt());
        }
        ch.globalCache[in.c] = p;
        return p;
    };

    // Enter another compiled function directly. This is a function rather than
    // inline code so that the CallScope is destroyed by an ordinary return:
    // dispatch leaves an opcode's block with `goto`, and a guard object's
    // destructor cannot be relied on to run on that path -- when it did not,
    // trace frames piled up until the call-depth limit tripped.
    // `self` is non-null for a fused method call, where the receiver is passed
    // in rather than baked into a bound copy of the method.
    auto directCall = [&](Chunk& cc, const std::shared_ptr<Function>& f,
                          const Value* argv, uint16_t n, int line,
                          const Value* self = nullptr) -> Value {
        CallScope scope(I, f, line);
        return run(I, cc, f, argv, n, line, self);
    };


    // Would the LLVM JIT take this callee for the arguments about to be passed?
    // If so the call goes the long way round, through the interpreter, which
    // enters the native code -- a numeric kernel over buffers runs far faster
    // there than as bytecode. The answer is cached per signature, and a call
    // site almost always passes the same types.
    auto jitClaims = [&](Chunk& cc, const FunctionStmt* d, const Value* argv, uint16_t n) {
        if (n > kMaxJitArgs) return false;
        JitSig sig = 0;
        for (uint16_t i = 0; i < n; ++i) {
            if (argv[i].isNumber()) sig = jitSigWith(sig, i, ArgKind::Num);
            else if (argv[i].isBuffer() && argv[i].bufRef().dtype == DType::F64)
                sig = jitSigWith(sig, i, ArgKind::BufF64);
            else return false;                 // nothing else is compilable
        }
        if (!cc.nativeResolved || cc.nativeSig != sig) {
            cc.native = (void*)I.jit.getCompiled(d, sig, I);
            cc.nativeSig = sig;
            cc.nativeResolved = true;
        }
        return cc.native != nullptr;
    };

    // Walk to the environment `depth` scopes out. Depth 1 is the closure itself:
    // depth 0 is this frame, and this frame is registers, not an Environment.
    auto envAt = [&](uint16_t depth) -> Environment* {
        Environment* e = fn->closure.get();
        for (uint16_t d = 1; d < depth && e; ++d) e = e->parent.get();
        if (!e) I.error("internal: bad closure depth", lineAt());
        return e;
    };

    VM_LOOP_BEGIN

    VM_CASE(LOAD_CONST) R[in.a] = ch.constants[in.b]; VM_NEXT();
    VM_CASE(LOAD_NIL)   R[in.a] = Value();            VM_NEXT();
    VM_CASE(LOAD_BOOL)  R[in.a] = Value(in.b != 0);   VM_NEXT();
    VM_CASE(MOVE)       R[in.a] = R[in.b];            VM_NEXT();

    VM_CASE(GET_GLOBAL) R[in.a] = *globalSlot(false); VM_NEXT();
    VM_CASE(SET_GLOBAL) *globalSlot(true) = R[in.a];  VM_NEXT();
    VM_CASE(GET_ENV)    R[in.a] = envAt(in.b)->slots[in.c]; VM_NEXT();
    VM_CASE(SET_ENV)    envAt(in.b)->slots[in.c] = R[in.a]; VM_NEXT();

    // Arithmetic and comparison: a number fast path inline, everything else
    // (strings, lists, and the error messages) through the interpreter's own
    // definition, so there is only one semantics to keep straight. Each operator
    // also has a K form taking its right operand from the constant table.
#define BEE_BIN(NAME, TOK, EXPR)                                                     \
    VM_CASE(NAME) {                                                                  \
        const Value& x = R[in.b]; const Value& y = R[in.c];                          \
        if (bothNums(x, y)) R[in.a] = Value(EXPR);                                   \
        else R[in.a] = I.applyBinary(TOK, x, y, lineAt());                           \
        VM_NEXT();                                                                   \
    }                                                                                \
    VM_CASE(NAME##K) {                                                               \
        const Value& x = R[in.b]; const Value& y = ch.constants[in.c];               \
        if (bothNums(x, y)) R[in.a] = Value(EXPR);                                   \
        else R[in.a] = I.applyBinary(TOK, x, y, lineAt());                           \
        VM_NEXT();                                                                   \
    }

    // ADD is the one operator with a third case: string append, which is done
    // in place when the destination is also the left operand (`s = s + x`).
    VM_CASE(ADD) {
        const Value& x = R[in.b]; const Value& y = R[in.c];
        if (bothNums(x, y)) R[in.a] = Value(x.asNumber() + y.asNumber());
        else if (in.a == in.b && x.isString()) appendInPlace(I, R[in.a], y);
        else R[in.a] = I.applyBinary(TokenType::PLUS, x, y, lineAt());
        VM_NEXT();
    }
    VM_CASE(ADDK) {
        const Value& x = R[in.b]; const Value& y = ch.constants[in.c];
        if (bothNums(x, y)) R[in.a] = Value(x.asNumber() + y.asNumber());
        else if (in.a == in.b && x.isString()) appendInPlace(I, R[in.a], y);
        else R[in.a] = I.applyBinary(TokenType::PLUS, x, y, lineAt());
        VM_NEXT();
    }

    BEE_BIN(SUB, TokenType::MINUS, x.asNumber() - y.asNumber())
    BEE_BIN(MUL, TokenType::STAR,  x.asNumber() * y.asNumber())
    BEE_BIN(LT,  TokenType::LT,    x.asNumber() <  y.asNumber())
    BEE_BIN(LE,  TokenType::LE,    x.asNumber() <= y.asNumber())
    BEE_BIN(GT,  TokenType::GT,    x.asNumber() >  y.asNumber())
    BEE_BIN(GE,  TokenType::GE,    x.asNumber() >= y.asNumber())
#undef BEE_BIN

    // Division and modulo carry a zero check, so they get their own pair.
#define BEE_DIVLIKE(NAME, TOK, MSG, EXPR)                                            \
    VM_CASE(NAME) {                                                                  \
        const Value& x = R[in.b]; const Value& y = R[in.c];                          \
        if (bothNums(x, y)) {                                                        \
            if (y.asNumber() == 0) I.error(MSG, lineAt());                           \
            R[in.a] = Value(EXPR);                                                   \
        } else R[in.a] = I.applyBinary(TOK, x, y, lineAt());                         \
        VM_NEXT();                                                                   \
    }                                                                                \
    VM_CASE(NAME##K) {                                                               \
        const Value& x = R[in.b]; const Value& y = ch.constants[in.c];               \
        if (bothNums(x, y)) {                                                        \
            if (y.asNumber() == 0) I.error(MSG, lineAt());                           \
            R[in.a] = Value(EXPR);                                                   \
        } else R[in.a] = I.applyBinary(TOK, x, y, lineAt());                         \
        VM_NEXT();                                                                   \
    }

    BEE_DIVLIKE(DIV, TokenType::SLASH,   "division by zero", x.asNumber() / y.asNumber())
    BEE_DIVLIKE(MOD, TokenType::PERCENT, "modulo by zero",   beeMod(x.asNumber(), y.asNumber()))
#undef BEE_DIVLIKE

    // The typed forms. Both operands are numbers by construction -- a declared
    // annotation was enforced where the value entered -- so there is no tag
    // test, no fallback branch and no checked accessor, just the arithmetic.
#define BEE_BIN_NUM(NAME, EXPR)                                                      \
    VM_CASE(NAME##_NUM) {                                                            \
        const double a = R[in.b].num(), b = R[in.c].num(); (void)b;                   \
        R[in.a] = Value(EXPR);                                                       \
        VM_NEXT();                                                                   \
    }                                                                                \
    VM_CASE(NAME##K_NUM) {                                                           \
        const double a = R[in.b].num(), b = ch.constants[in.c].num(); (void)b;        \
        R[in.a] = Value(EXPR);                                                       \
        VM_NEXT();                                                                   \
    }

    BEE_BIN_NUM(ADD, a + b)
    BEE_BIN_NUM(SUB, a - b)
    BEE_BIN_NUM(MUL, a * b)
    BEE_BIN_NUM(LT,  a <  b)
    BEE_BIN_NUM(LE,  a <= b)
    BEE_BIN_NUM(GT,  a >  b)
    BEE_BIN_NUM(GE,  a >= b)
    BEE_BIN_NUM(EQ,  a == b)
    BEE_BIN_NUM(NE,  a != b)
#undef BEE_BIN_NUM

    // Division still needs its zero check; only the type tests go away.
#define BEE_DIV_NUM(NAME, MSG, EXPR)                                                 \
    VM_CASE(NAME##_NUM) {                                                            \
        const double a = R[in.b].num(), b = R[in.c].num();                            \
        if (b == 0) I.error(MSG, lineAt());                                          \
        R[in.a] = Value(EXPR);                                                       \
        VM_NEXT();                                                                   \
    }                                                                                \
    VM_CASE(NAME##K_NUM) {                                                           \
        const double a = R[in.b].num(), b = ch.constants[in.c].num();                 \
        if (b == 0) I.error(MSG, lineAt());                                          \
        R[in.a] = Value(EXPR);                                                       \
        VM_NEXT();                                                                   \
    }

    BEE_DIV_NUM(DIV, "division by zero", a / b)
    BEE_DIV_NUM(MOD, "modulo by zero",   beeMod(a, b))
#undef BEE_DIV_NUM

    // A buffer holds unboxed numbers in contiguous memory, so a typed index is
    // a bounds check and a load -- no tag tests on either side.
    VM_CASE(INDEX_BUF) {
        Buffer& b = R[in.b].bufRef();   // borrowed: no refcount traffic
        long long i = (long long)R[in.c].num();
        if (i < 0) i += (long long)b.count();
        if (i < 0 || (size_t)i >= b.count())
            I.error("buffer index out of range (" + std::to_string(b.count()) +
                    " element(s))", lineAt());
        R[in.a] = Value(b.get((size_t)i));
        VM_NEXT();
    }
    VM_CASE(INDEX_SET_BUF) {
        Buffer& b = R[in.a].bufRef();   // borrowed: no refcount traffic
        long long i = (long long)R[in.b].num();
        if (i < 0) i += (long long)b.count();
        if (i < 0 || (size_t)i >= b.count())
            I.error("buffer index out of range (" + std::to_string(b.count()) +
                    " element(s))", lineAt());
        b.set((size_t)i, R[in.c].num());
        VM_NEXT();
    }

    VM_CASE(BAND) R[in.a] = I.applyBinary(TokenType::BIT_AND, R[in.b], R[in.c], lineAt()); VM_NEXT();
    VM_CASE(BOR)  R[in.a] = I.applyBinary(TokenType::BIT_OR,  R[in.b], R[in.c], lineAt()); VM_NEXT();
    VM_CASE(BXOR) R[in.a] = I.applyBinary(TokenType::BIT_XOR, R[in.b], R[in.c], lineAt()); VM_NEXT();
    VM_CASE(SHL)  R[in.a] = I.applyBinary(TokenType::SHL,     R[in.b], R[in.c], lineAt()); VM_NEXT();
    VM_CASE(SHR)  R[in.a] = I.applyBinary(TokenType::SHR,     R[in.b], R[in.c], lineAt()); VM_NEXT();

    // Compound assignment (`xs[i] += 1`, `obj.f -= 2`) has its own arithmetic
    // helper in the interpreter, with its own error messages.
    VM_CASE(AADD) R[in.a] = I.applyBinaryArith(TokenType::PLUS,  R[in.b], R[in.c], lineAt()); VM_NEXT();
    VM_CASE(ASUB) R[in.a] = I.applyBinaryArith(TokenType::MINUS, R[in.b], R[in.c], lineAt()); VM_NEXT();
    VM_CASE(AMUL) R[in.a] = I.applyBinaryArith(TokenType::STAR,  R[in.b], R[in.c], lineAt()); VM_NEXT();
    VM_CASE(ADIV) R[in.a] = I.applyBinaryArith(TokenType::SLASH, R[in.b], R[in.c], lineAt()); VM_NEXT();

    // Equality works on every type, so it needs no number check -- but the
    // number case is still worth taking without a call.
    VM_CASE(EQ)
        if (bothNums(R[in.b], R[in.c])) R[in.a] = Value(R[in.b].asNumber() == R[in.c].asNumber());
        else R[in.a] = Value(I.valuesEqual(R[in.b], R[in.c]));
        VM_NEXT();
    VM_CASE(EQK)
        if (bothNums(R[in.b], ch.constants[in.c]))
            R[in.a] = Value(R[in.b].asNumber() == ch.constants[in.c].asNumber());
        else R[in.a] = Value(I.valuesEqual(R[in.b], ch.constants[in.c]));
        VM_NEXT();
    VM_CASE(NE)
        if (bothNums(R[in.b], R[in.c])) R[in.a] = Value(R[in.b].asNumber() != R[in.c].asNumber());
        else R[in.a] = Value(!I.valuesEqual(R[in.b], R[in.c]));
        VM_NEXT();
    VM_CASE(NEK)
        if (bothNums(R[in.b], ch.constants[in.c]))
            R[in.a] = Value(R[in.b].asNumber() != ch.constants[in.c].asNumber());
        else R[in.a] = Value(!I.valuesEqual(R[in.b], ch.constants[in.c]));
        VM_NEXT();

    VM_CASE(NEG)
        if (!R[in.b].isNumber()) I.error("operand of unary '-' must be a number", lineAt());
        R[in.a] = Value(-R[in.b].asNumber());
        VM_NEXT();
    VM_CASE(NOT)  R[in.a] = Value(!R[in.b].truthy()); VM_NEXT();
    VM_CASE(BNOT)
        if (!R[in.b].isNumber()) I.error("operand of unary '~' must be a number", lineAt());
        R[in.a] = Value((double)(~(long long)R[in.b].asNumber()));
        VM_NEXT();

    VM_CASE(JUMP)          pc = in.b; VM_GOTO();
    VM_CASE(JUMP_IF_FALSE) if (!R[in.a].truthy()) { pc = in.b; VM_GOTO(); } VM_NEXT();
    VM_CASE(JUMP_IF_TRUE)  if (R[in.a].truthy())  { pc = in.b; VM_GOTO(); } VM_NEXT();

    VM_CASE(NEW_LIST) {
        auto l = std::make_shared<ValueList>();
        l->reserve(in.c);
        for (uint16_t i = 0; i < in.c; ++i) l->push_back(R[in.b + i]);
        R[in.a] = Value(l);
        VM_NEXT();
    }
    VM_CASE(NEW_DICT) {
        auto d = std::make_shared<ValueDict>();
        for (uint16_t i = 0; i < in.c; ++i)
            (*d)[I.keyString(R[in.b + 2 * i])] = R[in.b + 2 * i + 1];
        R[in.a] = Value(d);
        VM_NEXT();
    }
    VM_CASE(LIST_PUSH) R[in.a].listRef().push_back(R[in.b]); VM_NEXT();

    // Indexing a list by a number is the single most common data operation in
    // Bee code, so it is done here rather than through a call.
    VM_CASE(INDEX) {
        const Value& o = R[in.b];
        const Value& x = R[in.c];
        if (o.isList() && x.isNumber()) {
            const ValueList& l = o.listRef();
            long long i = (long long)x.asNumber();
            if (i < 0) i += (long long)l.size();
            if (i < 0 || i >= (long long)l.size()) I.error("list index out of range", lineAt());
            // The destination may *be* the object register -- `sort(xs)[0]`
            // compiles to INDEX t, t, k. Assigning into it would drop the last
            // reference to the list and free it while `l` still points inside,
            // so the element is copied out first.
            Value v = l[(size_t)i];
            R[in.a] = std::move(v);
        } else {
            R[in.a] = I.indexGet(o, x, lineAt());
        }
        VM_NEXT();
    }
    VM_CASE(INDEX_SET) {
        const Value& o = R[in.a];
        const Value& x = R[in.b];
        if (o.isList() && x.isNumber()) {
            ValueList& l = o.listRef();
            long long i = (long long)x.asNumber();
            if (i < 0) i += (long long)l.size();
            if (i < 0 || i >= (long long)l.size()) I.error("list index out of range", lineAt());
            l[(size_t)i] = R[in.c];
        } else {
            I.indexSet(o, x, R[in.c], lineAt());
        }
        VM_NEXT();
    }
    // Field access through the site's shape cache: a pointer compare and an
    // array index while the shape holds, the general path when it does not.
    VM_CASE(GET_PROP) {
        PropSite& site = ch.sites[in.c];
        const Value& obj = R[in.b];
        if (obj.isInstance()) {
            Instance* inst = obj.instRef().get();
            if (inst->shape == site.shape) { R[in.a] = inst->slots[(size_t)site.slot]; VM_NEXT(); }
            int slot = inst->shape->slotOf(ch.names[site.name]);
            if (slot >= 0) {
                site.shape = inst->shape;
                site.slot = slot;
                R[in.a] = inst->slots[(size_t)slot];
                VM_NEXT();
            }
            // Not a field: a method, or an error. Both live in getProperty.
        }
        R[in.a] = I.getProperty(obj, ch.names[site.name], lineAt());
        VM_NEXT();
    }
    VM_CASE(SET_PROP) {
        PropSite& site = ch.sites[in.b];
        if (!R[in.a].isInstance()) I.error("only instances have fields", lineAt());
        Instance* inst = R[in.a].instRef().get();
        if (inst->shape == site.shape) { inst->slots[(size_t)site.slot] = R[in.c]; VM_NEXT(); }
        // A miss is either a field this shape already has, or one it gains now,
        // which moves the instance to a new shape. Cache the hit; a transition
        // is not cached, because the shape it started from is not the one it
        // ends in.
        Shape* before = inst->shape;
        int slot = before->slotOf(ch.names[site.name]);
        inst->setField(ch.names[site.name], R[in.c]);
        if (slot >= 0) { site.shape = before; site.slot = slot; }
        VM_NEXT();
    }
    VM_CASE(SUPER)
        R[in.a] = I.superMethod(R[in.c + 1], R[in.c], ch.names[in.b], lineAt());
        VM_NEXT();
    VM_CASE(SLICE) R[in.a] = I.sliceValue(R[in.b], R[in.c], R[in.c + 1], lineAt()); VM_NEXT();

    VM_CASE(CALL) {
        // Arguments are already contiguous in registers. When the callee is
        // another compiled Bee function taking exactly these arguments, it can
        // be entered from here: no argument vector, no callFunction, and the
        // callee's registers are copied straight out of ours. Everything else
        // -- built-ins, classes, defaults, rest parameters, uncompiled bodies,
        // and numeric functions the LLVM JIT claimed -- goes through the
        // interpreter, so it all behaves exactly as before.
        const Value& calleeV = R[in.a];
        if (calleeV.isFunction()) {
            const std::shared_ptr<Function>& f = calleeV.funcRef();
            const FunctionStmt* decl = f->decl;
            if (decl && !f->isInitializer && decl->restParam < 0 &&
                in.b == decl->params.size()) {
                if (Chunk* cc = chunkFor(decl)) {
                    if (!jitClaims(*cc, decl, R + in.a + 1, in.b)) {
                        R[in.a] = directCall(*cc, f, R + in.a + 1, in.b, lineAt());
                        VM_NEXT();
                    }
                }
            }
        }
        std::vector<Value> argv(R + in.a + 1, R + in.a + 1 + in.b);
        R[in.a] = I.callValue(R[in.a], argv, lineAt());
        VM_NEXT();
    }

    VM_CASE(CALL_METHOD) {
        // `obj.m(args)`, without ever materialising `obj.m`. Reading a method
        // as a value means allocating a copy of it bound to the receiver; here
        // the receiver is simply passed in, so a method call costs no more than
        // a function call.
        PropSite& site = ch.sites[in.b];
        const Value& recv = R[in.a];
        if (recv.isInstance()) {
            const std::shared_ptr<Instance>& inst = recv.instRef();
            // A field holding a callable shadows a method, exactly as
            // getProperty has it, so fields are checked first.
            if (inst->shape->slotOf(ch.names[site.name]) < 0) {
                Class* k = inst->klass.get();
                if (site.klass != k) {              // fill the cache for this class
                    site.klass = k;
                    site.method = inst->klass->findMethod(ch.names[site.name]);
                }
                const std::shared_ptr<Function>& mth = site.method;
                if (mth && mth->decl && !mth->isInitializer &&
                    mth->decl->restParam < 0 && in.c == mth->decl->params.size()) {
                    if (Chunk* cc = chunkFor(mth->decl)) {
                        if (!jitClaims(*cc, mth->decl, R + in.a + 1, in.c)) {
                            R[in.a] = directCall(*cc, mth, R + in.a + 1, in.c, lineAt(), &recv);
                            VM_NEXT();
                        }
                    }
                }
            }
        }
        Value callee = I.getProperty(recv, ch.names[site.name], lineAt());
        std::vector<Value> argv(R + in.a + 1, R + in.a + 1 + in.c);
        R[in.a] = I.callValue(callee, argv, lineAt());
        VM_NEXT();
    }

    VM_CASE(ITER_PREP) {
        const Value& src = R[in.b];
        if (src.isList()) {
            R[in.a] = src;              // iterate the live list, as the tree-walker does
        } else if (src.isString()) {
            auto l = std::make_shared<ValueList>();
            for (char c : src.asString()) l->push_back(Value(std::string(1, c)));
            R[in.a] = Value(l);
        } else if (src.isDict()) {
            auto l = std::make_shared<ValueList>();
            for (auto& kv : src.dictRef()) l->push_back(Value(kv.first));
            R[in.a] = Value(l);
        } else {
            I.error("value is not iterable", lineAt());
        }
        R[in.a + 1] = Value(0.0);
        VM_NEXT();
    }
    VM_CASE(ITER_NEXT) {
        const ValueList& l = R[in.a].listRef();
        size_t i = (size_t)R[in.a + 1].asNumber();
        if (i >= l.size()) { pc = in.b; VM_GOTO(); }
        // Copied out before the store, for the same reason as INDEX: writing a
        // register that holds the last reference to the list would free it
        // while `l` still points inside. The compiler does not currently emit
        // an aliasing pair here, but that is not a property worth relying on.
        Value v = l[i];
        R[in.c] = std::move(v);
        R[in.a + 1] = Value((double)(i + 1));
        VM_NEXT();
    }

    VM_CASE(CHECK_TYPE) {
        const TypeCheck& tc = ch.typeChecks[in.b];
        I.checkDeclared(*tc.type, tc.name, R[in.a], tc.line);
        VM_NEXT();
    }

    VM_CASE(RETURN)
        if (ch.decl && ch.decl->returnType.declared())
            I.checkReturnType(*ch.decl, R[in.a], lineAt());
        return R[in.a];
    VM_CASE(RETURN_NIL)
        // Falling off the end yields nil, which a declared return type rejects.
        if (ch.decl && ch.decl->returnType.declared())
            I.checkReturnType(*ch.decl, Value(), callLine);
        return Value();
    VM_CASE(HALT)       return Value();

    VM_LOOP_END
}

} // namespace bee
