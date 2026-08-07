# Changelog

All notable changes to **BeeLang** are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

Nothing yet.

## [0.3.1] — 2026-08-07

Packaging. Installing a package that contains a native module no longer means
compiling one by hand, packages are a binary format rather than a text file, and
a bug that made 0.3.0's `.deb` unable to build *any* native module is fixed.

### Fixed

- **0.3.0's `.deb` shipped an unusable header set.** `packaging/build-deb.sh`
  installed `src/*.hpp`, but `bee_buffer.h` is a plain C header — and
  `bee_native.hpp` includes it. So every native module compiled against an
  installed BeeLang failed with `fatal error: bee_buffer.h: No such file or
  directory`. Both extensions are installed now, and the packaging script fails
  rather than shipping a header set that can't compile a module.

### Added

- **Prebuilt native modules.** A package lists binaries per platform, and
  `hive install` uses the matching one instead of compiling:

  ```json
  "binaries": {
    "linux-x86_64":   "prebuilt/linux-x86_64/net_native.so",
    "windows-x86_64": "prebuilt/windows-x86_64/net_native.dll"
  }
  ```

  ```
  $ hive install net
    using prebuilt net for linux-x86_64
  ```

  Installing should be a download, not a build. Compiling is now only what
  happens on a platform a package doesn't ship for.

- **A package can declare how to build itself.** `hive.json` gains a `"build"`
  command, run once in the package directory after install — the fallback when
  no prebuilt matches:

  ```json
  { "name": "net", "main": "init.bee", "build": "bash build.sh" }
  ```

  The command is printed before it runs, since it came from a downloaded
  package; `hive install --no-build` skips it. A failed build shows its output
  and exits non-zero but leaves the files in place, because the cause is usually
  a missing compiler and re-running it by hand is then the whole fix.

  Note that a `.pkg` does not carry the executable bit, so a build command needs
  to name its interpreter: `bash build.sh`, not `./build.sh`.

### Changed

- **Packages are now `.pkg`, and no longer plain text.** `hive pack` writes
  `<name>-<version>.pkg` instead of `.hive`, in a new `BEEPKG1` container: the
  payload is compressed with a small built-in LZSS and then XORed with a
  keystream. That roughly halves a package and makes it a binary blob rather
  than a text file with the sources sitting in it.

  The obfuscation is worth naming for what it is: **it is not encryption.** The
  key is a constant in `src/hive/archive.cpp`, and anything that can install a
  package can extract one. It stops a package being browsed or hand-edited in a
  text editor; it keeps nothing secret. Integrity is unchanged and is the part
  that carries weight — per-file SHA-256, rejected paths, and a loud failure on
  truncated, padded or tampered bytes. An old `HIVE1` archive is reported as
  such, with the suggestion to repack it.

  This is a breaking format change. No registry is running yet, so nothing
  published needs migrating; repack any local archives with `hive pack`.

## [0.3.0] — 2026-08-07

The interoperability release. BeeLang can now call C and C++ libraries without
rebuilding the interpreter: **native modules** import like any other module,
**`beegen`** generates the binding for you from a header, and **buffers** give
bulk data somewhere to live that isn't a million boxed `Value`s — and cross the
native boundary without a copy. OpenCV 4.6 was bound and driven end to end to
prove the set is sufficient for a real library, not just a toy one. 180 checks,
up from 100.

### Added

- **Native modules.** A shared library can now be imported like any other
  module, so calling a C or C++ library no longer means rebuilding the
  interpreter:

  ```cpp
  extern "C" const char* bee_native_abi() { return BEE_NATIVE_ABI; }
  extern "C" int bee_module_init(bee::NativeModule* m) {
      m->def("add", 2, [](bee::Interpreter&, std::vector<bee::Value>& a) {
          return bee::Value(bee::native::num(a[0], "add", 0) +
                            bee::native::num(a[1], "add", 1));
      });
      return 0;
  }
  ```

  `import demo` finds `demo.so` (`.dll`, `.dylib`) through the same lookup as a
  `.bee` file, including inside an installed package — so a hive package can ship
  a compiled module. [`src/bee_native.hpp`](src/bee_native.hpp) is the API, with
  conversion helpers that report a bad argument as a normal BeeLang error, with a
  stack trace.

- **`beegen`, a binding generator.** It reads C++ headers with libclang and
  writes a native module plus an idiomatic BeeLang wrapper:

  ```bash
  beegen shapes.hpp --module shapes && ./build.sh
  ```

  Free functions, classes (constructors, methods, public fields, statics) and
  enums are mapped; C++ classes become BeeLang classes holding an opaque handle,
  enums become dicts. Every declaration it can't map is **reported with a
  reason** — templates, variadics, out-parameters, unbound types — because a
  binding that silently omits half a library is worse than one that says so.
  It also writes `hive.json`, so a binding installs like any other package. Full
  documentation in [docs/BINDINGS.md](docs/BINDINGS.md).

  libclang is loaded at run time through a hand-declared slice of its stable C
  ABI, so building BeeLang needs no clang headers or libraries at all.

- **Buffers: a contiguous typed array.** BeeLang's answer to an ndarray, and the
  type bulk data travels in:

  ```
  let img = zeros([480, 640, 3], "u8")   # 900 KB contiguous, not 15 MB of Values
  print(img)                             # buffer<u8>[480,640,3] [0, 0, ...]
  ```

  `f32`/`f64`/`i8`/`u8`/`i16`/`u16`/`i32`/`i64`, flat `[]` indexing plus `at` /
  `set_at` per dimension, `zeros`/`ones`/`full`/`buffer_from`/`to_list`,
  `shape`/`dtype`/`byte_len`, `reshape`/`astype`/`copy`/`fill`, and
  `buf_add`/`sub`/`mul`/`div`/`sum`/`min`/`max`. Buffers compare by contents and
  print with a preview rather than a million elements.

- **Zero-copy buffers across the native boundary.** A shim declares a parameter
  as `BeeBuffer` — a plain C struct in the new [`bee_buffer.h`](src/bee_buffer.h)
  that needs no BeeLang header — and `beegen` hands the buffer's own memory over
  by pointer. This is what makes binding an image or tensor library practical
  rather than merely possible.

- **Native code can call back into BeeLang.** `bee::native::callback()` wraps a
  BeeLang function so a library's log or progress hook can invoke it, with
  `GilLock` for callbacks arriving on the library's own threads and `GilOff` for
  handing the lock back during a long call. The interpreter is now linked with
  `-rdynamic` so a module can resolve `Interpreter::callValue`.

- **Class hierarchies and factory-made interfaces.** A derived handle is accepted
  where a base is expected, through a registered `static_cast` (so the pointer is
  adjusted correctly even under multiple inheritance). Abstract classes get no
  constructor and no `free()` — their instances come from a factory function and
  are released by the API's own `destroy()`. This is the shape TensorRT and ONNX
  Runtime expose, and it is now bindable directly.

- **`std::vector<T>` maps to a list** in both directions, for numeric, bool and
  string elements.

- **Wrappers adopt factory handles.** `new Image(vision.imread("cat.png"))` works
  as well as `new Image()`: a generated wrapper either constructs a new object or
  takes over a handle a factory function returned. Library APIs of any size are
  full of factories, so a wrapper that could only construct could not be used
  with them at all.

- **Verified against a real library.** OpenCV 4.6 was bound through a 40-line
  shim and driven from BeeLang end to end -- pixels built in a buffer, handed to
  OpenCV with no copy, resized, converted, blurred, Canny-detected, written as a
  PNG, read back and pulled into a buffer again.

- **C++ default arguments** become optional BeeLang arguments: `beegen` emits one
  native entry point per callable arity and the wrapper dispatches on how many
  arguments it was given, so the C++ compiler supplies the defaults and beegen
  never has to parse them.

- `tests/beegen_test.sh` — 53 checks covering generation, the skip report, the
  wrapper's shape, compiling the generated module, calling it from BeeLang, the
  boundary errors (wrong type, wrong arity, wrong handle, use-after-free), and
  the capability set above against a header shaped like a real inference API.

### Changed

- The `.deb` and Windows installer also install `beegen`, and the `.deb` now
  ships BeeLang's headers under `/usr/include/bee` so native modules can be
  compiled against an installed interpreter.

- **The VS Code extension moved to its own repository**,
  [beelang-project/vscode-bee](https://github.com/beelang-project/vscode-bee),
  with its history intact. It was `editors/vscode-bee/`, which tied its releases
  to the interpreter's — the `vscode-beelang-v0.1.0` tag matched this repo's
  `v*` release trigger and tried to build a `.deb` out of it. Editor tooling and
  the language now version and ship independently. The release trigger here is
  narrowed to `v[0-9]*` so only interpreter versions fire it.

### Fixed

- **A C++ parameter named like a BeeLang keyword** (`in`, `class`, `from` are all
  ordinary C++ names) generated code that would not parse. Such names now get a
  trailing underscore.
- **A C++ library's own exception no longer aborts the process.** `cv::Exception`,
  `Ort::Exception`, `std::bad_alloc` and friends thrown inside a native call are
  converted to a BeeLang runtime error with a stack trace. Bee-level `throw` and
  control flow still pass through built-ins that call back into Bee code.
- `hive install` now refuses to install a package into its own source tree
  (which produced `greet/hive_modules/greet`, helping nothing) and never records
  a package as depending on itself. `--force` overrides the first if you really
  mean it.

### Known limitations

- **`beegen` needs libclang at run time** — not to build BeeLang, but to read a
  header. Without it (`apt install libclang-18-dev`, or `--libclang <path>`) it
  says so and stops. Everything else, including running generated bindings,
  works without it.
- **Not every declaration can be bound.** Templates, variadics, out-parameters
  and types beegen can't map are skipped — reported individually, with a reason,
  rather than silently dropped. Hand-write a shim for those; the OpenCV binding
  needed 40 lines of one.
- Buffers are dense and contiguous only: no strides, no views, no broadcasting.
  `reshape` and slicing a buffer copy.
- The Windows installer still ships an interpreter-only build (no JIT), and now
  no libclang either, so `beegen` on Windows needs one installed separately.
- Carried from 0.2.0: no public registry is running, so `hive install <name>`
  needs a `--registry`; `hive publish` doesn't exist; the REPL has no line
  editing or history.

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

- **VS Code extension** ([beelang-project/vscode-bee](https://github.com/beelang-project/vscode-bee),
  then `editors/vscode-bee/` in this repo) with
  syntax highlighting, completions (keywords, built-ins, type methods, and file
  symbols), hovers, snippets, and a bee icon for `.be` / `.bee` files.

### Documentation & examples

- Full [language reference](docs/LANGUAGE.md).
- A topic-by-topic set of 14 runnable [examples](examples/).

### Known limitations

- The Windows installer ships an interpreter-only build (no JIT yet).
- The `.deb` requires `libllvm18`, available on Ubuntu 24.04 and newer.
- No anonymous/lambda functions — pass named functions (e.g. to `spawn`).

[0.3.1]: https://github.com/beelang-project/bee/releases/tag/v0.3.1
[0.3.0]: https://github.com/beelang-project/bee/releases/tag/v0.3.0
[0.2.0]: https://github.com/beelang-project/bee/releases/tag/v0.2.0
[0.1.1]: https://github.com/beelang-project/bee/releases/tag/v0.1.1
[0.1.0]: https://github.com/beelang-project/bee/releases/tag/v0.1.0
