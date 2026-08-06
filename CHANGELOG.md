# Changelog

All notable changes to **BeeLang** are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.2.0] — 2026-08-06

The sharing-and-diagnostics release. BeeLang gains **Hive**, a package manager,
so code can be published and installed; **stack traces**, so an error says where
it came from instead of just naming a line; and three conveniences the language
was missing — **string interpolation**, **slicing**, and an **interactive REPL**.
A new test suite covers all of it: 100 checks, plus the 15 examples.

### Language

- **String interpolation.** An `f` prefix substitutes `{expressions}` into a
  string — `f"{name} has {n * 2} items"` — with any expression allowed inside the
  braces, including strings of its own (`f"{d["key"]}"`). Write `{{` and `}}` for
  literal braces; a string without the prefix is untouched, so nothing that used
  to work changes meaning.

- **Slicing.** `xs[1:3]`, `xs[:2]`, `xs[2:]`, `xs[-2:]` and `xs[:]`, on lists and
  strings. Bounds may be negative (counting from the end) and are clamped rather
  than checked, so `xs[0:100]` is the whole list and an inverted range is empty.
  Slicing a list returns a copy.

- **An interactive REPL.** `bee` with no arguments starts a session: a bare
  expression prints its value, definitions persist across lines, an open block
  keeps prompting with `...` until it closes, and an error doesn't end the
  session. Two more entry points come with it — `bee -e '<code>'` for a one-liner
  and `bee < script.bee` (or a pipe) to read a program from stdin.

### Diagnostics

- **Stack traces on every error.** An uncaught error now prints the file, line
  and function for each call on the way in, innermost first:

  ```
  Runtime error: division by zero
    at safe_div()  lib/math.bee:8
    at total()     report.bee:14
    at <main>      report.bee:31
  ```

  Methods appear as `Class.method()`. Built-in failures are reported at their
  call site, an error raised inside a callback (`map`, `sort`, `spawn`) keeps its
  own deeper trace, and an uncaught `throw` carries the trace from where it was
  thrown. Lex and parse errors name their file too — including the file of a
  module that failed to parse, and the import that pulled it in. Very deep traces
  are truncated in the middle.

- **Runaway recursion is an error, not a crash.** Recursion deeper than the call
  limit stops with `call stack overflow` plus a trace, where it previously
  overflowed the C++ stack and died on a signal with no message at all. The limit
  scales with the process's stack limit, so `ulimit -s 65536` genuinely buys
  deeper recursion, and `BEE_MAX_DEPTH` overrides it outright. A numeric function
  compiled by the JIT recurses natively and so escapes that check; a `SIGSEGV`
  handler catches the overflow and explains it instead of leaving a bare
  "Segmentation fault" (set `BEE_NO_CRASH_HANDLER=1` to get the core dump back).

### Packages — Hive

- **`hive`, a package manager**, shipped alongside `bee`:

  ```bash
  hive install greet              # from a registry
  hive install ./greet-1.2.0.hive # from a local archive
  hive install                    # everything in hive.json
  ```

  It resolves dependencies transitively, verifies every download against its
  SHA-256, and writes a `hive.lock` that makes a repeat install reproducible —
  and, because the lock carries URLs and hashes, servable entirely from the cache
  with `--offline`. Other commands: `uninstall`, `list`, `info`, `search`,
  `init`, `pack`, `cache`.

  A registry is just static files (`packages/<name>.json`, `index.json`, and the
  archives), so it can live on GitHub Pages, S3, a plain web server, or a
  directory on disk. Full documentation in [docs/HIVE.md](docs/HIVE.md).

- **The `.hive` package format.** A dependency-free container: a `HIVE1` magic
  line, a JSON header holding the manifest and a per-file SHA-256, then the file
  bytes. No zip or tar library on either side, and a corrupt, truncated or
  tampered archive fails loudly instead of installing.

- **`hive.json` manifests.** `name`, `version`, `main`, `dependencies` (with `^`,
  `~`, `>=`, `<`, `=` and comma-separated constraints), `files`/`exclude` for
  packing, plus the usual descriptive fields. Unknown keys are preserved when
  Hive rewrites the file.

- **A worked example** in [`examples/hive-demo/`](examples/hive-demo/): pack a
  package, install it, import it — no registry and no network needed.

### Tooling & tests

- **`make test`** runs two suites: `tests/lang_test.sh` (56 checks — traces,
  locations, interpolation, slicing, the REPL, recursion limits, exit codes) and
  `tests/hive_test.sh` (44 checks — the package manager end to end, against a
  registry served straight off the filesystem, so the tests never touch the
  network).

- The `.deb` and the Windows installer now install `hive` next to `bee`, and
  `make` builds both binaries.

### Changed

- **Module resolution finds installed packages.** `import name` searches the
  importing file's directory, its sibling `lib/`, `hive_modules/` in that
  directory and every directory above it, `$BEE_PATH`, and finally the global
  library at `$HIVE_HOME/lib` (default `~/.hive/lib`). Local code still wins over
  an installed package of the same name, and a directory found this way is
  imported as a package via its manifest's `"main"` (default `init.bee`).

- **Caught errors read better.** A runtime error caught by `try` / `catch` binds
  as a single line that now includes its location — `Runtime error: division by
  zero (lib/math.bee:8)` rather than `Runtime error (line 8): division by zero` —
  and never the stack trace, so printing it inside a message of your own stays
  readable.

- **`bee` with no arguments** starts the REPL (on a terminal) or reads a program
  from stdin, rather than printing usage and exiting.

- A failed `import` of a bare name now suggests `hive install <name>`.

### Fixed

- Reading a module no longer treats a *directory* whose name matches the module
  as an empty source file — the old resolver accepted anything `ifstream` could
  open.
- Variable references carried no line number, so `undefined variable 'x'` was
  reported without a location. They do now.

### Known limitations

- No public registry is running yet, so `hive install <name>` needs a `--registry`
  (or `HIVE_REGISTRY`, or `~/.hive/config.json`) pointing at one. Installing from
  a local `.hive` file works with no registry at all.
- `hive publish` doesn't exist: `hive pack` prints the SHA-256 to paste into your
  registry metadata.
- The REPL has no line editing or history — arrow keys and Ctrl-C aren't handled.
- The VS Code extension doesn't highlight `f"..."` strings or slices yet.

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

[0.2.0]: https://github.com/playatanu/beelang/releases/tag/v0.2.0
[0.1.1]: https://github.com/playatanu/beelang/releases/tag/v0.1.1
[0.1.0]: https://github.com/playatanu/beelang/releases/tag/v0.1.0
