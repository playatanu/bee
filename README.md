<div align="center">

<img src="docs/assets/bee.png" width="120" alt="Bee logo" />

# Bee

**Bee** - a friendly programming language with a built-in native (LLVM) JIT.

[![License: MIT](https://img.shields.io/badge/License-MIT-4c9a2a.svg?style=flat-square)](LICENSE)
[![Language: C++17](https://img.shields.io/badge/C%2B%2B-17-00599c.svg?style=flat-square)](Makefile)
[![Platforms](https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey.svg?style=flat-square)](#-installation)
[![Version](https://img.shields.io/badge/version-0.3.4-f5b51e.svg?style=flat-square)](#)

[Install](#-installation) · [Quick start](#-quick-start) · [Language tour](#-language-tour) · [Packages](#-packages) · [Bindings](#-c-bindings) · [Docs](docs/LANGUAGE.md) · [Examples](examples/)

</div>

```
fn greet(name) {
    return "Hello, " + name + "!"
}

print(greet("world"))     # Hello, world!
```

Bee is a dynamically-typed language designed to be **easy to read and quick to pick up** — familiar C-style syntax, first-class functions and closures, classes, a real module system, threads, and a batteries-included standard library. Bee runs on a compact C++17 runtime that transparently compiles hot numeric functions to native code with a built-in LLVM JIT, so tight loops run at near-native speed.

---

## ✨ Features

- **Clean, familiar syntax** — `let`, `fn`, `if`/`while`/`for`, `class`, and both `#` and `//` comments.
- **String interpolation & slicing** — `f"{name} has {n}"`, `xs[1:3]`, `s[-5:]`.
- **An interactive REPL** — run `bee` with no arguments, or `bee -e '<code>'` for a one-liner.
- **First-class functions & closures** — pass functions around, capture state, build counters and callbacks.
- **Classes & single inheritance** — constructors, methods, `this`, `super`, and custom `str()` for printing.
- **Module system** — `import`, `from … import`, aliases, and a sibling `lib/` search path.
- **A package manager** — `hive install <package>` fetches a package into `hive_modules/`, where `import` finds it. See the [Hive guide](docs/HIVE.md).
- **Native modules & automatic bindings** — call C/C++ from Bee, and generate the glue from a header with `beegen`. See the [bindings guide](docs/BINDINGS.md).
- **Error handling** — `try` / `catch` / `finally` with `throw` of *any* value.
- **Real stack traces** — every error names the file, line and function it came from, all the way back to `<main>`.
- **Threads with a GIL** — safe shared state and real overlap for I/O-bound work.
- **Rich standard library** — strings, lists, dicts, math, files, time, random, env, and process execution — no imports needed.
- **Native LLVM JIT** — numeric-heavy functions are compiled to native code and run at near-native speed, transparently.
- **Ahead-of-time compilation** — `beec` turns a program into a standalone native executable that runs without the interpreter. See the [compiling guide](docs/COMPILING.md).
- **First-class editor support** — a VS Code extension with highlighting, completions, hovers, and snippets.

## 📦 Installation

### Download & run — no compiler needed

| Platform | Package | How |
|----------|---------|-----|
| **Windows** | `bee-0.3.4-amd64.exe` | Run the installer. It adds `bee` and `hive` to your `PATH` and gives `.be` / `.bee` files a bee icon so you can double-click to run them. |
| **Debian / Ubuntu** | `bee-0.3.4-amd64.deb` | Double-click (opens your software centre) or `sudo apt install ./bee-0.3.4-amd64.deb`. Installs `bee` and `hive`. |

> Grab the latest packages from the [**Releases**](https://github.com/playatanu/bee/releases) page.

### Build from source

Requires a C++17 compiler, `make`, and LLVM (17/18) for the JIT:

```bash
sudo apt install llvm-18-dev   # the JIT dependency (Debian/Ubuntu)
make                           # builds ./bee (with the native JIT), ./hive and ./beegen
make test                      # end-to-end tests for hive and module resolution
sudo cp bee hive beegen /usr/local/bin  # (optional) put them on your PATH
```

The packaging scripts in [`packaging/`](packaging/) reproduce the release
artifacts: [`packaging/build-deb.sh`](packaging/build-deb.sh) builds the `.deb`,
and [`packaging/windows/`](packaging/windows/) holds the Inno Setup script and a
guide for building the Windows installer.

## 🚀 Quick start

Save this as `hello.bee`:

```
let names = ["Ada", "Alan", "Grace"]

for name in names {
    print("Hello, " + name + "!")
}
```

Run it:

```bash
bee hello.bee
```

```
Hello, Ada!
Hello, Alan!
Hello, Grace!
```

A source file uses the `.be` or `.bee` extension. Statements may end with an
optional semicolon; newlines are not significant.

```bash
bee --version              # bee 0.3.4
bee --help                 # usage and options
bee                        # an interactive REPL
bee -e 'print(1 + 1)'      # run one line
```

## 📚 Packages

`hive`, Bee's package manager, ships with `bee`:

```bash
hive init                     # start a project (writes hive.json)
hive install greet            # fetch a package into ./hive_modules
hive install greet@^1.2.0     # ... a particular version
hive install ./greet-1.2.0.pkg    # ... or a local .pkg package
hive list                     # what's installed
```

Then import it by name:

```
import greet

print(greet.hello("Bee"))
```

Dependencies resolve transitively, every download is checked against its
SHA-256, and `hive.lock` pins exact versions so a repeat install — or an
`--offline` one — gets the same bytes. Publishing is `hive pack` plus a static
file on any web server.

**[Full guide → docs/HIVE.md](docs/HIVE.md)**

## 🔗 C++ bindings

`beegen` reads a C++ header and writes the bindings for you — a native module
plus a Bee wrapper:

```bash
beegen shapes.hpp --module shapes   # parses the header with libclang
./build.sh                          # compiles shapes_native.so
```

```
import shapes
from shapes import Rect

print(shapes.greet("Bee"))
let r = new Rect(3, 4)
print(r.area())                     # calls the real C++
r.free()
```

C++ classes become Bee classes, enums become dicts, and anything that can't
be mapped is *reported* rather than silently dropped. Native modules load with
`import`, so binding a library never means rebuilding the interpreter.

**[Full guide → docs/BINDINGS.md](docs/BINDINGS.md)**

## 🐝 Language tour

<table>
<tr><td>

**Closures**

```
fn counter() {
    let n = 0
    fn tick() { n += 1; return n }
    return tick
}
let c = counter()
print(c(), c(), c())   # 1 2 3
```

</td><td>

**Classes & inheritance**

```
class Animal {
    init(name) { this.name = name }
    speak() { return this.name + " makes a sound" }
}
class Dog extends Animal {
    speak() { return super.speak() + ": woof!" }
}
print(Dog("Rex").speak())
```

</td></tr>
<tr><td>

**Modules**

```
from mathutil import square, PI
print(square(5))   # 25
print(PI)
```

</td><td>

**Error handling**

```
try {
    throw {"kind": "range", "msg": "too big"}
} catch (e) {
    print(e.kind, "-", e.msg)
}
```

</td></tr>
</table>

**Threads** — overlap blocking work; the GIL keeps shared state safe:

```
fn download(url) { return exec("curl -s " + url).output }

let a = spawn(download, "http://example.com/a")
let b = spawn(download, "http://example.com/b")
print(join(a), join(b))   # both downloads ran concurrently
```

See the full [**Language Reference**](docs/LANGUAGE.md) for everything: values,
operators, control flow, the standard library, type methods, and more.

## ⚡ Performance

Bee ships with a built-in **LLVM ORCv2 JIT**. Functions that stay within a
numeric subset are compiled to native code operating on unboxed doubles; the
rest of your program runs on the runtime, with identical results. The JIT is
enabled by default — just make sure LLVM (17/18) is installed before you build:

```bash
sudo apt install llvm-18-dev   # Debian/Ubuntu — the JIT dependency
make                           # "Building with LLVM JIT via ..."
bee examples/13_benchmark.bee
```

| Benchmark | Without JIT | With JIT |
|-----------|------------:|---------:|
| `fib(32)` | ~11 s | ~0.014 s |

> If LLVM isn't found at build time, `make` falls back to a JIT-less binary that
> behaves identically, just slower — handy for minimal environments.

## 🧱 Compile to a native binary

`beec`, Bee's ahead-of-time compiler, turns a program into a standalone native
executable — a real binary you can ship and run with no interpreter installed:

```bash
beec hello.bee -o hello   # generates C++, links the runtime, emits a native binary
./hello                   # runs on its own
```

It compiles the whole language — variables, operators, strings, lists, dicts,
control flow, `match`, functions, closures, classes and inheritance,
destructuring, list comprehensions, error handling, threads, `import` (resolved
like the interpreter — sibling files, `lib/`, and `hive_modules/` packages —
and compiled into the same binary, dependencies and all), and the whole standard
library. The one thing it can't yet handle — importing a native C/C++ `.so`
extension — is *reported* with a file and line rather than miscompiled. The build
machine needs a C++17 compiler; the output needs nothing.

**[Full guide → docs/COMPILING.md](docs/COMPILING.md)**

## 🧩 Editor support

A [VS Code extension](https://github.com/beelang-project/vscode-bee) provides
syntax highlighting, completions (keywords, built-ins, `.`-methods, and file
symbols), hovers, and snippets — plus a bee icon for `.be` / `.bee` files. It
lives in its own repository and is versioned independently; see its
[README](https://github.com/beelang-project/vscode-bee#readme) to install.

## 🗂️ Project layout

```
Bee/
├── src/               # the C++17 interpreter (lexer, parser, resolver, VM, JIT)
├── examples/          # sample .bee programs
├── lib/               # standard-library modules search path
├── packaging/         # .deb builder + Windows (Inno Setup) installer
└── docs/LANGUAGE.md   # full language reference
```

## 🤝 Contributing

Issues and pull requests are welcome. To hack on the interpreter:

```bash
make            # build
make run FILE=examples/01_hello.bee
make clean
```

Bee is plain C++17. The built-in native JIT uses LLVM (17/18) — install it
to build the default JIT-enabled `bee`; without it the build still works, just
without native compilation. Sources live in [`src/`](src/).

## 📄 License

Released under the [MIT License](LICENSE). © 2026 Atanu Debnath.
