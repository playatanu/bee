# Changelog

All notable changes to **Bee** are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.3.3] — 2026-08-10

### Fixed

- **Windows release build.** The JIT backend DLL failed to link on MSYS2 with
  `export ordinal too large`: MinGW auto-exports every global symbol, and with
  LLVM folded in statically that far exceeds the 65535-entry PE export table.
  The DLL now links with `--exclude-all-symbols` and exports only its one entry
  point (`bee_jit_create`, via `__declspec(dllexport)`), which is all `bee.exe`
  resolves. This is what kept the 0.3.2 release job red.

## [0.3.2] — 2026-08-10

### Language

- **Optional type annotations.** Parameters, variables and return types can be
  declared, and are checked where the value enters:

  ```
  fn dot(a: buffer, b: buffer, n: num) -> num { ... }
  let total: num = 0
  ```

  The type names are `num`, `str`, `bool`, `list`, `dict`, `buffer`, `nil`,
  `fn`, `any`, and any class name; a class annotation accepts instances of that
  class and of anything deriving from it. A violated annotation is a runtime
  error naming the parameter, the declared type and what arrived, reported at
  the call site with the usual stack trace. Return types are checked on every
  path out, including falling off the end — a `-> num` function that returns
  nothing yields `nil`, which is an error.

  Types are entirely optional and mix freely with untyped code: an unannotated
  parameter is `any`, a typed function can call an untyped one, and a program
  with no annotations behaves exactly as before and runs at exactly the same
  speed (the per-call check is skipped by a single flag test on the
  declaration). All four ways into a function — the tree-walker, the bytecode
  VM, a direct VM-to-VM call and a fused method call — enforce them identically,
  which the differential suite checks.

  An annotation binds the name for its whole life, not just its initialiser:
  assigning a string to something declared `num` is an error wherever it
  happens, and a parameter's default value has to satisfy the annotation too.
  That is what makes the declaration something later code can rely on.

- **Declared types generate better code.** The compiler tracks what each
  register is known to hold and, where both operands are declared numbers, emits
  arithmetic with no type test and no fallback branch. A declared `buffer`
  indexed by a declared `num` compiles to a bounds check and a direct load
  against contiguous unboxed memory, with no call into the interpreter.

  A `dot()` kernel over two buffers went from 0.115 s to **0.089 s** by adding
  annotations. A plain arithmetic loop gained almost nothing (0.715 s → 0.691 s),
  which is the more informative result: the type test was never the cost, and
  the remaining work is to stop building a `Value` for numbers at all — see
  [docs/PERFORMANCE.md](docs/PERFORMANCE.md).

- `BEE_DUMP_BYTECODE=1` prints each function's bytecode as it is compiled.

- **The JIT compiles per argument signature, and buffers reach native code.**
  It previously accepted unboxed numbers and nothing else, so any function
  touching a container was never compiled. Each call now classifies what it is
  actually passing — number, f64 buffer, or something else — and that signature
  keys the compilation. A call whose arguments do not match a compiled signature
  finds none and runs interpreted, so the argument types are themselves the
  guard: there is nothing to invalidate and no state to reconstruct.

  An f64 buffer is passed as a raw base pointer and an element count, so `b[i]`
  compiles to a bounds check and a load against contiguous unboxed memory. A
  `dot()` kernel over two buffers went from 0.089 s to **0.026 s** — 3.3× faster
  than CPython, and the gap to Go closed from 22.3× to **6.5×**.

  This needs no annotations: the signature comes from what the call passes, not
  what the source declares, so an untyped `fn dot(a, b, n)` compiles just as
  well. Buffer *reads* are compiled and *writes* are not, which preserves the
  contract that makes bailing out safe — native code that gives up is re-run by
  the interpreter, and re-running can only be correct if nothing was written.

  Measured in isolation, a compiled `dot()` runs at **1.01 ns per element**
  against 44.79 ns on the bytecode VM. With process startup excluded, that is
  3.67× off Go and 6.7× faster than CPython.

- **The LLVM engine is built on first use, not at startup.** Standing up an ORC
  session cost every run of `bee` a couple of milliseconds for a JIT most
  scripts never trigger.

- `BEE_NO_JIT=1` runs everything on the bytecode VM, the counterpart of
  `BEE_NO_VM=1`. Both exist so a measurement can be attributed to a tier rather
  than guessed at.

- **`bench/run.sh` reports a `work` column** with each runtime's startup floor
  subtracted. Bee starts in ~15 ms against CPython's ~12 ms and a Go binary's
  ~1 ms, so on a benchmark doing 10 ms of work, comparing totals said more about
  process startup than about either language.

### Performance

- **The LLVM JIT is a dlopen'd shared object, not a link-time dependency.** The
  backend used to be linked straight into `bee`, so the dynamic loader mapped
  ~120MB of `libLLVM.so` on *every* invocation — measured here at ~6ms of pure
  startup (an interpreter-only build starts in <1ms), paid whether or not a
  script ever compiled anything. Most scripts never trigger the JIT, so this was
  the single broadest cost a person felt running one.

  The backend now lives in `libbee_jit.so`, which `bee` `dlopen`s the first time
  a function or loop is hot enough to compile; the executable itself links no
  LLVM at all. A plain run maps the library never, and starts at the
  interpreter-only floor. When compilation *is* triggered the library loads
  once, transparently — `fib(34)` still runs ~10× faster than the VM. The shared
  object calls back into the interpreter through the same `-rdynamic` symbol
  resolution native modules use, and this is the same technique `beegen` already
  uses for `libclang`. If the library is missing or fails to load, execution
  simply falls back to the interpreter/VM. Set `BEE_JIT_LIB` to point at a
  specific backend.

  The Windows installer now ships the JIT too — as `bee_jit.dll`, built under
  MSYS2 UCRT64 against an ABI-matched LLVM and `dlopen`'d by `bee.exe` the same
  way. Previously the Windows build was interpreter-only. The DLL is
  self-contained and shares `bee.exe`'s heap through the Universal CRT.

- **`BEE_NO_JIT=1` now also suppresses the loop JIT**, not just the function
  JIT, so it disables the native tier completely — the counterpart of
  `BEE_NO_VM=1`, and what makes a measurement attributable to one tier.

- **A register bytecode VM.** Function bodies are compiled once to a flat
  instruction stream and executed there instead of being walked as a tree. The
  design is register-based rather than stack-based, so `s = s + xs[i]` is two
  instructions where a stack VM needs six, and dispatch is by computed goto, so
  each opcode gets its own indirect branch and its own prediction history.

  Registers *are* the resolver's frame slots. That works because a function
  whose body creates no closure has all of its scopes merged into one frame (see
  scope merging below), so every local sits at depth 0 with nothing to walk. A
  frame is a window into one flat, thread-local register array — entering a
  function is an index, not an allocation.

  Anything the compiler does not cover yet — a nested function or class, `try`,
  `import`, destructuring — makes it decline that *one* function, which then
  runs on the tree-walker exactly as before. Nothing about the language changes,
  and `BEE_NO_VM=1` runs everything on the tree-walker.

  A new suite, `tests/vm_diff_test.sh`, runs 29 programs on both engines and
  requires identical stdout, stderr and exit code — including error messages,
  stack traces and uncaught-error exit codes.

- **Scopes that nothing can capture are merged into the enclosing frame.** A
  block or loop that declares names used to allocate an `Environment` on every
  entry — meaning once per *iteration* for a loop body. When no function or
  class is created inside it, nothing can outlive the scope, so its variables
  now live in slots of the enclosing frame and no environment is allocated at
  all. Where a closure *is* created, the scope is untouched, so each iteration
  still captures its own bindings.

- **Call frames are recycled.** A call allocated an environment (a control block
  and a slots vector) and dropped it on return. Frames now come from a small
  free list, and only the ones a closure captured are left to live on.

- **Compiled functions call each other directly.** A `CALL` whose target is
  another compiled Bee function taking exactly those arguments now enters it
  from the dispatch loop: no argument vector, no trip through `callFunction`,
  and the callee's registers are filled straight from the caller's. Built-ins,
  classes, defaults, rest parameters, uncompiled bodies and the numeric
  functions the LLVM JIT claimed still go through the interpreter, so behaviour
  — traces, arity errors, the depth limit — is unchanged. A call-heavy loop
  dropped from 0.73 s to 0.48 s.

- **Instance fields are shapes, not a map.** An instance's fields lived in a
  `std::map<std::string, Value>`, so `p.x` was a tree walk with string
  comparisons — measured at ~41 ns, which was most of the cost of
  object-oriented code. A `Shape` now records which field names an instance has
  and which slot each one occupies; instances that gained their fields in the
  same order (for a class with a normal `init`, all of them) share a single
  shape, so the fields themselves are a flat vector. Each property site caches
  "this shape means slot N" and validates it with a pointer compare.

  Field access is **4.0× faster** and now beats CPython. One class reaching two
  different shapes, fields added after construction, and a field shadowing a
  method of the same name all still behave as before.

- **`obj.m(args)` is one instruction.** It used to be a property read followed by
  a call, and reading `obj.m` on its own has to produce a callable — which for a
  method meant allocating a copy of it bound to the receiver, on every call.
  The receiver is now passed directly, with the method resolved once per class
  at each call site. Instance method dispatch is **28.7× faster** than 0.3.1.

- **A benchmark suite**, in [`bench/`](bench/): 17 programs covering loops,
  calls, methods, classes, dicts, strings, closures, sorting and error handling,
  with equivalents in Python and Go. `make bench` times the current build;
  `bench/run.sh --vs <other-bee> --all` compares against another build, CPython
  and Go at once. Contenders run interleaved and keep their best time, because
  on a laptop the CPU's boost state drifts enough during a suite to swing
  back-to-back results by 2×.

- Cheaper calls and indexing: a call frame stores the function pointer and
  formats its name only when a stack trace is printed; `callFunction` takes its
  function by reference rather than by `shared_ptr` copy; list and dict indexing
  borrow the container instead of copying a `shared_ptr`; and the VM indexes a
  list by number without leaving the dispatch loop.

  Measured against 0.3.1, best of five interleaved runs, 9 M inner iterations:

  | | 0.3.1 | now |
  |---|---|---|
  | nested loop over a list, `s = s + xs[i]` | 1.13 s | **0.21 s** (5.4×) |
  | the same with `if (xs[i] % 2 == 0)` | 1.47 s | **0.38 s** (3.9×) |
  | the same written with `continue` | 20.57 s | **0.38 s** (54×) |
  | `xs[i] = xs[i] + 1` | 0.45 s | **0.09 s** (5.0×) |
  | a `let` in the loop body | 1.08 s | **0.17 s** (6.4×) |
  | loop calling an interpreted function | 4.98 s | **0.44 s** (11.3×) |

  Against CPython 3.12 on the same three list benchmarks, Bee went from
  3.9×, 3.7× and **52× slower** to **0.80×, 1.07× and 1.05×** — at or slightly
  ahead of CPython, where the whole interpreted path used to sit well behind it.
  Geometric mean across the whole suite: **3.94× faster than 0.3.1**, with
  object-oriented code 17–29× faster.

### Changed

- **The language is now called "Bee", not "BeeLang".** Prose across the README,
  docs and examples uses the shorter name; the binary (`bee`), the package
  manager (`hive`), file extensions (`.be`/`.bee`), the GitHub organisation and
  every URL and install path are unchanged, so nothing about an existing setup
  breaks.

### Fixed

- **String building was quadratic inside a compiled function.** `s = s + x` has
  grown its buffer in place since 0.1.1, but the bytecode VM did not carry that
  over, so building a 3 MB string took **363 s** instead of 0.06 s. Every
  correctness test passed throughout — the answer was right, only the
  complexity was wrong — so `tests/perf_guard_test.sh` now checks that this and
  four other idioms stay linear.

- **`sort(xs, cmp)[0]` could read freed memory.** Indexing straight off a call
  result compiles to an instruction whose destination register is also its
  object register; writing the element back into that register dropped the last
  reference to the list and freed it mid-read. It produced a denormal double for
  small inputs and a crash for large ones. Both this and the aliasing shape
  behind it are covered by the differential suite now.

- **`break`, `continue` and `return` no longer throw C++ exceptions.** They were
  `throw BreakSignal{}` / `ContinueSignal` / `ReturnSignal`, caught by every loop
  and by every call — which cost **2.4–3.8 µs each**, 50–100× an entire
  interpreted loop iteration, on paths that are not exceptional at all: every
  function call returns, and a `continue` runs on most iterations of the loop
  that uses it. A statement now reports how it finished (`Flow::Normal`,
  `Break`, `Continue`, `Return`) and the enclosing loop or call frame absorbs it.

  Measured against the previous release, best of three runs:

  | | before | after |
  |---|---|---|
  | nested loop with `continue` in the body (9 M iterations) | 20.33 s | **1.39 s** |
  | loop calling an interpreted function (1 M calls) | 2.45 s | **0.23 s** |
  | `continue` microbenchmark (1 M) | 3.85 s | **0.05 s** |
  | nested loop summing `xs[i]` (9 M) | 2.18 s | **1.04 s** |

  The last row is a side effect worth having: dropping the `try` region from
  around each loop-body execution let the C++ compiler optimise a loop it
  previously could not touch.

  Nothing about the language changes. `finally` still runs on every exit path,
  and a `return` out of a `finally` still wins over one out of the body. A Bee
  `throw`, and runtime errors, remain C++ exceptions — those really are rare.

- **Cheaper calls and indexing.** A call frame stores the function pointer and
  formats its name only when a stack trace is printed, instead of building and
  copying a `std::string` on every call; `callFunction` takes its function by
  reference rather than by `shared_ptr` copy; and list/dict indexing borrows the
  container instead of copying a `shared_ptr`, removing two atomic refcount
  operations per `xs[i]`.

- **[`docs/PERFORMANCE.md`](docs/PERFORMANCE.md)** — a measured account of where
  the interpreter spends its time, with per-operation costs, comparisons against
  CPython, and the remaining plan (stack frames instead of heap environments, a
  bytecode VM, a cheaper value representation, and a type-guarded JIT that
  deoptimises instead of refusing to compile).

## [0.3.1] — 2026-08-07

Packaging. Installing a package that contains a native module no longer means
compiling one by hand, packages are a binary format rather than a text file, and
a bug that made 0.3.0's `.deb` unable to build *any* native module is fixed.

### Fixed

- **0.3.0's `.deb` shipped an unusable header set.** `packaging/build-deb.sh`
  installed `src/*.hpp`, but `bee_buffer.h` is a plain C header — and
  `bee_native.hpp` includes it. So every native module compiled against an
  installed Bee failed with `fatal error: bee_buffer.h: No such file or
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

The interoperability release. Bee can now call C and C++ libraries without
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
  conversion helpers that report a bad argument as a normal Bee error, with a
  stack trace.

- **`beegen`, a binding generator.** It reads C++ headers with libclang and
  writes a native module plus an idiomatic Bee wrapper:

  ```bash
  beegen shapes.hpp --module shapes && ./build.sh
  ```

  Free functions, classes (constructors, methods, public fields, statics) and
  enums are mapped; C++ classes become Bee classes holding an opaque handle,
  enums become dicts. Every declaration it can't map is **reported with a
  reason** — templates, variadics, out-parameters, unbound types — because a
  binding that silently omits half a library is worse than one that says so.
  It also writes `hive.json`, so a binding installs like any other package. Full
  documentation in [docs/BINDINGS.md](docs/BINDINGS.md).

  libclang is loaded at run time through a hand-declared slice of its stable C
  ABI, so building Bee needs no clang headers or libraries at all.

- **Buffers: a contiguous typed array.** Bee's answer to an ndarray, and the
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
  that needs no Bee header — and `beegen` hands the buffer's own memory over
  by pointer. This is what makes binding an image or tensor library practical
  rather than merely possible.

- **Native code can call back into Bee.** `bee::native::callback()` wraps a
  Bee function so a library's log or progress hook can invoke it, with
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
  shim and driven from Bee end to end -- pixels built in a buffer, handed to
  OpenCV with no copy, resized, converted, blurred, Canny-detected, written as a
  PNG, read back and pulled into a buffer again.

- **C++ default arguments** become optional Bee arguments: `beegen` emits one
  native entry point per callable arity and the wrapper dispatches on how many
  arguments it was given, so the C++ compiler supplies the defaults and beegen
  never has to parse them.

- `tests/beegen_test.sh` — 53 checks covering generation, the skip report, the
  wrapper's shape, compiling the generated module, calling it from Bee, the
  boundary errors (wrong type, wrong arity, wrong handle, use-after-free), and
  the capability set above against a header shaped like a real inference API.

### Changed

- The `.deb` and Windows installer also install `beegen`, and the `.deb` now
  ships Bee's headers under `/usr/include/bee` so native modules can be
  compiled against an installed interpreter.

- **The VS Code extension moved to its own repository**,
  [beelang-project/vscode-bee](https://github.com/beelang-project/vscode-bee),
  with its history intact. It was `editors/vscode-bee/`, which tied its releases
  to the interpreter's — the `vscode-beelang-v0.1.0` tag matched this repo's
  `v*` release trigger and tried to build a `.deb` out of it. Editor tooling and
  the language now version and ship independently. The release trigger here is
  narrowed to `v[0-9]*` so only interpreter versions fire it.

### Fixed

- **A C++ parameter named like a Bee keyword** (`in`, `class`, `from` are all
  ordinary C++ names) generated code that would not parse. Such names now get a
  trailing underscore.
- **A C++ library's own exception no longer aborts the process.** `cv::Exception`,
  `Ort::Exception`, `std::bad_alloc` and friends thrown inside a native call are
  converted to a Bee runtime error with a stack trace. Bee-level `throw` and
  control flow still pass through built-ins that call back into Bee code.
- `hive install` now refuses to install a package into its own source tree
  (which produced `greet/hive_modules/greet`, helping nothing) and never records
  a package as depending on itself. `--force` overrides the first if you really
  mean it.

### Known limitations

- **`beegen` needs libclang at run time** — not to build Bee, but to read a
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

The sharing-and-diagnostics release. Bee gains **Hive**, a package manager,
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

The first public release of Bee — a small, friendly scripting language with
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

[0.3.3]: https://github.com/beelang-project/bee/releases/tag/v0.3.3
[0.3.2]: https://github.com/beelang-project/bee/releases/tag/v0.3.2
[0.3.1]: https://github.com/beelang-project/bee/releases/tag/v0.3.1
[0.3.0]: https://github.com/beelang-project/bee/releases/tag/v0.3.0
[0.2.0]: https://github.com/beelang-project/bee/releases/tag/v0.2.0
[0.1.1]: https://github.com/beelang-project/bee/releases/tag/v0.1.1
[0.1.0]: https://github.com/beelang-project/bee/releases/tag/v0.1.0
