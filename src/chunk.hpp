#pragma once
//
// Bytecode for Bee's register VM.
//
// The tree-walker spends most of its time on the walk itself: a switch, a
// virtual-ish dispatch, a pointer chase and a returned Value per AST node. A
// flat instruction stream removes the recursion and the pointer chasing, and a
// *register* design (rather than a stack) removes most of the dispatches too --
// `s = s + xs[i]` is two instructions here where a stack VM needs six.
//
// Registers are the frame's slots. The resolver already assigns every local a
// slot in its frame, and (since scope merging) every scope inside a closure-free
// function is merged into that one frame -- so locals *are* registers 0..n, and
// the compiler allocates temporaries above them.
//
#include "ast.hpp"        // TypeAnn, and the AST nodes a chunk points back at
#include "jit.hpp"        // JitSig: native code is cached per argument signature
#include "value.hpp"
#include "token.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace bee {

// The opcodes are listed once, here, and both the enum and the VM's dispatch
// table are generated from this list -- so they cannot drift apart, which is the
// one way a computed-goto interpreter goes badly wrong.
//
//   LOAD_CONST     R[a] = K[b]
//   LOAD_NIL       R[a] = nil
//   LOAD_BOOL      R[a] = (bool)b
//   MOVE           R[a] = R[b]
//   GET_GLOBAL     R[a] = *globalCache[c]   (name K[b]; c indexes the cache table)
//   SET_GLOBAL     *globalCache[c] = R[a]
//   GET_ENV        R[a] = closure env at depth b, slot c
//   SET_ENV        closure env at depth b, slot c = R[a]
//   ADD..NE        R[a] = R[b] op R[c]
//   <op>K          R[a] = R[b] op K[c] -- the same operator with a literal right
//                  operand (`i + 1`, `i < n`, `x % 2 == 0`), which is most of
//                  what a loop does and saves a LOAD_CONST and a register each
//   AADD..ADIV     R[a] = R[b] op R[c], with compound-assignment semantics
//   NEG/NOT/BNOT   R[a] = op R[b]
//   JUMP           pc = b
//   JUMP_IF_FALSE  if (!truthy(R[a])) pc = b
//   JUMP_IF_TRUE   if ( truthy(R[a])) pc = b
//   NEW_LIST       R[a] = list of R[b .. b+c)
//   NEW_DICT       R[a] = dict from c key/value pairs at R[b], R[b+1], ...
//   LIST_PUSH      append R[b] to the list in R[a]
//   INDEX          R[a] = R[b][R[c]]
//   INDEX_SET      R[a][R[b]] = R[c]
//   GET_PROP       R[a] = R[b].<site c>
//   SET_PROP       R[a].<site b> = R[c]
//   CALL_METHOD    R[a] = R[a].<site b>(R[a+1] .. R[a+1+c))
//   SUPER          R[a] = super.K[b] bound to `this` in R[c]  (R[c+1] = superclass)
//   SLICE          R[a] = R[b][R[c] : R[c+1]]  (nil bound => open end)
//   CALL           R[a] = R[a](R[a+1] .. R[a+1+b))
//   RETURN         return R[a]
//   ITER_PREP      R[a] = iterable R[b] normalised to a list; R[a+1] = 0
//   ITER_NEXT      if exhausted pc = b, else R[c] = next item
//
#define BEE_OPCODES(X)                                                        \
    X(LOAD_CONST) X(LOAD_NIL) X(LOAD_BOOL) X(MOVE)                            \
    X(GET_GLOBAL) X(SET_GLOBAL) X(GET_ENV) X(SET_ENV)                         \
    X(ADD) X(ADDK) X(SUB) X(SUBK) X(MUL) X(MULK)                              \
    X(DIV) X(DIVK) X(MOD) X(MODK)                                             \
    X(BAND) X(BOR) X(BXOR) X(SHL) X(SHR)                                      \
    X(LT) X(LTK) X(LE) X(LEK) X(GT) X(GTK) X(GE) X(GEK)                       \
    X(EQ) X(EQK) X(NE) X(NEK)                                                 \
    X(AADD) X(ASUB) X(AMUL) X(ADIV)                                           \
    /* Typed forms: both operands are known numbers, so no tag test and no    \
       fallback branch. Emitted only where a declared annotation guarantees   \
       it -- see the compiler's register typing. */                           \
    X(ADD_NUM) X(ADDK_NUM) X(SUB_NUM) X(SUBK_NUM)                             \
    X(MUL_NUM) X(MULK_NUM) X(DIV_NUM) X(DIVK_NUM) X(MOD_NUM) X(MODK_NUM)      \
    X(LT_NUM) X(LTK_NUM) X(LE_NUM) X(LEK_NUM)                                 \
    X(GT_NUM) X(GTK_NUM) X(GE_NUM) X(GEK_NUM)                                 \
    X(EQ_NUM) X(EQK_NUM) X(NE_NUM) X(NEK_NUM)                                 \
    X(INDEX_BUF) X(INDEX_SET_BUF)                                             \
    X(NEG) X(NOT) X(BNOT)                                                     \
    X(JUMP) X(JUMP_IF_FALSE) X(JUMP_IF_TRUE)                                  \
    X(NEW_LIST) X(NEW_DICT) X(LIST_PUSH)                                      \
    X(INDEX) X(INDEX_SET) X(GET_PROP) X(SET_PROP) X(SUPER) X(SLICE)           \
    X(CALL) X(CALL_METHOD) X(RETURN) X(RETURN_NIL) X(CHECK_TYPE) X(COERCE)    \
    X(ITER_PREP) X(ITER_NEXT)                                                 \
    X(HALT)

enum class Op : uint16_t {
#define BEE_OP_ENUM(n) n,
    BEE_OPCODES(BEE_OP_ENUM)
#undef BEE_OP_ENUM
};

// One instruction. Eight bytes, three operands, no variable-length decoding.
// Registers, constant indices and jump targets are all 16-bit, so a function
// needing more than 65535 of anything falls back to the tree-walker.
struct Instr {
    Op op;
    uint16_t a = 0, b = 0, c = 0;
    Instr() : op(Op::HALT) {}
    Instr(Op o, uint16_t x = 0, uint16_t y = 0, uint16_t z = 0) : op(o), a(x), b(y), c(z) {}
};

static const uint16_t kMaxOperand = 0xFFFF;

// A frame's registers must fit in one block of the VM's register stack, so
// that block allocation never has to split a frame. A function wanting more
// than this runs on the tree-walker instead; real ones use tens.
static const int kMaxFrameRegs = 4096;

// One property or method-call site, and what it saw last time.
//
// Instances of a class share a Shape, so "this shape means slot 3" holds for
// every instance a site is likely to see, and a field access becomes a pointer
// compare and an array index. Method sites cache the resolved method for a
// class the same way, which also avoids rebuilding a bound method per call.
// Both caches are checked, never trusted: a miss just falls back to the general
// path and re-fills.
struct PropSite {
    uint16_t name = 0;                     // index into Chunk::names
    Shape* shape = nullptr;                // last shape seen
    int slot = -1;                         // that shape's slot for `name`
    Class* klass = nullptr;                // last class seen, for method calls
    std::shared_ptr<Function> method;      // its method of that name (kept alive)
};

// One CHECK_TYPE site: an annotated binding being written, and what it must be.
struct TypeCheck {
    const TypeAnn* type = nullptr;   // borrowed from the AST
    std::string name;
    int line = 0;
};

// A compiled function body.
struct Chunk {
    std::vector<Instr> code;
    std::vector<int> lines;             // parallel to code; consulted only on error
    std::vector<Value> constants;
    std::vector<std::string> names;     // global names and property names
    // One cache slot per GET_GLOBAL / SET_GLOBAL site. Filled on first execution
    // with a pointer to the binding, exactly like the tree-walker's inline cache.
    // std::map keeps element pointers stable, and a global is never erased.
    std::vector<Value*> globalCache;
    std::vector<PropSite> sites;        // one per GET_PROP / SET_PROP / CALL_METHOD
    // Which closure the caches were resolved against. A nested function gets a
    // different closure per call of its enclosing function, so the caches are
    // dropped when this changes rather than being trusted across owners.
    Environment* cacheOwner = nullptr;
    // Whether the LLVM JIT also claimed this function, resolved once. A numeric
    // function runs faster natively than as bytecode, so the VM's direct-call
    // path declines those and lets callFunction take its native route.
    // Native code exists per argument signature, so the answer is cached along
    // with the signature it was asked about; a call site almost always passes
    // the same types, making this a single comparison.
    void* native = nullptr;
    JitSig nativeSig = 0;
    bool nativeResolved = false;
    int numRegs = 0;                    // frame slots + temporaries
    int paramStart = 0;
    int numParams = 0;
    // The declaration this was compiled from, for the names and declared types
    // an error message needs. The AST outlives the interpreter.
    const FunctionStmt* decl = nullptr;
    // The annotated bindings this body writes to, in CHECK_TYPE operand order.
    // The TypeAnn is borrowed from the AST, which outlives every chunk.
    std::vector<TypeCheck> typeChecks;
};

} // namespace bee
