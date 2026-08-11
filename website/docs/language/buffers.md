# Buffers

A **buffer** is a contiguous typed array: raw bytes with a dtype and a shape. Use
one instead of a list when data is large or when it has to reach a C library -
a list of numbers costs 16 bytes an element and is converted one element at a
time, while a buffer is handed over as a single pointer with no copy at all.

```
let b = buffer([2, 3], "f32")     # 6 zeroed float32s
print(b)                          # buffer<f32>[2,3] [0, 0, 0, 0, 0, 0]
print(len(b))                     # 6      -- element count
print(shape(b))                   # [2, 3]
print(dtype(b))                   # f32
print(byte_len(b))                # 24
```

Element types: `f32`, `f64`, `i8`, `u8`, `i16`, `u16`, `i32`, `i64` (`float32`,
`uint8` … also work). Values are read and written as ordinary numbers and stored
in the dtype, so a `u8` buffer keeps one byte per element.

### Creating

```
zeros([480, 640, 3], "u8")        # an image's worth of pixels, 900 KB
ones(4)                           # buffer<f64>[4] [1, 1, 1, 1]
full([2, 2], 7, "i32")            # every element 7
buffer_from([1, 2, 3])            # from a flat list
buffer_from([[1, 2], [3, 4]], "u8")   # nesting sets the shape
to_list(b)                        # back to nested lists
```

### Reading and writing

`[]` indexes flat, whatever the shape; `at` and `set_at` take one index per
dimension. Negative indices count from the end.

```
b[0] = 1.5
print(b[0])                       # 1.5
print(b[-1])                      # last element

set_at(b, 9, 1, 2)                # b[row 1][col 2] = 9
print(at(b, 1, 2))                # 9
fill(b, 0)                        # every element
```

### Shape and type

```
reshape(b, [3, 2])                # same data, new shape (a copy)
astype(b, "u8")                   # converted copy
copy(b)                           # independent copy
```

### Arithmetic

Enough to prepare and inspect data, not a numeric library. The second operand may
be a buffer of the same length or a single number.

```
buf_add(b, 1)      buf_sub(a, b)     buf_mul(b, 255)    buf_div(b, 2)
buf_sum(b)         buf_min(b)        buf_max(b)
```

Buffers compare by contents, like lists: `buffer_from([1,2]) == buffer_from([1,2])`
is `true`. Passing one to a native module is a pointer hand-off - see the
[bindings guide](../bindings.md#buffers-bulk-data-without-a-copy).
