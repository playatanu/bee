# BeeLang Language Reference

The complete language reference for **BeeLang**. New to the language? Start with
the [README](../README.md) — installation and a quick tour live there.

- [Comments](#comments)
- [Values & types](#values--types)
- [Variables](#variables)
- [Operators](#operators)
- [Truthiness](#truthiness)
- [Strings](#strings)
- [Lists](#lists)
- [Dicts](#dicts)
- [Control flow](#control-flow)
- [Functions & closures](#functions--closures)
- [Classes & inheritance](#classes--inheritance)
- [Modules](#modules)
- [Error handling](#error-handling)
- [Built-in functions](#built-in-functions)
- [System library](#system-library) — file I/O, time, random, env, processes
- [Threads](#threads)
- [Type methods](#type-methods)
- [Runtime errors](#runtime-errors)
- [Editor support](#editor-support)
- [A complete example](#a-complete-example)

---

## Comments

Two line-comment styles; there are no block comments.

```
# this is a comment
// so is this
let x = 1   # trailing comment
```

---

## Values & types

| Type | Examples | Notes |
|------|----------|-------|
| `nil` | `nil` | absence of a value |
| `bool` | `true`, `false` | |
| `number` | `42`, `3.14`, `-7` | 64-bit float; integral values print without a decimal |
| `string` | `"hi"`, `'hi'` | immutable |
| `list` | `[1, 2, 3]` | ordered, mutable, heterogeneous |
| `dict` | `{"a": 1}` | string-keyed map, mutable |
| function | `fn ...`, closures | first-class |
| class / instance | `class ...`, `new C()` | |
| module | `import ...` | |

Use `type(x)` to get a type name at runtime:

```
print(type(42))        # number
print(type("hi"))      # string
print(type([1,2]))     # list
print(type(nil))       # nil
```

---

## Variables

Declare with `let`. Assignment (`=`) targets an existing binding.

```
let x = 10
let name = "Ada"
let empty          # defaults to nil
x = x + 1          # reassign
```

Variables are lexically scoped. Blocks, functions, and loops introduce new
scopes; inner scopes can shadow outer names.

```
let x = 1
{
    let x = 2
    print(x)       # 2
}
print(x)           # 1
```

---

## Operators

**Arithmetic:** `+  -  *  /  %`

```
print(7 + 2)       # 9
print(7 / 2)       # 3.5
print(7 % 3)       # 1
print(2 * 3)       # 6
```

`+` is overloaded:

```
print(1 + 2)              # 3          (numbers add)
print("foo" + "bar")      # foobar     (strings concatenate)
print("n=" + 5)           # n=5        (if either side is a string, the other is stringified)
print([1, 2] + [3, 4])    # [1, 2, 3, 4]   (lists concatenate)
```

`*` also repeats strings and lists:

```
print("ab" * 3)           # ababab
print([0] * 4)            # [0, 0, 0, 0]
```

Dividing or taking modulo by zero is a runtime error.

**Comparison:** `==  !=  <  >  <=  >=`

```
print(3 < 5)              # true
print("abc" < "abd")      # true   (lexicographic; both operands must be strings)
print([1, 2] == [1, 2])   # true   (lists/dicts compare by value, deeply)
print(1 == "1")           # false  (different types are never equal)
```

`<  >  <=  >=` require both operands to be numbers, or both to be strings.

**Logical:** `and` / `&&`, `or` / `||`, `not` / `!`

```
print(true and false)     # false
print(true or false)      # true
print(not true)           # false
print(a && b || c)        # && and || are aliases for and / or
```

`and` / `or` short-circuit and return one of their operands (not a coerced bool):

```
let name = user_name or "anonymous"
```

**Assignment:** `=`, and compound `+=  -=  *=  /=`

```
let n = 10
n += 5      # 15
n -= 2      # 13
n *= 2      # 26
n /= 2      # 13
```

> Compound assignment works on **variables** and is desugared to `x = x <op> y`.
> It is **not** supported on properties or index targets — write those out in
> full: `obj.count = obj.count + 1`, `a[i] = a[i] + 1`.

---

## Truthiness

Only `nil` and `false` are falsey. **Everything else is truthy**, including
`0`, `""`, and `[]`.

```
if 0 { print("zero is truthy") }        # prints
if "" { print("empty str is truthy") }  # prints
if [] { print("empty list is truthy") } # prints
if nil { } else { print("nil is falsey") }  # prints
```

---

## Strings

Single or double quoted. Supported escapes: `\n  \t  \r  \\  \"  \'  \0`.

```
let s = "line1\nline2"
let q = 'she said "hi"'
```

Index with `[]` (returns a one-character string). Negative indices count from
the end.

```
let s = "hello"
print(s[0])        # h
print(s[-1])       # o
print(len(s))      # 5
```

Strings are immutable and have [methods](#type-methods):

```
print("Hello".upper())            # HELLO
print("a,b,c".split(","))         # ["a", "b", "c"]
print("  hi  ".trim())            # hi
print("foobar".replace("o", "0")) # f00bar
```

---

## Lists

Ordered and mutable. Index with `[]` (negative indices allowed).

```
let xs = [1, 2, 3]
xs.push(4)            # [1, 2, 3, 4]
print(xs[0])          # 1
print(xs[-1])         # 4
xs[0] = 99            # assign by index
print(len(xs))        # 4
print(xs.pop())       # 4  (removes and returns last)
```

Iterate with `for … in`:

```
let total = 0
for x in [10, 20, 30] {
    total += x
}
print(total)          # 60
```

See [list methods](#type-methods) for `push`, `pop`, `contains`, `insert`, etc.

---

## Dicts

String-keyed maps. Keys that aren't strings are converted with `str()`.

```
let user = {"name": "Ada", "age": 36}
user["role"] = "engineer"     # add / update
print(user["name"])           # Ada
print(user["missing"])        # nil   (missing keys read as nil)
print(len(user))              # 3
```

Dot access is sugar for keys, **but built-in method names take precedence**:

```
let d = {"u": {"name": "Ada"}}
print(d.u)          # {"name": "Ada"}   (key access via dot)
print(d.keys())     # ["u"]             (method, not a key named "keys")
```

To read a key that collides with a method name, use `d["keys"]`.

Iterating a dict yields its keys:

```
for k in {"a": 1, "b": 2} {
    print(k)        # a, then b
}
```

See [dict methods](#type-methods) for `keys`, `values`, `has`, `get`, `remove`.

---

## Control flow

### if / else

Parentheses around the condition are optional.

```
if x > 0 {
    print("positive")
} else if x < 0 {
    print("negative")
} else {
    print("zero")
}
```

### while

```
let n = 3
while n > 0 {
    print(n)
    n -= 1
}
```

### for (C-style)

```
for (let i = 0; i < 5; i = i + 1) {
    print(i)        # 0 1 2 3 4
}
```

The parentheses are optional: `for let i = 0; i < 5; i += 1 { … }`.

### for … in (iteration)

Iterates over a **list** (elements), **string** (characters), or **dict** (keys):

```
for c in "abc"          { write(c, " ") }   # a b c
for x in [10, 20]       { print(x) }         # 10, 20
for k in {"a": 1}       { print(k) }         # a
for i in range(3)       { print(i) }         # 0, 1, 2
```

### break / continue

```
for i in range(10) {
    if i == 3 { continue }   # skip 3
    if i == 6 { break }      # stop at 6
    write(i, " ")            # 0 1 2 4 5
}
```

---

## Functions & closures

Declare with `fn`. `return` is optional; a function with no `return` yields `nil`.

```
fn add(a, b) {
    return a + b
}
print(add(2, 3))       # 5
```

Functions are first-class values and close over their defining scope:

```
fn make_counter() {
    let count = 0
    fn tick() {
        count += 1
        return count
    }
    return tick
}

let c = make_counter()
print(c())             # 1
print(c())             # 2
print(c())             # 3
```

The number of arguments must match the number of parameters (there are no
default or variadic parameters for user functions).

---

## Classes & inheritance

Declare with `class`. `init` is the constructor. Methods refer to the instance
with `this`. The `fn` keyword before a method is optional.

```
class Animal {
    init(name) {
        this.name = name
    }
    speak() {
        return this.name + " makes a sound"
    }
    str() {                      # optional: used by print() / str()
        return "Animal(" + this.name + ")"
    }
}

let a = Animal("generic")        # `new` is optional sugar: new Animal("generic")
print(a.speak())                 # generic makes a sound
print(a)                         # Animal(generic)   (via str())
print(a.name)                    # generic           (field access)
```

Inherit with `extends`; call an overridden parent method with `super`:

```
class Dog extends Animal {
    speak() {
        return super.speak() + ": woof!"
    }
}

let d = Dog("Rex")
print(d.speak())                 # Rex makes a sound: woof!
```

- `new C(args)` and `C(args)` are equivalent.
- If a class defines `init`, constructor arguments are passed to it.
- Defining a method named `str` that returns a string customizes how instances
  print.

---

## Modules

Each `.be` or `.bee` file is a module. Modules are resolved relative to the importing
file's directory and a sibling `lib/` folder; the `.be`/`.bee` extension is added
automatically. Dotted names map to sub-directories (`a.b.c` → `a/b/c.bee`).

`mathutil.bee`:
```
let PI = 3.14159265358979

fn square(x) { return x * x }
```

Importing it:

```
import mathutil                      # bind the module object as `mathutil`
print(mathutil.square(5))            # 25
print(mathutil.PI)

import mathutil as m                 # bind under an alias
print(m.square(6))

from mathutil import square          # bring specific names into scope
print(square(7))

from mathutil import square as sq    # ... with an alias
print(sq(8))

from mathutil import *               # bring in all public names
print(square(9))
```

`from … import *` skips names beginning with `_` (treat those as private).
Circular imports are tolerated (a module is cached as soon as it starts loading).

---

## Error handling

Raise an error with `throw`, and handle it with `try` / `catch` / `finally`.

```
try {
    risky()
} catch (e) {
    print("failed:", e)
} finally {
    print("always runs")
}
```

- `throw expr` raises `expr` — it can be any value (a string, a dict, an
  instance, …).
- `catch` binds the thrown value. The binding is optional: `catch (e) { }`,
  `catch e { }`, or bare `catch { }`.
- Built-in runtime errors (division by zero, index out of range, …) are catchable
  too; they arrive as their **message string**.
- `finally` runs on every exit path — normal completion, a handled or rethrown
  error, or a `return`/`break`/`continue` leaving the block.
- At least one of `catch` / `finally` is required.

```
# throw a structured error and inspect it
fn parse_age(s) {
    let n = num(s)
    if n < 0 { throw {"kind": "range", "msg": "age cannot be negative"} }
    return n
}

try {
    parse_age("-5")
} catch (err) {
    print(err.kind, "-", err.msg)     # range - age cannot be negative
}
```

There is no automatic re-raise; if `catch` doesn't handle everything, `throw` the
value again inside the handler.

---

## Built-in functions

Always available, no import required.

### I/O
| Function | Description |
|----------|-------------|
| `print(...values)` | Print values separated by spaces, then a newline. |
| `write(...values)` | Like `print` but with no trailing newline. |
| `input([prompt])` | Print the optional prompt, read one line from stdin (returns `nil` at EOF). |

### Conversion & inspection
| Function | Description |
|----------|-------------|
| `len(x)` | Length of a string, list, or dict. |
| `type(x)` | Type name (`"number"`, `"string"`, …; a class name for instances). |
| `str(x)` | Display-string form of a value. |
| `repr(x)` | Debug form (strings come back quoted). |
| `num(x)` | Parse a number from a string / bool / number. |
| `int(x)` | Truncate a number (or parsed string) toward zero. |
| `bool(x)` | Truthiness of a value. |

### Math
| Function | Description |
|----------|-------------|
| `abs(n)` | Absolute value. |
| `floor(n)` / `ceil(n)` / `round(n)` | Round down / up / nearest. |
| `sqrt(n)` | Square root. |
| `pow(base, exp)` | Exponentiation. |
| `min(a, b, ...)` / `min(list)` | Smallest number. |
| `max(a, b, ...)` / `max(list)` | Largest number. |
| `range(stop)` / `range(start, stop[, step])` | Build a list of numbers. |

### Collections
| Function | Description |
|----------|-------------|
| `push(list, x)` | Append `x`; returns the list. |
| `pop(list)` | Remove and return the last element. |
| `keys(dict)` | List of keys. |
| `values(dict)` | List of values. |

### Misc
| Function | Description |
|----------|-------------|
| `ord(s)` | Character code of the first character. |
| `chr(n)` | One-character string for code point `n`. |
| `assert(cond[, message])` | Raise a runtime error if `cond` is falsey. |

```
print(range(5))            # [0, 1, 2, 3, 4]
print(range(1, 4))         # [1, 2, 3]
print(range(0, 10, 2))     # [0, 2, 4, 6, 8]
print(max([3, 1, 4, 1]))   # 4
print(chr(65), ord("A"))   # A 65
assert(1 + 1 == 2, "math broke")
```

---

## System library

These built-ins reach outside the program: the filesystem, the clock, the OS
environment, and external processes. They're always available (no import).

### File I/O
| Function | Description |
|----------|-------------|
| `read_file(path)` | Whole file as a string. |
| `read_lines(path)` | List of lines (newlines stripped). |
| `write_file(path, text)` | Write `text`, truncating the file. |
| `append_file(path, text)` | Append `text` to the file. |
| `file_exists(path)` | `true` if the path exists. |
| `remove_file(path)` | Delete a file; returns whether it was removed. |
| `make_dir(path)` | Create a directory (and parents); returns whether it was created. |
| `list_dir(path)` | List of entry names in a directory. |

```
write_file("notes.txt", "hello\nworld\n")
print(read_lines("notes.txt"))     # ["hello", "world"]
if file_exists("notes.txt") { remove_file("notes.txt") }
```

### Time & date
| Function | Description |
|----------|-------------|
| `clock()` | Monotonic seconds — use for measuring elapsed time. |
| `time()` | Seconds since the Unix epoch (with fraction). |
| `now()` | Local date/time as a dict: `year, month, day, hour, minute, second, weekday, yearday`. |
| `format_time(fmt[, epoch])` | Format with `strftime` codes (e.g. `"%Y-%m-%d %H:%M"`). |
| `sleep(seconds)` | Pause; releases the lock so other threads run. |

```
let t0 = clock()
heavy_work()
print("took", clock() - t0, "seconds")
print(format_time("%Y-%m-%d"))     # 2026-08-05
```

### Random
| Function | Description |
|----------|-------------|
| `random()` | Float in `[0, 1)`. |
| `random_int(a, b)` | Integer in `[a, b]` inclusive. |
| `random_range(a, b)` | Float in `[a, b)`. |
| `random_choice(list)` | A random element. |
| `random_seed(n)` | Seed the generator (same seed ⇒ same sequence). |

```
random_seed(42)
print(random_int(1, 6))            # a dice roll, reproducible
print(random_choice(["red", "green", "blue"]))
```

### Environment & arguments
| Function | Description |
|----------|-------------|
| `env(name[, default])` | Environment variable, or `default` / `nil`. |
| `set_env(name, value)` | Set an environment variable. |
| `args()` | List of command-line arguments after the script path. |

```
./bee script.bee alpha beta      # args() == ["alpha", "beta"]
print(env("HOME"))
print(env("MISSING", "n/a"))
```

### Processes
| Function | Description |
|----------|-------------|
| `exec(cmd)` | Run `cmd` in the shell; returns `{"code": exit_status, "output": stdout}`. |

```
let r = exec("ls -1")
if r.code == 0 {
    for line in r.output.trim().split("\n") { print(line) }
}
```

---

## Threads

Bee has a **global interpreter lock (GIL)**, like CPython: threads run
concurrently and share data safely, but only one runs Bee code at a time. This
is ideal for I/O-bound work (network, files, `sleep`, `exec`) — the lock is
released during those blocking calls so other threads make progress. It does
**not** give CPU parallelism.

| Function | Description |
|----------|-------------|
| `spawn(fn[, ...args])` | Start `fn(...args)` on a new thread; returns a handle. |
| `join(handle)` | Wait for the thread and return its result (re-raises a thrown error). |

```
fn download(url) {
    # ... a blocking call releases the lock, so peers run meanwhile ...
    return exec("curl -s " + url).output
}

let a = spawn(download, "http://example.com/a")
let b = spawn(download, "http://example.com/b")
let ra = join(a)
let rb = join(b)      # both downloads overlapped
```

Because the GIL serializes Bee execution, shared lists/dicts/objects don't
corrupt — updates between blocking points are effectively atomic:

```
let total = [0]
fn add_up() { for i in range(100000) { total[0] = total[0] + 1 } }
let ts = []
for i in range(4) { ts.push(spawn(add_up)) }
for t in ts { join(t) }
print(total[0])       # exactly 400000 — no lost updates
```

Any threads you don't `join` are joined automatically when the program ends.
Errors thrown inside a thread are re-raised by `join`, so wrap it in `try`:

```
fn risky() { throw "nope" }
let t = spawn(risky)
try { join(t) } catch (e) { print("thread failed:", e) }
```

> **Note:** there are no anonymous/lambda functions yet, so pass a *named*
> function to `spawn` (`spawn(worker, arg)`), not an inline one.

---

## Type methods

Called with dot syntax on a value, e.g. `"hi".upper()`.

### String
| Method | Description |
|--------|-------------|
| `len()` / `length()` | Number of characters. |
| `upper()` / `lower()` | Case conversion. |
| `trim()` | Strip leading/trailing whitespace. |
| `contains(s)` | Whether `s` occurs in the string. |
| `starts_with(s)` / `ends_with(s)` | Prefix / suffix test. |
| `split(sep)` | Split into a list (empty `sep` splits into characters). |
| `replace(from, to)` | Replace all occurrences. |
| `substr(start, len)` | Substring (`start` may be negative). |
| `to_num()` | Parse the string as a number. |

### List
| Method | Description |
|--------|-------------|
| `len()` / `length()` | Number of elements. |
| `push(x)` / `append(x)` | Append; returns the list. |
| `pop()` | Remove and return the last element. |
| `contains(x)` / `includes(x)` | Membership test (by value). |
| `index_of(x)` | First index of `x`, or `-1`. |
| `insert(i, x)` | Insert `x` at index `i`. |
| `remove_at(i)` | Remove and return the element at index `i`. |

### Dict
| Method | Description |
|--------|-------------|
| `keys()` | List of keys. |
| `values()` | List of values. |
| `has(key)` | Whether the key exists. |
| `get(key[, default])` | Value for `key`, or `default` / `nil`. |
| `remove(key)` | Delete a key; returns the dict. |
| `len()` / `length()` | Number of entries. |

```
let xs = [3, 1, 2]
xs.insert(0, 99)          # [99, 3, 1, 2]
print(xs.index_of(1))     # 2
print(xs.contains(2))     # true

let d = {"a": 1}
print(d.get("a"))         # 1
print(d.get("z", 0))      # 0
print(d.has("a"))         # true
```

---

## Runtime errors

Uncaught lex, parse, and runtime errors are reported with a line number, and the
interpreter exits with a non-zero status:

```
Runtime error (line 7): division by zero
```

Catch and handle them with [`try` / `catch`](#error-handling), or use `assert`
for defensive checks. Common runtime errors include undefined variables, wrong
argument counts, indexing out of range, calling a non-callable, and type
mismatches in operators.

---

## Editor support

A VS Code extension in [`editors/vscode-bee/`](../editors/vscode-bee/) provides syntax
highlighting, completions (keywords, built-ins, `.`-methods, and file symbols),
hovers, and snippets. See [`editors/vscode-bee/README.md`](../editors/vscode-bee/README.md) for
installation.

---

## A complete example

```
# fizzbuzz, plus a tiny class

fn fizzbuzz(n) {
    let out = []
    for i in range(1, n + 1) {
        if i % 15 == 0 {
            out.push("FizzBuzz")
        } else if i % 3 == 0 {
            out.push("Fizz")
        } else if i % 5 == 0 {
            out.push("Buzz")
        } else {
            out.push(str(i))
        }
    }
    return out
}

print(fizzbuzz(15))

class Stack {
    init() { this.items = [] }
    push(x) { this.items.push(x) }
    pop()   { return this.items.pop() }
    size()  { return len(this.items) }
}

let s = Stack()
s.push(1)
s.push(2)
print(s.size())     # 2
print(s.pop())      # 2
```

The `examples/` directory has more complete programs.

