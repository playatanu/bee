# Type methods

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
