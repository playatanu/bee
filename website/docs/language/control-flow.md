# Control flow

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
