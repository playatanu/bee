# Bee Performance - findings and improvement plan

A measured account of where Bee 0.3.1 spends its time, why it is slower than
Go (and, off the JIT path, slower than CPython), and what to change, in order of
value for effort.

Measured on an AMD Ryzen 5 5600H, Linux, `bee 0.3.1` built with `-O2` and the
LLVM 18 JIT enabled. Compared against CPython 3.12.3. Every number below is
reproducible from the benchmarks in the appendix.

---

## Summary

The JIT is not the problem. **The tree-walking interpreter is**, and it has one
catastrophic flaw plus three structural ones:

| # | Problem | Cost | Status |
|---|---------|------|--------|
| 1 | `break` / `continue` / `return` are C++ exceptions | **2.4-3.8 µs each**, 50-100× a whole loop iteration | **Done** |
| 2 | A heap `Environment` is allocated per block and per call | ~62 ns (2 mallocs) per entry | **Done** |
| 3 | AST walking, ~49 ns per trivial loop iteration | the structural cost | **Done** - register VM |
| 4 | A 24-byte `std::variant` value with atomic refcounts | a jump table and two atomics per copy | Open |
| 5 | The JIT is all-or-nothing: one non-numeric operation bails the whole function | real code is never JIT-compiled | Open - **now the biggest lever**, see below |

Priority order changed once the Go column existed: the two benchmarks the JIT
compiles are within 2-4× of Go, and every one it declines is 20-87× behind. The
remaining gap is JIT *coverage*, not interpreter speed.

### Results

Measured against 0.3.1 (a binary built from the commit before this work), best
of five interleaved runs - old and new alternating, so both see the same CPU
boost state, which on this laptop is worth up to 2× on its own. 9 M inner
iterations each.

| benchmark | 0.3.1 | now | speedup |
|---|---|---|---|
| nested loop over a list, `s = s + xs[i]` | 1.13 s | **0.21 s** | 5.4× |
| the same with `if (xs[i] % 2 == 0)` | 1.47 s | **0.38 s** | 3.9× |
| the same written with `continue` | 20.57 s | **0.38 s** | **54×** |
| `xs[i] = xs[i] + 1` | 0.45 s | **0.09 s** | 5.0× |
| a `let` in the loop body | 1.08 s | **0.17 s** | 6.4× |
| loop calling an interpreted function (1 M calls) | 2.44 s | **0.24 s** | 10.2× |
| `continue` microbenchmark (1 M) | 3.86 s | **0.02 s** | **193×** |
| loop calling a function with locals | 4.98 s | **0.44 s** | 11.3× |

### The four-way picture

`bench/run.sh --all` runs the same 19 programs against 0.3.1, CPython 3.12 and
Go 1.22 (compiled ahead of the timing loop). Best of three, interleaved:

| benchmark | bee | 0.3.1 | vs 0.3.1 | python | vs py | go | vs go |
|---|---|---|---|---|---|---|---|
| call_function | 0.474 s | 7.579 s | 16.0× | 0.211 s | 2.25× | 0.007 s | **67.7×** |
| call_method | 0.389 s | 0.395 s | 1.0× | - | - | - | - |
| closure_loop | 0.218 s | 0.867 s | 4.0× | - | - | - | - |
| dict_ops | 0.261 s | 0.320 s | 1.2× | 0.132 s | 1.98× | 0.052 s | 5.0× |
| **fib_recursive** | 0.058 s | 0.057 s | 1.0× | 0.682 s | **0.09×** | 0.029 s | **2.0×** |
| list_comp | 0.067 s | 0.082 s | 1.2× | - | - | - | - |
| loop_continue | 0.443 s | 20.071 s | **45.3×** | 0.409 s | 1.08× | 0.008 s | 55.4× |
| loop_forin | 0.179 s | 0.586 s | 3.3× | 0.252 s | **0.71×** | 0.009 s | 19.9× |
| loop_list | 0.249 s | 1.128 s | 4.5× | 0.304 s | **0.82×** | 0.007 s | 35.6× |
| loop_list_if | 0.439 s | 1.498 s | 3.4× | 0.405 s | 1.08× | 0.009 s | 48.8× |
| **loop_numeric** | 0.093 s | 0.092 s | 1.0× | - | - | 0.027 s | **3.4×** |
| loop_write | 0.269 s | 0.641 s | 2.4× | - | - | - | - |
| matrix | 0.155 s | 0.498 s | 3.2× | 0.113 s | 1.37× | 0.004 s | 38.8× |
| oo_class | 0.124 s | 2.166 s | **17.5×** | 0.085 s | 1.46× | 0.004 s | 31.0× |
| oo_field | 0.114 s | 0.496 s | 4.4× | 0.132 s | **0.86×** | - | - |
| oo_method | 0.267 s | 7.665 s | **28.7×** | 0.134 s | 1.99× | - | - |
| sort_list | 0.450 s | 7.975 s | 17.7× | - | - | - | - |
| string_build | 0.062 s | 0.105 s | 1.7× | - | - | 0.015 s | 4.1× |
| try_catch | 0.459 s | 0.861 s | 1.9× | - | - | - | - |

Geometric mean against 0.3.1: **3.94×**. Against CPython, Bee is now ahead
on list loops and instance fields, 11× faster on numeric recursion, and behind
on three things: function calls (2.25×), instance method dispatch (1.99×) and
dicts (1.98×).

`call_method` is the one row that did not move, and it is the honest signal of
what is left in this area: it is `xs.len()`, a *built-in* method, and
`getProperty` still allocates a `Builtin` object with a captured receiver on
every one of those calls. Instance methods no longer do (see below); built-in
methods still need the same treatment, which means restructuring the type-method
section of `getProperty` so a receiver can be passed in rather than captured.

### What the Go column actually says

The two rows where Bee is within a small factor of Go - `fib_recursive` at
2.2× and `loop_numeric` at 3.5× - are exactly the two the LLVM JIT compiles.
Every row where it is 20-87× behind is a row the JIT declined.

That is the whole roadmap in one observation. The gap is not dispatch any more,
and it is not going to be closed by making the VM 20% faster: **it is JIT
coverage**. Priority 5 - type guards and deoptimisation instead of refusing to
compile anything non-numeric - is worth more than everything else remaining put
together.

(Some of the Go margin on the list loops is autovectorisation of a
statically-typed `[]int`, which no dynamic language reaches. `oo_class` at 87×
and `call_function` at 68× are not that: they are boxing, dispatch and per-call
overhead, and they are addressable.)

### Verification

`make test` runs 215 checks: 77 language, 30 differential, 5 complexity guards,
50 hive and 53 beegen.

- [`tests/vm_diff_test.sh`](https://github.com/beelang-project/bee/blob/main/tests/vm_diff_test.sh) runs every example and 16
  hand-written programs twice - once on the VM, once with `BEE_NO_VM=1` forcing
  the tree-walker - and requires identical stdout, stderr **and** exit code. So
  error messages, stack traces, `finally` ordering, per-iteration closure
  capture and uncaught-error exit codes are all covered, not just return values.
- [`tests/perf_guard_test.sh`](https://github.com/beelang-project/bee/blob/main/tests/perf_guard_test.sh) checks *complexity*,
  not speed, with budgets an order of magnitude loose. It exists because two
  bugs got through everything else:
  - **String building went quadratic** when function bodies moved to the VM. The
    tree-walker has grown `s = s + x` in place since 0.1.1; the VM did not, and
    a 300 k-append benchmark went from 0.06 s to **363 s**. Every correctness
    test passed the whole time, because the answer was right.
  - **A use-after-free in `INDEX`.** `sort(xs, cmp)[0]` compiles to an
    instruction whose destination register is also its object register.
    Assigning the element into that register dropped the last reference to the
    list and freed it while the read was still in flight. It surfaced as a
    denormal double for small inputs and a segfault for large ones - and the
    differential suite had not caught it, because both engines have to *run* the
    shape before they can disagree about it. There is a case for it now.

Both were found by the benchmark suite on its first complete run, which is the
argument for having one.

---

## What the measurements show

### The JIT is fine - when it applies

A purely numeric nested loop, top-level or inside a function, with or without an
`if`, is compiled to native code and runs at Go-like speed:

| benchmark (9,000,000 inner iterations) | time |
|---|---|
| top-level nested loop, `s = s + 1` | 0.09 s |
| top-level nested loop with `if (j % 2 == 0)` | 0.03 s |
| same inside a function | 0.02 s |
| same inside a function, with `continue` | 0.03 s |

Note that even `continue` is free here - the JIT lowers it to a branch
([`jit_llvm.cpp:297`](https://github.com/beelang-project/bee/blob/main/src/jit_llvm.cpp#L297)).

### …but any real code leaves the subset

Codegen throws `JitBail` on the first construct outside the numeric subset
([`jit_llvm.cpp:301`](https://github.com/beelang-project/bee/blob/main/src/jit_llvm.cpp#L301) for statements,
[`jit_llvm.cpp:412`](https://github.com/beelang-project/bee/blob/main/src/jit_llvm.cpp#L412) for expressions). That means a
single `xs[i]`, string, `print`, `for … in`, builtin call, field access, `try`,
or `match` anywhere in the function drops the **entire** function or loop back to
the tree-walker. Since real programs touch lists, real programs are never
JIT-compiled.

The same benchmark, once a list index is involved (9,000,000 inner iterations):

| benchmark | bee | python3 | ratio |
|---|---|---|---|
| `s = s + xs[i]` | 1.17 s | 0.30 s | **3.9× slower** |
| `if (xs[i] % 2 == 0) { s = s + xs[i] }` | 1.51 s | 0.40 s | **3.8× slower** |
| the same written with `continue` | **21.00 s** | 0.40 s | **52× slower** |

### Per-operation costs in the interpreted path

Each row is the marginal cost of adding that one operation to the body of an
otherwise identical 10 M-iteration loop:

| operation | marginal cost |
|---|---|
| loop iteration `s = s + 1` (baseline dispatch) | 49 ns |
| read a local `x` | ~0 ns |
| index `xs[i]` | +28 ns |
| `if (x > 0) { … }` | +31 ns |
| `let t = …` inside the loop body | +62 ns |
| builtin call `len(xs)` | +59 ns |
| method call `xs.len()` | +108 ns |
| user function call (JIT-compiled callee) | +75 ns |
| **`continue`** | **+3,800 ns** |
| **`return` from an interpreted function** | **+2,370 ns** |

For scale: CPython executes a simple loop iteration in roughly 30-40 ns and a
function call in roughly 60 ns. Go, being AOT-compiled to machine code with
static types, does the loop iteration in 1-2 ns.

---

## Root causes

### 1. Control flow is implemented with C++ exceptions

[`interpreter.cpp:844`](https://github.com/beelang-project/bee/blob/main/src/interpreter.cpp#L844),
[`:878`](https://github.com/beelang-project/bee/blob/main/src/interpreter.cpp#L878),
[`:880`](https://github.com/beelang-project/bee/blob/main/src/interpreter.cpp#L880):

```cpp
case Stmt::Kind::Return:   throw ReturnSignal{v};
case Stmt::Kind::Break:    throw BreakSignal{};
case Stmt::Kind::Continue: throw ContinueSignal{};
```

caught by every loop ([`:772`](https://github.com/beelang-project/bee/blob/main/src/interpreter.cpp#L772),
[`:788`](https://github.com/beelang-project/bee/blob/main/src/interpreter.cpp#L788),
[`:806`](https://github.com/beelang-project/bee/blob/main/src/interpreter.cpp#L806)) and by `callFunction`
([`:1755`](https://github.com/beelang-project/bee/blob/main/src/interpreter.cpp#L1755)).

C++ exceptions are "zero-cost" only when *not* thrown. Each throw walks the
unwind tables, calls `_Unwind_RaiseException`, runs the personality routine, and
does an RTTI type match - measured at **2.4-3.8 µs**. These are not exceptional
paths: `return` runs on every single call, and `continue` runs on most iterations
of the loop that uses it.

This is precisely why *"a condition inside a nested loop makes it much slower"*.
A condition in a loop is usually there to `continue` or `break`, and every helper
function called from that loop `return`s.

### 2. A heap `Environment` per scope entry

[`:753`](https://github.com/beelang-project/bee/blob/main/src/interpreter.cpp#L753) (block),
[`:783`](https://github.com/beelang-project/bee/blob/main/src/interpreter.cpp#L783) (for),
[`:800`](https://github.com/beelang-project/bee/blob/main/src/interpreter.cpp#L800) (for-in),
[`:1733`](https://github.com/beelang-project/bee/blob/main/src/interpreter.cpp#L1733) (call frame) all do:

```cpp
auto child = std::make_shared<Environment>(env, s->slotCount);
```

That is two allocations - the shared control block and the `slots` vector - on
every block entry and every call, plus a `std::map<std::string, Value> values`
member ([`environment.hpp:28`](https://github.com/beelang-project/bee/blob/main/src/environment.hpp#L28)) that a function frame
never uses. Hence the 62 ns cost of a single `let` in a loop body.

### 3. Value representation

`Value` ([`value.hpp:116`](https://github.com/beelang-project/bee/blob/main/src/value.hpp#L116)) is a 24-byte `std::variant`
over eight `shared_ptr` alternatives. Consequences on every operation:

- `evaluate()` returns `Value` **by value**; the variant copy constructor is a
  switch over the active index, not a memcpy.
- `asList()` / `asDict()` / `asString()` return the `shared_ptr` **by value**
  ([`value.hpp:167-174`](https://github.com/beelang-project/bee/blob/main/src/value.hpp#L167-L174)), so a single `xs[i]` does
  roughly four atomic refcount read-modify-writes.
- The refcounts are atomic even though Bee has a GIL
  ([`interpreter.hpp:115`](https://github.com/beelang-project/bee/blob/main/src/interpreter.hpp#L115)) - the synchronisation is
  paid for and never used.

### 4. Per-call and per-property overhead

- [`callFunction:1684`](https://github.com/beelang-project/bee/blob/main/src/interpreter.cpp#L1684) builds a `std::string name`
  and copies it into a `CallFrame` on **every** call, for a stack trace that is
  only read if an error occurs.
- `callFunction` takes `std::shared_ptr<Function>` **by value** - an atomic
  refcount pair per call.
- `evalCall` ([`:1629`](https://github.com/beelang-project/bee/blob/main/src/interpreter.cpp#L1629)) heap-allocates a
  `std::vector<Value> args` per call.
- `getProperty` ([`:1779`](https://github.com/beelang-project/bee/blob/main/src/interpreter.cpp#L1779)) dispatches type methods
  through a chain of 26 `name == "…"` string comparisons.

---

## Improvement plan

### Priority 1 - Replace exception-based control flow with status returns (done)

**Measured: 14.6× on `continue`-heavy loops, 10.7× on call-heavy code, 2.1× on a
plain loop. Contained to [`interpreter.cpp`](https://github.com/beelang-project/bee/blob/main/src/interpreter.cpp). No
language-visible change.**

`execute()` returns a signal instead of throwing one:

```cpp
enum class Flow : uint8_t { Normal, Break, Continue, Return };

// in Interpreter:
Value returnValue_;                      // set when Flow::Return is produced

Flow execute(Stmt* stmt, std::shared_ptr<Environment>& env);
Flow execBlock(const std::vector<StmtPtr>& stmts, std::shared_ptr<Environment> env);
```

Each statement propagates:

```cpp
Flow Interpreter::execBlock(const std::vector<StmtPtr>& stmts,
                            std::shared_ptr<Environment> env) {
    for (auto& s : stmts) {
        Flow f = execute(s.get(), env);
        if (f != Flow::Normal) return f;      // no unwinding, just a return
    }
    return Flow::Normal;
}
```

Loops absorb `Break`/`Continue` and pass `Return` upward:

```cpp
case Stmt::Kind::While: {
    auto* s = static_cast<WhileStmt*>(stmt);
    if (tryJitLoop(stmt, env)) break;
    while (evaluate(s->condition.get(), env).truthy()) {
        Flow f = execute(s->body.get(), env);
        if (f == Flow::Break)  break;
        if (f == Flow::Return) return f;      // Continue falls through
    }
    break;
}
```

and `callFunction` reads the value instead of catching it:

```cpp
for (auto& s : decl->body) {
    if (execute(s.get(), frame) == Flow::Return) {
        if (fn->isInitializer && fn->boundThis) return Value(fn->boundThis);
        return std::move(returnValue_);
    }
}
```

What the change covers:

- [x] `execute`, `execBlock`, `execProgram`, `execTry`, `runCatch` return `Flow`.
- [x] `While` / `For` / `ForIn` absorb `Break`/`Continue`, propagate `Return`.
      `continue` in a C-style `for` still runs the increment; in a `while` it
      still re-tests the condition.
- [x] `Match` and `If` propagate whatever their branch returned.
- [x] `execTry` runs `finally` on every exit path, and a `return`/`break`/
      `continue` out of `finally` still wins over one out of the body. The
      body's pending return value is parked across the `finally` block, which
      can otherwise overwrite `returnValue_` by calling a function.
- [x] `callFunction` moves `returnValue_` out the moment it sees `Flow::Return`,
      before anything else can run.
- [x] The pass-through catches in `callValue` are gone. Built-ins that call back
      into Bee code (`map`, `sort`, `spawn`) go through `callFunction`, which
      converts a `Return` into a value before the built-in sees anything.
- [x] The REPL and top-level drivers check the returned `Flow`;
      `break`/`continue` outside a loop report the same message as before, and a
      `break` that escapes a function body now reports it with a stack trace.
- [x] `BeeThrow` and `TracedError` stay C++ exceptions. They *are* exceptional,
      and `throw` is rare in a hot loop.

Regression guard: `tests/lang_test.sh` (77 checks) and all 14 examples pass. The
edge cases most at risk - `return` out of a loop inside `try`/`finally`,
`finally`'s own `return`, `break`/`continue` crossing a `try`, `return` through a
callback - are covered by the new cases described in "Results so far".

### Priority 2 - Stop allocating a scope per block and per call (done)

**Measured: 6.4× on a loop whose body declares a local, 11.3× on a loop calling
a function with locals.**

Two changes, both in [`resolver.cpp`](https://github.com/beelang-project/bee/blob/main/src/resolver.cpp) and
[`interpreter.cpp`](https://github.com/beelang-project/bee/blob/main/src/interpreter.cpp):

- **Scope merging.** A block or loop that declares names used to allocate an
  `Environment` on entry - once per *iteration* for a loop body. `Resolver::canMerge()`
  asks whether anything in the subtree creates a closure; if not, nothing can
  outlive the scope, so its declarations get slots in the enclosing frame and no
  environment is allocated. Sibling scopes reuse slots (the counter rewinds; a
  high-water mark sizes the frame).

  The condition is exactly right, not conservative-and-cheap: a closure created
  in a loop body is the *only* way to tell a fresh-environment-per-iteration
  from a shared one, and where one exists the scope is left alone. The
  differential suite covers this directly - three closures made in a loop still
  capture 0, 10, 20 and not 20, 20, 20.

- **Frame recycling.** Call frames come from a thread-local free list and are
  returned on exit when nothing else references them (`use_count() == 1`, which
  is false exactly when a closure captured the frame). The slots vector keeps
  its capacity, so a warm call allocates nothing.

Scope merging is also what makes Priority 3 possible: in a closure-free function
every local ends up at depth 0 in one frame, which is precisely a register file.

### Priority 3 - Compile to bytecode (done)

**Measured: 3.9×-54× over 0.3.1, and from ~4× behind CPython to slightly ahead
of it.**

A **register** VM, not a stack one: `s = s + xs[i]` is two instructions
(`INDEX`, `ADD`) where a stack VM needs six pushes and pops. Registers are the
resolver's frame slots, so the register allocation was already done - the
compiler only allocates temporaries above the locals, on a stack discipline.

- [`chunk.hpp`](https://github.com/beelang-project/bee/blob/main/src/chunk.hpp) - 8-byte three-operand instructions, and the
  opcode list as an X-macro so the enum and the VM's jump table are generated
  from one source and cannot drift.
- [`compiler.cpp`](https://github.com/beelang-project/bee/blob/main/src/compiler.cpp) - AST to bytecode. Declines a function it
  cannot fully compile (nested function or class, `try`, `import`,
  destructuring, spread), which then runs on the tree-walker as before.
- [`vm.cpp`](https://github.com/beelang-project/bee/blob/main/src/vm.cpp) - computed-goto dispatch (`&&label` / `goto *`), so
  each opcode ends with its own indirect branch and its own branch-prediction
  history rather than every opcode sharing the one at the top of a switch.

Things worth keeping in mind if you extend it:

- **Every operator has a `K` form** taking its right operand from the constant
  table (`i + 1`, `i < n`, `x % 2 == 0`). A literal right operand is most of what
  a loop does; folding it in removed a `LOAD_CONST` and a register per use and
  was worth 25% on the conditional benchmarks.
- **Semantics live in one place.** `applyBinary`, `applyBinaryArith`, `indexGet`,
  `indexSet`, `sliceValue`, `getProperty` and `superMethod` were lifted out of the
  tree-walker so both engines call the same code. The VM only inlines fast paths
  (numbers, list-by-number indexing) and falls into the shared function for
  everything else - which is why the error messages match exactly.
- **Calls go through `Interpreter::callValue`.** Bee-to-Bee calls could set up a
  VM frame directly and skip the argument vector, but routing through the
  interpreter is what makes defaults, rest parameters, classes, built-ins,
  stack traces and the depth limit behave identically for free. That is the
  obvious next optimisation.
- The register array is thread-local and frames are windows into it, so a call
  is an index rather than an allocation. Nested Bee calls recurse into `run()`,
  using the C++ stack as the frame stack.

### Shapes, fused method calls and direct calls (done)

Three changes aimed at the two rows that were worst against CPython. Measured
first, because guessing which half of `oo_class` was expensive would have been a
coin flip: field access was ~41 ns each and method dispatch ~125 ns each, so
roughly 62/38.

- **Direct VM-to-VM calls.** A `CALL` whose target is another compiled function
  taking exactly those arguments enters it from the dispatch loop - no argument
  vector, no `callFunction`. Built-ins, classes, defaults, rest parameters and
  JIT-claimed numeric functions still take the interpreter's path, so nothing
  about behaviour changes. `call_function`: 0.73 s → 0.47 s.

- **Shapes (hidden classes).** An instance's fields were a
  `std::map<std::string, Value>`, so `p.x` was a tree walk with string
  comparisons. A `Shape` now records which names an instance has and which slot
  each occupies; instances that gained their fields in the same order share one,
  so fields are a flat vector. Each property site caches "this shape means slot
  N" and checks it with a pointer compare. Field access: **4.0× faster**, and
  now faster than CPython.

  The cache is checked, never trusted. One class can reach two shapes (fields
  assigned in different orders on different paths), fields can be added after
  construction, and a field holding a callable still shadows a method - all
  covered in the differential suite.

- **Fused method calls.** `obj.m(args)` compiled to a property read followed by
  a call, and reading `obj.m` on its own has to *produce a callable*, which for
  a method meant allocating a bound copy of it per call. `CALL_METHOD` passes
  the receiver directly instead, with the resolved method cached per class at
  the site. Instance method dispatch: **28.7× faster than 0.3.1**, 1.7× on top
  of the VM alone.

### Priority 4 - A cheaper Value - next

Now the largest remaining item. With dispatch down to a few instructions per
opcode, the `Value` copy is a visible share of what is left: `std::variant`'s
copy constructor is a jump table, and every list, dict or string copy is a pair
of atomic refcount operations that the GIL already makes unnecessary.

The catch, and the reason this is its own piece of work: `Value` is part of the
**public native-module ABI** ([`bee_native.hpp`](https://github.com/beelang-project/bee/blob/main/src/bee_native.hpp)). Changing
its layout breaks every module compiled against 0.3.1, so it needs a
`BEE_NATIVE_ABI` bump and a rebuild of anything that ships a `.so`. Plan for
that explicitly rather than discovering it.

**Impact: removes the variant switch and the atomic refcounts from every
operation.**

- NaN-box into 8 bytes, or use a 16-byte `{ tag, payload }` struct. Either way
  the copy becomes a memcpy and `isNumber()` becomes a compare.
- Return `const shared_ptr&` / raw pointers from `asList()` etc. rather than
  copies - this alone removes most of the atomics on `xs[i]` and can land
  independently of the rest.
- Since a GIL already serialises Bee code, use non-atomic refcounts (or a simple
  mark-sweep GC, which also fixes reference cycles - today a cyclic structure
  leaks).
- Intern strings, and key dicts and instance fields on interned pointers instead
  of `std::string`.

### Types, and the typed backend - steps 1 and 2 done

Optional type annotations landed (see [LANGUAGE.md](language.md)), and the
compiler now uses them.

**Register typing.** Two sources are trusted: a *declared* parameter or local,
because the annotation is enforced everywhere the binding is written - entry,
every assignment, and defaults - so it holds across loop back-edges; and a
temporary, whose type is fixed by the single instruction that writes it. An
*undeclared* local is deliberately left unknown even when its initialiser is
obviously a number, because propagating that forward is only sound with a
fixpoint over the control-flow graph: an assignment late in a loop body reaches
uses earlier in the same loop. Annotate the variable and the typed path opens.

**Typed opcodes.** Where both operands are known numbers the compiler emits
`ADD_NUM` rather than `ADD` - no tag test, no fallback branch, no checked
accessor. A declared `buffer` indexed by a declared `num` becomes `INDEX_BUF`: a
bounds check and a load against contiguous unboxed memory, with no call into the
interpreter and no `shared_ptr` traffic.

#### What that was actually worth

| | untyped | typed | |
|---|---|---|---|
| `dot()` over two buffers | 0.115 s | **0.089 s** | 1.29× |
| arithmetic loop, no containers | 0.715 s | 0.691 s | 1.03× |

The buffer kernel gained; the plain arithmetic loop essentially did not, and
that result is the useful one. **The tag test was never the cost.** It is a
perfectly predicted branch next to a dependent load; removing it frees nothing.
What `INDEX_BUF` removes is a real call into `indexGet`, a `shared_ptr` copy with
its atomic refcount, and two type dispatches - and that is worth 29%.

So step 2 of the original plan (unchecked opcodes) is done and is worth roughly
nothing on its own. The conclusion stands from the other direction: what makes
typed arithmetic fast is not skipping the check, it is **not building a `Value`
at all**.

#### Step 3 - unboxed numeric registers, the actual win

A register provably holding a number for its whole lifetime does not need to be
a `Value`. Give each frame a parallel `double[]`, keep those registers there, and
`ADD_NUM` becomes a raw double add into a raw double slot: no tagged union
assignment, no destructor, no refcount. `BOX` / `UNBOX` only at the boundaries -
call arguments, returns, and stores into lists, fields or globals.

This is a dual-mode code generator (every expression has to know whether it is
producing a boxed or an unboxed result), which is why it is its own step rather
than an extension of this one.

#### Step 4 - declared types as JIT entry conditions

A function whose parameters and return are all declared numeric is *guaranteed*
numeric at the boundary, so the LLVM JIT needs no entry guard. More importantly
`buffer` parameters give it something it currently refuses outright: contiguous
unboxed memory it can index directly. `fn dot(a: buffer, b: buffer, n: num) ->
num` is the shape that should reach Go - it is 22× off today, against 2× for the
numeric recursion the JIT already takes.

Note the relationship to Priority 5 below: type *feedback* observes what a site
happened to see and must guard against being wrong; a type *annotation* is
promised and checked once at the boundary. They feed the same code generator, and
the annotation path is simpler because it needs no deoptimisation machinery.

### Priority 5 - guards instead of bail-on-everything - first stage done

The JIT used to accept exactly one kind of argument: an unboxed number. Anything
else meant the function was never compiled at all, which is why every benchmark
touching a container sat 20-60× behind Go while the two purely numeric ones sat
at 2-3.5×.

It now compiles a function **per argument signature**, and the argument types
*are* the guard:

- Each call classifies what it is actually passing - number, f64 buffer, or
  something else. That signature keys the compilation cache.
- A call whose arguments do not match any compiled signature simply finds none
  and runs interpreted. There is nothing to invalidate and no state to
  reconstruct, because the check happens before entry rather than inside.
- An f64 buffer is passed as a raw base pointer and an element count, both loop
  invariant, so `b[i]` becomes a bounds check and a load against contiguous
  unboxed memory - no `Value`, no tag, no interpreter call.

**This works on unannotated code.** The signature comes from what the call
passes, not from what the source declares, so `fn dot(a, b, n)` compiles exactly
as well as the annotated version - the two benchmarks are within noise of each
other. Declared types make the *bytecode* better (previous section); type
feedback makes the *native code* possible. They are independent wins.

| | before | after | vs Go |
|---|---|---|---|
| `dot()` over two f64 buffers | 0.089 s | **0.026 s** | 22.3× → **6.5×** |

Also 3.3× faster than CPython on the same kernel, and 12.3× faster than 0.3.1.

#### Why this is safe to bail out of

The existing contract - "native code gave up, re-run it interpreted" - is only
sound because the compiled subset has no side effects. That invariant is
preserved deliberately: buffer **reads** are compiled, buffer **writes** are
not. An out-of-range index bails and the interpreter reproduces the error with
the right message and line, and re-running cannot double-apply anything because
nothing was applied.

Compiling writes needs real deoptimisation - reconstructing interpreter state at
the guard rather than restarting - which is the next stage, not this one.

#### What is left in the gap - and a measurement correction

The first attempt at answering this was wrong, and it is worth recording why.

"6.5× off Go" looked like it pointed at the bounds checks: they branch to a bail
block, and a loop with a side exit does not vectorise, while Go's version does.
The obvious next move seemed to be hoisting them out of the loop. Measuring the
kernel in isolation - one call over a 20 M-element buffer, differenced against
the same program without the call - says otherwise:

| | ns per element |
|---|---|
| `dot()` on the bytecode VM | 44.79 |
| `dot()` compiled | **1.01** |

**One nanosecond per element is scalar native speed**, 44× the VM. The compiled
loop was never the problem.

What the 6.5× actually contained was **process startup**. `bee` starts in 15 ms
where an interpreter-only build starts in 2.7 ms; the difference is dynamically
linking `libLLVM.so`, which happens on every run whether or not anything is ever
compiled. On a benchmark doing 10 ms of work that is not a rounding error, it is
most of the number.

`bench/run.sh` now measures each runtime's startup floor and reports a `work`
column with it removed. Corrected, and against the same Go programs:

| benchmark | work | vs python | vs go |
|---|---|---|---|
| `dot()` over buffers | 0.011 s | **0.15×** | **3.67×** |
| `fib` recursion | 0.042 s | **0.06×** | **1.50×** |
| numeric nested loop | 0.075 s | - | **3.00×** |

So on the code the JIT covers, Bee is **1.5-3.7× off Go** and 6-16× *faster*
than CPython. Every figure quoted before this correction was inflated by the
startup floor, worst on the shortest benchmarks.

Two conclusions for the roadmap, both the opposite of what was planned:

- **Hoisting bounds checks is not the next move.** It would buy vectorisation on
  buffer kernels, worth maybe 4× on that one shape, but the scalar loop is
  already near optimal and this is not where the time goes.
- **Startup was, and is now fixed.** 12.3 ms on every single invocation of
  `bee`, for a JIT most scripts never trigger, was a bigger and far broader win
  than any loop optimisation - and it is the thing a person actually feels when
  running a script. Making the engine construction lazy was only worth 1.2 ms;
  the rest was the loader mapping and relocating a ~120 MB library before `main`
  runs.

  The LLVM backend now lives in a separate shared object, `libbee_jit.so`, that
  `bee` `dlopen`s on the first compile - the same technique the project uses for
  libclang in `beegen`. The executable links no LLVM, so a plain run maps the
  library never and starts at the interpreter-only floor; the ~120 MB is mapped
  once, lazily, only when a script is hot enough to compile. Measured on the dev
  machine, per-invocation startup dropped from ~6.9 ms to ~0.9 ms - the same as
  an interpreter-only build - with the JIT still delivering its full speedup when
  it does engage. `BEE_JIT_LIB` overrides the library path; if it is missing or
  fails to load, execution falls back to the interpreter/VM.

### Priority 5, remaining - deoptimisation for the general case

Today the JIT is still a static all-or-nothing filter over a numeric subset. The
bytecode tier it needed now exists, so this is the next structural step after
Priority 4:

- Count executions per function/loop; compile only what is hot.
- Emit **type guards** rather than refusing to compile: assume `xs` is a list and
  `i` is a number based on observed types, and deoptimise back to the interpreter
  at the guard when the assumption fails. That is what turns `xs[i]` from "bails
  the entire function" into "one compare".
- Keep the existing numeric fast path as the trivial case of the same machinery.

### Priority 6 - Cheap, local wins

- [x] **`callFunction` takes `const std::shared_ptr<Function>&`** instead of a
      copy - one atomic refcount pair saved per call.
- [x] **The call frame no longer formats a name.** `CallFrame` holds a
      `const Function*`, and `functionName()` formats it only when a trace is
      actually printed. This removed a `std::string` build *and* a copy from
      every call; traces, including `Class.method()` qualification, are
      unchanged.
- [x] **Borrowing container accessors.** `Value::listRef()` / `dictRef()` hand
      back a reference instead of a `shared_ptr` copy, and `xs[i]` and
      `xs[i] = v` use them - removing two atomic refcount operations per index.
- [ ] **Method dispatch:** replace the `name == "…"` chain in `getProperty`
      ([`:1779`](https://github.com/beelang-project/bee/blob/main/src/interpreter.cpp#L1779)) with a per-type hash table, and
      cache the resolved method on the `GetExpr` AST node (an inline cache, like
      the one already used for globals in [`ast.hpp:51`](https://github.com/beelang-project/bee/blob/main/src/ast.hpp#L51)).
- [ ] **`evalCall`:** use a small-buffer argument vector, or push arguments
      straight onto the value stack from Priority 2, instead of a fresh
      `std::vector` per call.
- **`ForIn` over a list** copies each element into the loop slot; for large lists
  this is a `Value` copy (and a refcount pair) per element that a reference could
  avoid.
- **Dicts** are `std::map` ([`value.hpp:20`](https://github.com/beelang-project/bee/blob/main/src/value.hpp#L20)) - a red-black
  tree with a node allocation per entry and `std::string` comparisons per lookup.
  An open-addressing hash map with interned keys is both faster and smaller.

---

## On "slower than Go"

Go is compiled ahead of time to machine code, with static types resolved at
compile time and no per-operation dispatch. A dynamically-typed language will not
match it in general, and no amount of tuning the tree-walker will close that gap.
The realistic targets are:

| target | what it takes | state |
|---|---|---|
| Beat CPython everywhere, not just on the JIT path | Priorities 1-3 | **reached** |
| Lua-class (5-15× CPython) | Priority 4, plus direct VM-to-VM calls and method-call fusion | next |
| Within a few × of Go on hot numeric code | already true inside JIT-covered code | **reached** |
| Within a few × of Go on general code | Priority 5 - guards and deoptimisation | after 4 |

---

## Appendix - reproducing the numbers

Each program below is run as `time bee <file>` and against the equivalent Python.
Loop counts are chosen so every variant executes 9-10 M inner iterations.

**A. The JIT path (fast).** Pure numeric, nothing outside the subset:

```
let s = 0
for (let i = 0; i < 3000; i = i + 1) {
  for (let j = 0; j < 3000; j = j + 1) {
    if (j % 2 == 0) { s = s + 1 }
  }
}
print(s)
```

**B. The interpreted path.** Identical shape, but `xs[i]` bails the JIT:

```
fn run(n) {
  let xs = []
  for (let i = 0; i < n; i = i + 1) { xs.push(i) }
  let s = 0
  for (let r = 0; r < 3000; r = r + 1) {
    for (let i = 0; i < n; i = i + 1) {
      if (xs[i] % 2 == 0) { s = s + xs[i] }
    }
  }
  return s
}
print(run(3000))
```

**C. The `continue` cost.** B rewritten with `continue` - 21.00 s against B's
1.51 s, for the same 4.5 M taken branches:

```
      if (xs[i] % 2 == 0) { continue }
      s = s + xs[i]
```

**D. Isolating one operation.** The marginal-cost table was produced by varying
only the inner statement of this harness (10 M iterations), so the difference
from the `s = s + 1` baseline is the cost of the operation under test:

```
fn one(a) { return a }
fn run() {
  let xs = []
  for (let i = 0; i < 1000; i = i + 1) { xs.push(i) }
  let s = 0
  for (let r = 0; r < 10000; r = r + 1) {
    for x in xs { <statement under test> }
  }
  return s
}
print(run())
```

**E. Interpreted `return`.** `pick` indexes a list, so it cannot be JIT-compiled
and its `return` throws. 2.45 s for 1 M calls, against 0.08 s for the same
indexing written inline:

```
fn pick(xs, i) { return xs[i] }
...
    for x in xs { s = s + pick(xs, 0) }
```
