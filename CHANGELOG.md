# Changelog

All notable changes to **BeeLang** are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.1.1] — 2026-08-06

Performance work focused on loops, condition checking, and string building. No
language or API changes — existing programs run identically, only faster. All
14 examples and the correctness suites pass unchanged.

### Performance

- **Inline cache for global variables.** Reads/writes of top-level (named)
  variables now go through a cached pointer instead of a `std::map` lookup on
  every access. Top-level loops that were ~3.7× slower than Python are now
  ~2.7× faster than before this change.
- **Cross-function JIT.** The native JIT previously handled only direct
  self-recursion; it now compiles a whole numeric call graph — **helpers and
  mutual recursion** — into one module, so those calls are direct and
  inlinable. A helper-calling loop (e.g. a prime sieve) that used to fall back
  to the interpreter now runs ~6× faster than Python.
- **Top-level loops are JIT-compiled automatically.** A numeric top-level
  `while`/`for` no longer needs to be wrapped in a function to hit native
  speed. The numeric globals a loop touches are passed as an in/out array,
  loaded on entry and written back only on clean completion; anything
  non-numeric (a `print`, a string, division by zero) transparently falls back
  to the interpreter from unmodified state. A 10 M-iteration top-level loop
  dropped from ~1.84 s to ~0.03 s (~16× faster than Python).
- **In-place string append.** The `x = x + rhs` / `x += rhs` idiom was O(n²) —
  a fresh, growing string each iteration (~246× slower than Python). It now
  grows the buffer in place when the string is not aliased, falling back to a
  copy to preserve value semantics otherwise. Building a 200 k-character string
  went from ~2.46 s to ~0.03 s (~82× faster).
- **JIT warmup guard.** A small, flat `for` loop with a compile-time-known trip
  count below ~40 k now runs interpreted instead of paying the ~3 ms one-time
  native compilation, which for such loops costs more than it saves. Nested,
  large, or unanalyzable loops still compile as before. A 10 k-iteration loop
  dropped from ~4.3 ms to ~0.5 ms, matching Python.
- **Void numeric functions no longer run twice.** A function that falls off the
  end (no value-returning `return`) used to be compiled, executed natively, then
  discarded and re-run in the interpreter — doubling the work. Native completion
  now reports a nil result directly, so such functions keep their native run. A
  100 M-iteration side-effect-free function dropped from ~6 s to milliseconds;
  a 10 M-iteration accumulating loop runs ~20× faster than Python.

## [0.1.0] — 2026-08-06

The first public release of BeeLang — a small, friendly scripting language with
a built-in native (LLVM) JIT. 🐝

### Language

- Dynamically-typed values: `nil`, booleans, numbers (64-bit float), strings,
  lists, and dicts.
- Variables with `let`, lexical scoping, and block/function/loop scopes.
- Full operator set: arithmetic, comparison, logical (`and`/`or`/`not` with
  `&&`/`||`/`!` aliases), and compound assignment (`+=`, `-=`, `*=`, `/=`).
- Control flow: `if` / `else if` / `else`, `while`, C-style `for`, and
  `for … in` over lists, strings, and dicts, plus `break` / `continue`.
- First-class functions and closures.
- Classes with single inheritance (`extends`), `this`, `super`, an `init`
  constructor, and a customizable `str()` for printing.
- A module system: `import`, `import … as`, `from … import`, and
  `from … import *`, resolved relative to the file and a sibling `lib/` folder.
- Error handling with `try` / `catch` / `finally` and `throw` of any value.
- Two comment styles (`#` and `//`) and an optional statement terminator (`;`).

### Runtime & performance

- Compact C++17 runtime with no required third-party dependencies.
- **Built-in LLVM ORCv2 JIT** (enabled by default when LLVM 17/18 is present):
  functions within a numeric subset are compiled to native code operating on
  unboxed doubles, transparently falling back to the runtime for everything
  else. `fib(32)` drops from ~11 s to ~0.014 s.
- `bee --version` / `-v` and `bee --help` / `-h`.

### Standard library (no imports required)

- **I/O:** `print`, `write`, `input`.
- **Conversion & inspection:** `len`, `type`, `str`, `repr`, `num`, `int`,
  `bool`.
- **Math:** `abs`, `floor`, `ceil`, `round`, `sqrt`, `pow`, `min`, `max`,
  `range`.
- **Files:** `read_file`, `read_lines`, `write_file`, `append_file`,
  `file_exists`, `remove_file`, `make_dir`, `list_dir`.
- **Time & randomness:** `clock`, `time`, `now`, `format_time`, `sleep`,
  `random`, `random_int`, `random_range`, `random_choice`, `random_seed`.
- **Environment & processes:** `env`, `set_env`, `args`, `exec`.
- **Threads:** `spawn` / `join` with a global interpreter lock (GIL) — safe
  shared state and real overlap for I/O-bound work.
- Type methods for strings, lists, and dicts (`upper`, `split`, `push`, `pop`,
  `keys`, `has`, `get`, …).

### Tooling & packaging

- `Makefile` build with automatic LLVM JIT detection and an overridable
  `VERSION`.
- **Debian/Ubuntu `.deb`** package ([`packaging/build-deb.sh`](packaging/build-deb.sh))
  built with the JIT; dependencies are auto-detected.
- **Windows installer** ([`packaging/windows/`](packaging/windows/)) via Inno
  Setup — adds `bee` to `PATH`, associates `.be` / `.bee` files, and registers
  an uninstaller.
- **GitHub Actions release workflow** that builds both packages and publishes
  them on a version tag.

### Editor support

- **VS Code extension** ([`editors/vscode-bee/`](editors/vscode-bee/)) with
  syntax highlighting, completions (keywords, built-ins, type methods, and file
  symbols), hovers, snippets, and a bee icon for `.be` / `.bee` files.

### Documentation & examples

- Full [language reference](docs/LANGUAGE.md).
- A topic-by-topic set of 14 runnable [examples](examples/).

### Known limitations

- The Windows installer ships an interpreter-only build (no JIT yet).
- The `.deb` requires `libllvm18`, available on Ubuntu 24.04 and newer.
- No anonymous/lambda functions — pass named functions (e.g. to `spawn`).

[0.1.1]: https://github.com/playatanu/beelang/releases/tag/v0.1.1
[0.1.0]: https://github.com/playatanu/beelang/releases/tag/v0.1.0
