# Native modules and automatic bindings

Bee can call C and C++ directly. There are two pieces:

- **Native modules** — a shared library that `import` loads, so calling a C++
  library doesn't mean rebuilding the interpreter.
- **`beegen`** — a generator that reads C++ headers with libclang and writes the
  native module *and* an idiomatic Bee wrapper for you.

```bash
beegen sqlite3.h --module sqlite     # read the header, write the bindings
cd . && ./build.sh                   # compile the module
bee -e 'import sqlite; print(sqlite.libversion())'
```

---

## Contents

- [Generating bindings](#generating-bindings)
- [What the output looks like](#what-the-output-looks-like)
- [How types map](#how-types-map)
- [Buffers: bulk data without a copy](#buffers-bulk-data-without-a-copy)
- [Calling back into Bee](#calling-back-into-beelang)
- [Class hierarchies and factories](#class-hierarchies-and-factories)
- [Errors from a C++ library](#errors-from-a-c-library)
- [What gets skipped, and why](#what-gets-skipped-and-why)
- [Options](#options)
- [Shipping a binding as a package](#shipping-a-binding-as-a-package)
- [Writing a native module by hand](#writing-a-native-module-by-hand)
- [Memory, threads and safety](#memory-threads-and-safety)
- [Binding a real library: the recipe](#binding-a-real-library-the-recipe)
- [Requirements and limits](#requirements-and-limits)

---

## Generating bindings

A worked example. Given `shapes.hpp`:

```cpp
#include <string>

enum Color { COLOR_RED = 0, COLOR_GREEN = 1, COLOR_BLUE = 7 };

int add(int a, int b);
std::string greet(const std::string& who);

class Rect {
public:
    Rect(double w, double h);
    double area() const;
    void grow(double by);
    static double unit_area();
    double width;
    double height;
};
```

Generate, build, use:

```bash
$ beegen shapes.hpp --module shapes
beegen: 2 function(s), 1 class(es) (3 method(s), 2 field(s)), 1 enum(s)
  wrote ./shapes_native.cpp
  wrote ./shapes.bee
  wrote ./build.sh
  wrote ./hive.json

$ ./build.sh          # add your library's own objects/-l flags if it has them
built shapes_native.so
```

```bee
import shapes
from shapes import Rect, Color

print(shapes.add(2, 3))            # 5
print(shapes.greet("Bee"))     # Hello, Bee!
print(Color.COLOR_BLUE)            # 7

let r = new Rect(3, 4)
print(r.area())                    # 12
r.grow(1)
print(r.get_width())               # 4
print(shapes.Rect_unit_area())     # 1
r.free()                           # release the C++ object
```

`build.sh` compiles only the generated file. If the library you're binding has
its own sources or needs linker flags, add them there — it's a normal script:

```bash
g++ -std=c++17 -O2 -fPIC -shared -I"$BEE_SRC" \
    shapes_native.cpp shapes.o -lsqlite3 -o shapes_native.so
```

---

## What the output looks like

Four files, with a deliberate split:

| File | What it is |
|---|---|
| `<module>_native.cpp` | the native module — flat, C-like functions (`Rect_new`, `Rect_area(handle)`) |
| `<module>.bee` | the wrapper you actually import — classes, enums, `free()` |
| `build.sh` | the compile command, with the include path already right |
| `hive.json` | a manifest, so the binding installs like any other package |

The C++ side stays flat on purpose: it keeps the generated code simple enough to
read and the ABI narrow. The Bee side is where a C++ class becomes something
that looks hand-written:

```bee
class Rect {
    init(w, h) {
        this._handle = shapes_native.Rect_new(w, h)
    }
    area() { return shapes_native.Rect_area(this._handle) }
    free() {
        if this._handle != nil {
            shapes_native.Rect_free(this._handle)
            this._handle = nil
        }
    }
}
```

Both files are regenerated wholesale, so don't edit them — change the header or
the flags and re-run. `hive.json` is the exception: it's yours to edit, and
`beegen` won't overwrite one that already exists.

---

## How types map

| C++ | Bee | Notes |
|---|---|---|
| `void` | `nil` | |
| `bool` | bool | |
| `int`, `long`, `size_t`, `char`, … | number | a fraction or an out-of-range value is a runtime error, never a silent truncation |
| `float`, `double` | number | |
| `const char*` | string | a null pointer becomes `nil`, not `""` |
| `std::string`, `std::string_view` | string | by value or by const reference |
| `enum` | number | plus a dict of its constants: `Color.COLOR_BLUE` |
| `T*`, `T&`, `const T&` | opaque handle | when `T` is a class that also got bound |
| `T` returned by value | handle to a heap copy | the caller owns it; call `free()` |
| public field | `get_x()` / `set_x(v)` | read-only when the field is `const` or a handle |
| `static` method | a module-level function, `Class_method()` | Bee classes have no static members |
| `BeeBuffer` | a [buffer](#buffers-bulk-data-without-a-copy) | **no copy** — the library gets a pointer to the buffer's own memory |
| `std::vector<T>` | list | for numeric, bool and string elements, in both directions |
| default arguments | optional arguments | one native entry point per arity; C++ supplies the defaults |
| abstract class | a wrapper around a factory's handle | no constructor and no `free()` is generated |
| derived class | accepted where a base is wanted | via a registered `static_cast`, correct under multiple inheritance |

A handle is a small dict carrying an address and a type name, so passing a
`Rect` where a `Canvas` is expected is caught at the boundary:

```
Runtime error: Canvas_draw: argument 1 must be a Canvas handle, got a Rect handle
```

Overloads get a numeric suffix — `open`, `open_2`, `open_3` — in declaration
order, because Bee dispatches on name alone.

---

## Buffers: bulk data without a copy

This is the part that decides whether a binding to an image or tensor library is
usable at all. A 640×480 RGB image is 921,600 numbers; as a Bee list that is
a `std::vector<Value>` at 16 bytes each — about 15 MB, rebuilt element by element
at every boundary. A **buffer** is raw bytes with a dtype and a shape, so it
crosses as a single pointer:

```bee
let img = buffer([480, 640, 3], "u8")   # 900 KB, contiguous
vision.to_gray(img, out)                # the library writes straight into it
```

On the C++ side, a shim declares its parameters as `BeeBuffer` (from
[`bee_buffer.h`](../src/bee_buffer.h), a plain C struct that needs no Bee
header) and `beegen` maps them automatically:

```cpp
#include "bee_buffer.h"

double tensor_sum(BeeBuffer t);              // reads t.data directly
void   tensor_scale(BeeBuffer t, double f);  // writes in place
```

```c
typedef struct BeeBuffer {
    void*     data;      /* first element                */
    long long bytes;     /* total length                 */
    int       dtype;     /* BEE_DTYPE_F32, _U8, ...      */
    int       ndim;
    long long shape[8];  /* row-major                    */
} BeeBuffer;
```

The pointer is valid **for the duration of the call only**. A library that keeps
the memory — an async inference queue, a GPU upload that outlives the call — must
copy it. And check `dtype` before casting: a `u8` buffer read as `float*` is
exactly the kind of bug that has no Bee-level symptom.

To allocate a buffer from native code, use `bee::native::makeBuffer(DType::F32,
{rows, cols})`. The Bee-side API — `buffer`, `zeros`, `ones`, `full`,
`buffer_from`, `to_list`, `shape`, `dtype`, `at`, `set_at`, `reshape`, `astype`,
`copy`, `buf_add`/`sub`/`mul`/`div`, `buf_sum`/`min`/`max` — is in the
[language reference](LANGUAGE.md#buffers).

---

## Calling back into Bee

Libraries that log, report progress, or ask a question mid-call need to run your
code. Hold the callable and invoke it:

```cpp
m->def("each", 2, [](Interpreter& I, std::vector<Value>& a) -> Value {
    auto cb = native::callback(I, a[1], "each", 1);
    for (int i = 0; i < 10; ++i) cb({Value((double)i)});
    return Value();
});
```

```bee
lib.each(data, fn(i) { print(f"row {i}") })
```

Two rules. **The GIL must be held** — it already is when the call came from Bee
code, but a callback arriving on a thread the library created must take it first
with `bee::native::GilLock lock(interp);`. And **a raw C function pointer cannot
be a closure**: for a C API that takes `(callback, void* userdata)`, write a
static trampoline that recovers the `Callback` from `userdata`. For a C++
interface like TensorRT's `ILogger`, implement the interface in your shim and
forward to a stored `Callback`. `beegen` skips raw function-pointer parameters
with a note pointing here.

While a long native call runs, hand the lock back so other Bee threads can
work: `bee::native::GilOff off(interp);`.

---

## Class hierarchies and factories

Interface-based APIs — TensorRT, ONNX Runtime, most COM-like C++ — never hand you
a constructible object. They give you an abstract interface from a factory
function and expect `destroy()` instead of `delete`. That shape is bound
directly:

```cpp
class IEngine {
public:
    virtual bool run(BeeBuffer in, BeeBuffer out) = 0;
    virtual void destroy() = 0;
};
IEngine* create_engine(ILogger* logger, int batch = 1);
```

```bee
let eng = new IEngine(infer.create_engine(logger._handle))
eng.run(input, output)
eng.destroy()          # the API's own teardown, not free()
```

`beegen` detects the pure virtual methods, generates **no** constructor and
**no** `free()` (deleting through an abstract base with no virtual destructor is
undefined behaviour), and gives the wrapper an `init(handle)` that adopts what
the factory returned.

Derived handles are accepted where a base is expected. The generated module
registers a real `static_cast` for each base:

```cpp
bee::native::registerUpcast("ConsoleLogger", "ILogger",
    [](void* p) -> void* { return static_cast<ILogger*>((ConsoleLogger*)p); });
```

so the pointer is adjusted correctly even under multiple inheritance, rather than
being reinterpreted and silently corrupting.

---

## Errors from a C++ library

A bound library throwing its own exception type — `cv::Exception`,
`Ort::Exception`, `std::bad_alloc` — becomes an ordinary Bee error with a
stack trace instead of unwinding past the interpreter and aborting:

```
Runtime error: native error: OpenCV(4.6.0) error: (-215:Assertion failed) !empty()
  at load_image()  vision.bee:22
  at <main>        detect.bee:4
```

This is a catch-all at the boundary, so nothing a library throws can take the
process down. A **segfault** inside the library still can — that isn't an
exception, and no error handling can catch it.

---

## What gets skipped, and why

Every declaration `beegen` can't map is **reported**, because a binding that
silently omits half a library is worse than one that tells you:

```
skipped 5 declaration(s):
  template function identity -- templates need explicit instantiation
  function sum_all -- variadic (...) functions can't be called safely
  function take_unbound -- parameter 1 (Unbound *): pointer to unbound type 'Unbound'
  function out_param -- parameter 1 (int &): non-const reference (possible out-parameter)
  class Unbound -- no bindable public members
```

The usual causes:

- **Templates.** There's nothing to call until they're instantiated. Bind a
  concrete typedef instead (`using IntVec = std::vector<int>;`).
- **Variadics.** `printf`-style functions can't be called safely without knowing
  the argument types at the call site.
- **Non-const references.** `int&` is usually an out-parameter, which has no
  Bee equivalent. Wrap it in C++ with a return value instead.
- **Unbound types.** A pointer or reference to a class that wasn't bound — often
  because it's only forward-declared, or because a `--prefix` / `--namespace`
  filter excluded it. Note that filtering out a class also unbinds every function
  that takes one.
- **Containers.** `std::vector`, `std::map` and friends aren't mapped yet.

If a run reports clang errors, take them seriously: a missing `-I` makes half a
header invisible and the bindings quietly smaller.

```bash
beegen gl.h -m gl -- -I/usr/local/include -DGL_GLEXT_PROTOTYPES
```

Everything after `--` goes to clang verbatim.

---

## Options

| Option | Effect |
|---|---|
| `-m`, `--module <name>` | the Bee module to generate (required) |
| `-o`, `--out-dir <dir>` | where to write the files (default: `.`) |
| `--namespace <ns>` | only bind declarations in this namespace (repeatable) |
| `--prefix <p>` | only bind names starting with this (repeatable) |
| `--skip <name>` | leave one function, class or method out (repeatable) |
| `--no-classes` / `--no-enums` | bind a subset |
| `--no-manifest` | don't write `hive.json` |
| `-I <dir>` | an include directory, for clang and for `build.sh` |
| `--std <std>` | C++ standard to parse with (default `c++17`) |
| `--bee-src <dir>` | where `bee_native.hpp` lives, for `build.sh` |
| `--libclang <path>` | a specific libclang, if the search doesn't find yours |
| `-q`, `--quiet` | print only warnings, errors and the skip report |

`--namespace` and `--prefix` are how you bind one library out of a header that
pulls in many.

---

## Shipping a binding as a package

The generated `hive.json` makes a binding installable like anything else:

```bash
beegen sqlite3.h -m sqlite && ./build.sh
hive pack .                       # -> sqlite-0.1.0.hive
hive install ./sqlite-0.1.0.hive  # into hive_modules/, where import finds it
```

The manifest lists both `sqlite.bee` and `sqlite_native.so`, and the interpreter
finds a package's native library through the same lookup as any module — a
package's `"main"` may even point straight at a `.so`.

One caveat worth stating plainly: a `.hive` holding a compiled `.so` only works
on the platform and toolchain it was built for. Ship the generated sources and a
`build.sh`, or publish per-platform archives.

---

## Writing a native module by hand

`beegen` is a convenience, not a requirement. A native module is just a shared
library exporting two symbols:

```cpp
#include "bee_native.hpp"
using namespace bee;

extern "C" const char* bee_native_abi() { return BEE_NATIVE_ABI; }

extern "C" int bee_module_init(NativeModule* m) {
    m->def("add", 2, [](Interpreter&, std::vector<Value>& a) {
        return Value(native::num(a[0], "add", 0) + native::num(a[1], "add", 1));
    });
    m->constant("ANSWER", Value(42.0));
    return 0;   // non-zero aborts the import
}
```

```bash
g++ -std=c++17 -O2 -fPIC -shared -I/path/to/beelang/src demo.cpp -o demo.so
bee -e 'import demo; print(demo.add(2, 3))'
```

[`src/bee_native.hpp`](../src/bee_native.hpp) has the conversion helpers:
`num`, `integer<T>`, `str`, `boolean`, `listArg`, `toVector` / `fromVector`, and
`makeHandle` / `handle<T>` for opaque pointers. They throw `RuntimeError` on a
mismatch, which the interpreter reports at the call site with a full stack trace.

---

## Memory, threads and safety

**Handles are not garbage collected.** Bee collects its own values, but a
handle points at memory only C++ knows about. Call `free()` when you're done:

```bee
let r = new Rect(3, 4)
try { use(r) } finally { r.free() }
```

`free()` blanks the handle, so a use-after-free or a double free is reported
rather than corrupting the heap:

```
Runtime error: Rect_area: argument 1 is a Rect handle that has already been freed
```

**Native code runs under the GIL**, like any built-in. A function that blocks
should hand the lock back so other threads can run:

```cpp
m->def("slow", 0, [](Interpreter& I, std::vector<Value>&) {
    I.gilRelease();
    do_slow_thing();
    I.gilAcquire();
    return Value();
});
```

**A native module can crash the process.** It is real C++ with real pointers —
a wrong cast segfaults, and no Bee error handling can catch that. Argument
types are checked at the boundary; what the library does afterwards is on the
library.

---

## Binding a real library: the recipe

Large C++ libraries are rarely bound header-first. The pattern that works — and
the one every other language uses — is a **thin shim**: a small C++ file that
exposes the slice of the library you actually want, in the flat, buffer-passing
shape `beegen` maps cleanly. Then generate bindings for the shim, not for
100,000 lines of headers.

```
your_shim.hpp   ->  beegen  ->  vision_native.cpp + vision.bee
     |                              |
     +-- calls the real library     +-- links against it
```

Write the shim so that:

- bulk data is a `BeeBuffer` parameter, never a list;
- outputs are written into a caller-supplied buffer, so nobody has to decide who
  frees what;
- objects you keep are returned as pointers with an explicit destroy function;
- overloads you don't need simply aren't declared.

### What each of the four needs

| Library | Shape of its API | What the shim has to do |
|---|---|---|
| **OpenCV** | `cv::Mat`, heavy overloads, `InputArray` proxies, defaults everywhere | Wrap `Mat` as a handle *or* pass pixels as a `BeeBuffer` with `(rows, cols, type)`. Bind the ~20 functions you use (`imread`, `resize`, `cvtColor`, `Canny`, `imwrite`), not `cv::`. Link with `pkg-config --libs opencv4`. |
| **ONNX Runtime** | a C API reached through `OrtGetApiBase()->GetApi()` — a struct of function pointers, so there is almost nothing for a generator to see | Shim it to flat functions: `ort_session_open(path)`, `ort_run(session, BeeBuffer in, BeeBuffer out)`, `ort_input_shape(session, i)` returning `std::vector<long long>`. Session/env are handles with explicit close. |
| **TensorRT** | abstract interfaces (`IBuilder`, `ICudaEngine`), factory functions, a mandatory `ILogger` **callback**, CUDA device memory | The interface/factory/destroy shape is supported directly. Implement `ILogger` in the shim forwarding to a `Callback`. Device memory never becomes a buffer: `cudaMemcpy` from a host `BeeBuffer` inside the shim. |
| **NumPy** | not a C++ library at all — it's a Python package | Nothing to bind. The Bee equivalent is the **buffer** type above. For heavy numerics, bind Eigen or xtensor through a shim, or add buffer operations to the interpreter. |

The abstract-interface, factory, callback, buffer and default-argument support
above exists precisely so those shims stay small.

---

## Requirements and limits

- **`beegen` needs a libclang shared library at run time**, not at build time —
  it's loaded with `dlopen` through a hand-declared slice of the stable C ABI, so
  building Bee needs no clang headers at all. Install `libclang-dev` (Debian
  and Ubuntu) or point `LIBCLANG_PATH`/`--libclang` at the library.
- **Native modules use a C++ ABI, not a C one.** `Value` holds `std::variant` and
  `std::shared_ptr`, so a module must be built with the same compiler and standard
  library as the `bee` that loads it. `bee_native_abi()` catches version drift;
  it can't catch a toolchain mismatch, which usually shows up as a crash on
  import.
- **Native modules that call back into Bee need the interpreter's symbols
  exported.** The bundled build does this with `-rdynamic`; a custom build of
  `bee` must too, or `Interpreter::callValue` will be undefined at load time. On
  Windows this needs an import library, which the build doesn't produce yet — so
  callbacks are POSIX-only for now.
- Not mapped yet: containers other than `std::vector` (`std::map`, `std::array`,
  `std::optional`), raw function-pointer parameters (write a trampoline — see
  [Calling back into Bee](#calling-back-into-beelang)), templates, operator
  overloads, nested classes, and out-parameters (`int&`).
- Buffers are dense and row-major only: no strides, so a non-contiguous
  `cv::Mat` ROI has to be cloned before it crosses.
- Handles are not garbage collected, and a buffer handed to a library that keeps
  the pointer past the call is a dangling pointer. Copy in the shim.
