# Strings

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

### Interpolation

Prefix a string with `f` to substitute `{expressions}` into it:

```
let name = "Ada"
let n = 5
print(f"{name} has {n} items")          # Ada has 5 items
print(f"{n} squared is {n * n}")        # 5 squared is 25
print(f"{name.upper()} / {[1, 2]}")     # ADA / [1, 2]
```

Any expression works inside the braces, including strings of its own:

```
let d = {"key": "val"}
print(f"{d["key"]}")                    # val
```

Values are stringified the way `str()` would do it. Write `{{` and `}}` for
literal braces, and note that a string without the `f` prefix is left alone:

```
print(f"{{not an expression}}")         # {not an expression}
print("{name} stays as typed")          # {name} stays as typed
```

### Slicing

`[start:end]` takes a range - `start` is included, `end` is not. Both bounds are
optional and may be negative (counting from the end):

```
let xs = [1, 2, 3, 4, 5]
print(xs[1:3])       # [2, 3]
print(xs[:2])        # [1, 2]
print(xs[2:])        # [3, 4, 5]
print(xs[-2:])       # [4, 5]
print(xs[:])         # [1, 2, 3, 4, 5]   (a copy)

let s = "hello world"
print(s[0:5])        # hello
print(s[-5:])        # world
```

Bounds are clamped rather than checked, so `xs[0:100]` is the whole list and an
inverted range is empty. Slicing a list returns a **new** list - changing it
doesn't touch the original.

Strings are immutable and have [methods](type-methods.md):

```
print("Hello".upper())            # HELLO
print("a,b,c".split(","))         # ["a", "b", "c"]
print("  hi  ".trim())            # hi
print("foobar".replace("o", "0")) # f00bar
```
