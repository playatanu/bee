# Sized numeric types

Bee's numbers are dynamic — every number is a 64-bit float, and `type(x)` always
reports `number`. On top of that, a binding can carry a **sized numeric type**:
a promise about its width and how it wraps or rounds. Annotations are optional;
a program with none behaves exactly as before.

```
let a: u8 = 300        # -> 44   (wraps into 0..255)
let b: i8 = 127 + 1    # -> -128 (two's-complement overflow)
let f: f32 = 1.0 / 3.0 # -> 0.333333343267441 (single precision)
```

## The types

| Family   | Types                       | Range / precision |
|----------|-----------------------------|-------------------|
| Signed   | `i8` `i16` `i32` `i64`      | two's-complement, e.g. `i8` is −128…127 |
| Unsigned | `u8` `u16` `u32` `u64`      | e.g. `u8` is 0…255 |
| Float    | `f16` `f32` `f64`           | half / single / double precision |

`num` remains the plain dynamic number. These names match the element types a
[buffer](buffers.md) can hold.

## Where an annotation applies

An annotation is a [type annotation](type-annotations.md), so the width is
enforced wherever a value **enters** the binding — a `let`, an assignment, a
parameter, or a return — and only there:

```
let x: i16 = 0             # a typed variable
fn scale(n: u8) -> u8 {    # a typed parameter and return
    return n * 2           # coerced to u8 on return
}
x = 40000                  # coerced on every assignment, not just the first
```

At each of those points the value is **coerced** into the type:

- **Integers** truncate toward zero, then wrap two's-complement into range:
  `u8(300)` → `44`, `i8(128)` → `-128`, `u8(-1)` → `255`, `i32(9.9)` → `9`.
- **Floats** round to the type's precision. `f32`/`f16` drop the extra bits a
  double carries; `f64` is unchanged.

Everything between those points is ordinary double arithmetic — only the value
that lands in the typed binding is coerced:

```
let acc: u8 = 250
for i in range(10) { acc = acc + 1 }   # each store wraps
print(acc)                             # -> 4
```

## Identical everywhere, and fast

The interpreter, the register VM, and the `beec` AOT compiler produce the same
result for a typed program. The annotation is also a compilation hint: when
`beec` compiles a program, a sized local that isn't captured by a closure
becomes a native machine value (`int32_t`, `double`, …) with native arithmetic
and no boxing, so a typed hot loop runs as fast as the untyped version and
integer wrapping is free.

## Notes and limits

- Values are still doubles, so `type(x)` is `number` and a typed value passed to
  an unannotated function is just a number.
- `i64`/`u64` carry only the ~53 bits a double represents exactly, and a single
  expression whose intermediate integer result exceeds 2⁶³ falls back to a
  defined modular reduction rather than native wrapping. Neither affects
  ordinary code.
- A non-number assigned to a sized binding is a type error, reported like any
  other annotation mismatch.
