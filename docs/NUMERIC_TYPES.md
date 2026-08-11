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

## Semantics are identical everywhere

The interpreter, the register VM, and the `beec` AOT compiler all produce the
**same** result for a typed program — wrapping and rounding are defined once and
shared. A typed function simply runs on the tree-walker under the VM/JIT (which
don't model wrapping), while the AOT compiler implements it directly.

## Speed

The annotation is also a compilation hint. When `beec` compiles a program, a
sized local that isn't captured by a closure becomes a **native machine value**
(`int32_t`, `double`, …) with native arithmetic — no boxing in hot loops. A
typed accumulator loop runs at the same speed as the untyped, dynamically-typed
version, and integer wrapping costs nothing (the hardware does it).

```bash
beec examples/11_numeric_types.bee -o types && ./types
```

## Notes and limits

- Values are still doubles at the language level, so `type(x)` is `number` and a
  typed value passed to an unannotated function is just a number.
- Because the model is double-based, `i64`/`u64` carry only the ~53 bits a
  double represents exactly; and a single expression whose intermediate integer
  result exceeds 2⁶³ falls back to a defined modular reduction rather than
  native machine wrapping. Neither affects ordinary code.
- A non-number assigned to a sized binding is a type error, reported the same
  way as any other annotation mismatch (`declared i32 but got str`).

See [`examples/11_numeric_types.bee`](../examples/11_numeric_types.bee) for a
runnable tour.
