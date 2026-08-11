# Built-in functions

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
