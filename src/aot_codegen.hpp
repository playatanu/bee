#pragma once
#include "ast.hpp"
#include <string>
#include <vector>

namespace bee {

// A construct the AOT compiler doesn't handle yet, with where it was found.
struct AotError {
    int line;
    std::string msg;
};

// An imported module to compile into the same binary. `program` is the module's
// resolved AST; `id` is a C++-safe suffix; `publicNames` lists its top-level,
// non-underscore bindings (for `from M import *`).
struct AotModule {
    std::string id;
    std::string name;                    // as written in `import` ('/'-joined)
    const Program* program;
    std::vector<std::string> publicNames;
};

// Translate a resolved Bee program (plus any imported modules) into a
// self-contained C++ translation unit that #includes "bee_aot.hpp". On success,
// `errors` is left empty. When the program uses a construct the compiler doesn't
// support yet, every such site is recorded in `errors`.
std::string aotGenerate(const Program& program, const std::string& sourceName,
                        const std::vector<AotModule>& modules,
                        std::vector<AotError>& errors);

} // namespace bee
