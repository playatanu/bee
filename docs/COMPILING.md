# Compiling to a native binary

Bee ships with `beec`, an ahead-of-time (AOT) compiler. It turns a `.bee`
program into a standalone native executable - a real binary you can ship and
run on its own, with no `bee` interpreter installed.

```bash
beec hello.bee -o hello   # compile
./hello                   # run the native binary
```

The produced file is an ordinary executable (an ELF binary on Linux). The whole
program - its control flow, functions, and calls - is compiled to machine code,
and the Bee runtime is linked in, so nothing is interpreted at run time.

## Usage

```bash
beec <input.bee> [options]
```

| Option | Meaning |
|--------|---------|
| `-o <file>` | Name of the output executable (default: the input name without its extension). |
| `-O <level>` | Optimisation level passed to the C++ compiler (default: `2`). |
| `--emit-cpp` | Also keep the generated C++ next to the output, for inspection. |
| `-v`, `--version` | Print the version. |
| `-h`, `--help` | Print help. |

## How it works

`beec` reuses Bee's own lexer, parser, and resolver, then generates C++ from the
program: each Bee function becomes a native function, variables become
reference-counted cells (so closures work), and every operation is handed to the
same runtime the interpreter uses. It then invokes your system C++ compiler to
link everything into one binary.

Because of that last step, **the machine that runs `beec` needs a C++17
compiler** (the same one you built Bee with). The machine that runs the *output*
needs nothing.

## What you can compile

The AOT compiler supports the whole language:

- variables, and all arithmetic / comparison / logical / bitwise operators
- strings (indexing, slicing, interpolation, methods), lists, and dicts
- `if` / `while` / both `for` forms / `break` / `continue` / `match`
- functions, closures, recursion, first-class functions, default and `...rest`
  parameters
- classes, inheritance, `this`, `super`, `new`, and custom `str()`
- list comprehensions and destructuring `let`
- `try` / `catch` / `finally` and `throw`
- threads (`spawn` / `join`)
- `import` / `from ... import` - resolved just as the interpreter does (sibling
  `.bee`/`.be` files, a `lib/` folder, `hive_modules/` packages found up the
  tree honouring each package's `hive.json` `main`, `$BEE_PATH`, and
  `~/.hive/lib`) and compiled into the same binary, dependencies and all
- the entire built-in standard library

## Not yet supported

A couple of things still need the interpreter - `beec` reports them (with a file
and line) rather than miscompiling:

- native modules: a compiled C/C++ extension (`.so`) imported by name. The
  pure-Bee module system, including installed packages, is fully supported.
- `import` statements that aren't at the top level of a file

## A note on speed

A compiled binary is standalone, but it is not automatically *faster* than the
interpreter. Arithmetic still flows through Bee's dynamic runtime, and the
interpreter already compiles hot numeric loops to native code with its
[JIT](PERFORMANCE.md). Making AOT output match or beat the JIT on numeric code,
by specialising on types, is planned work rather than a promise today.

## Requirements

- A C++17 compiler on the machine that runs `beec` (`c++` by default; override
  with the `BEE_CXX` environment variable). The machine that runs the compiled
  binary needs nothing.
- On Windows, run `beec` from an MSYS2 / MinGW shell, whose `g++` is the same
  GCC/Clang-style compiler Bee is built with. The compiled `.exe` is standalone.
  (AOT is currently tested on Linux; Windows and macOS are best-effort.)
- On a relocated install, point `beec` at the runtime with `BEE_AOT_INCDIR` (the
  directory holding `bee_aot.hpp`) and `BEE_AOT_RUNTIME_LIB` (the path to
  `libbee_runtime.a`).
