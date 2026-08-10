#pragma once
#include "chunk.hpp"
#include <memory>
#include <unordered_map>

namespace bee {

class Interpreter;
struct Function;
struct FunctionStmt;

// The register VM: compiles a function body once, then executes the bytecode
// instead of walking the AST. Functions it cannot compile keep running on the
// tree-walker, so this is purely additive -- see compiler.hpp.
class Vm {
public:
    // The compiled body for `fn`, compiling on first sight. Null means "runs on
    // the tree-walker"; that answer is cached too, so a function is only ever
    // offered to the compiler once.
    Chunk* chunkFor(const FunctionStmt* fn);

    // Execute a compiled body. `args` holds exactly the declared parameters.
    // `callLine` is where the call was written, which is where a bad argument
    // is reported -- the same place the tree-walker blames. `self` overrides the
    // receiver for a fused method call, where the method was never bound to a
    // copy of itself; null means "use fn->boundThis".
    Value run(Interpreter& I, Chunk& ch, const std::shared_ptr<Function>& fn,
              const Value* args, size_t argc, int callLine, const Value* self = nullptr);
    Value run(Interpreter& I, Chunk& ch, const std::shared_ptr<Function>& fn,
              std::vector<Value>& args, int callLine) {
        return run(I, ch, fn, args.data(), args.size(), callLine);
    }

private:
    std::unordered_map<const FunctionStmt*, std::unique_ptr<Chunk>> chunks_;
};

} // namespace bee
