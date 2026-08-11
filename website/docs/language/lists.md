# Lists

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

Take a range with a [slice](strings.md#slicing): `xs[1:3]`, `xs[:2]`, `xs[-2:]`.

See [list methods](type-methods.md) for `push`, `pop`, `contains`, `insert`, etc.
