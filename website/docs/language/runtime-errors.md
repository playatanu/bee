# Runtime errors

An uncaught error prints what went wrong and a **stack trace** -- the file, line
and function for every call on the way in, innermost first -- and the interpreter
exits with a non-zero status:

```
Runtime error: division by zero
  at safe_div()  lib/math.bee:8
  at total()     report.bee:14
  at <main>      report.bee:31
```

Each row is where *that* function was executing, so the innermost row is the
failing line and the rows below it are the calls that led there. Methods appear
as `Class.method()`, and functions from an installed package name the package's
own file -- so an error inside a dependency is traceable to its source.

Lex and parse errors name their file too:

```
Parse error: expected ')' after arguments
  at greet.bee:12
```

Catch and handle errors with [`try` / `catch`](error-handling.md), or use `assert`
for defensive checks. A caught runtime error binds as a single-line string with
its location, so printing it inside your own message stays readable:

```
try { risky() } catch (e) { print("failed: " + e) }
# failed: Runtime error: division by zero (lib/math.bee:8)
```

Common runtime errors include undefined variables, wrong argument counts,
indexing out of range, calling a non-callable, and type mismatches in operators.

### Recursion depth

Bee stops a runaway recursion with an error instead of letting the process die:

```
Runtime error: call stack overflow in 'walk' (deeper than 2293 nested calls) -- unbounded recursion?
       if the depth is intentional, raise it with BEE_MAX_DEPTH (and the stack with 'ulimit -s')
  at walk()  tree.bee:4
  ... 2280 more frames ...
  at <main>  tree.bee:19
```

The limit scales with the stack the process actually has, so `ulimit -s 65536`
genuinely buys deeper recursion; `BEE_MAX_DEPTH` overrides it outright. Very deep
traces are truncated in the middle -- the innermost and outermost frames are the
informative ones.
