# Values & types

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
