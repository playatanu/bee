# Sized numeric types

Bee's numbers are dynamic — every number is a 64-bit float, and `type(x)` always
reports `number`. On top of that, a binding can carry a **sized numeric type**:
a promise about its width and how it wraps or rounds. Annotations are optional;
a program with none behaves exactly as before.

```bee
let a: u8 = 300        # -> 44   (wraps into 0..255)
let b: i8 = 127 + 1    # -> -128 (two's-complement overflow)
let f: f32 = 1.0 / 3.0 # -> 0.333333343267441 (single precision)
```

## The types

| Family   | Types                                   | Range / precision |
|----------|-----------------------------------------|-------------------|
| Signed   | `i8` `i16` `i32` `i64`                  | two's-complement, e.g. `i8` is −128…127 |
| Unsigned | `u8` `u16` `u32` `u64`                  | e.g. `u8` is 0…255 |
| Float    | `f16` `f32` `f64`                       | half / single / double precision |

`num` remains the plain dynamic number (a double). These names match the element
types a [`buffer`](LANGUAGE.md#buffers) can hold.

## Where an annotation applies

The width is enforced wherever a value **enters** the binding — and only there:

```bee
let x: i16 = 0             # a typed variable
fn scale(n: u8) -> u8 {    # a typed parameter and return
    return n * 2           # the result is coerced to u8 on return
}
x = 40000                  # coerced on every assignment, not just the first
```

At each of those points the value is **coerced** into the type:

- **Integers** truncate toward zero, then wrap two's-complement into range.
  `u8(300)` → `44`, `i8(128)` → `-128`, `u8(-1)` → `255`, `i32(9.9)` → `9`.
- **Floats** round to the type's precision. `f32` and `f16` lose the extra bits
  a double carries; `f64` is unchanged.

Everything between those points is ordinary double arithmetic, so intermediate
results aren't clamped — only the value that lands in the typed binding is:

```bee
let acc: u8 = 250
for i in range(10) { acc = acc + 1 }   # each store wraps
print(acc)                             # -> 4
```

## Semantics across engines

The interpreter, the register VM, and the `beec` AOT compiler agree on wrapping
and rounding for every value that fits the ~53 bits a `double` represents
exactly — which is the entire range of `i8`…`i32`/`u8`…`u32` and the working
range of ordinary code. A typed function runs on the tree-walker under the
VM/JIT (which don't model wrapping); the AOT compiler implements it directly.

One deliberate difference: a **compiled binary computes sized-integer arithmetic
as true fixed-width two's-complement integers** — that is what the type promises.
The interpreter and VM compute through `double`, so an intermediate integer
result larger than 2⁵³ (for example a big `i32 * i32` product) is rounded before
it wraps. There the AOT binary is *exact* and the interpreter is approximate:

```bee
let a: i32 = 2000000000
print(a * a)          # AOT binary: -1651507200 (exact 32-bit wrap)
                      # interpreter: rounds the 4e18 product first
```

This only shows up for integer results beyond 2⁵³; every value inside that range
(all of `i8`…`u32`, and normal `i64` use) is identical on every engine.

## Speed

The annotation is also a compilation hint. When `beec` compiles a program, a
sized local that isn't captured by a closure becomes a **native machine value**
— integer types work in `int64_t`, floats in `double` — and its arithmetic,
comparisons, and loop conditions compile to native machine instructions with no
boxing and no per-operation coercion in hot loops. Integer wrapping costs nothing
(it is the hardware's own two's-complement). A typed numeric loop in a compiled
binary runs at native speed — on a tight nested `i64` loop it matches, and can
beat, the interpreter's JIT.

```bash
beec examples/11_numeric_types.bee -o types && ./types
```

## Notes and limits

- Values are still doubles at the language level, so `type(x)` is `number` and a
  typed value passed to an unannotated function is just a number.
- At the language level values are doubles, so a sized number that leaves its
  native local — printed, returned into a dynamic context, stored in a list — is
  observed through a `double` and so carries ~53 bits. In a compiled binary the
  arithmetic between native integer locals is exact 64-bit two's-complement (see
  *Semantics across engines*); the interpreter/VM stay double-based throughout.
- A non-number assigned to a sized binding is a type error, reported the same
  way as any other annotation mismatch (`declared i32 but got str`).

See [`examples/11_numeric_types.bee`](../examples/11_numeric_types.bee) for a
runnable tour.
