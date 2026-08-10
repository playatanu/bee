#pragma once
#include "chunk.hpp"
#include <memory>

namespace bee {

struct FunctionStmt;

// Compile a function body to register bytecode, or return nullptr if it uses
// something the compiler does not cover yet (a nested function or class, `try`,
// `import`, destructuring, ...). A null result is not an error: the caller runs
// the function on the tree-walker instead, exactly as before.
//
// Anything that creates a closure disqualifies a function, and that is what
// makes registers sound: the resolver merges every scope of a closure-free
// function into its frame, so all its locals live at depth 0 and no
// Environment is needed for the frame at all.
std::unique_ptr<Chunk> compileFunction(const FunctionStmt* fn);

} // namespace bee
