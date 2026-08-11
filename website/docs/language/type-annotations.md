# Type annotations

Types are **optional**. A program with no annotations behaves exactly as it
always has, and an unannotated parameter accepts anything. What an annotation
buys is a guarantee: it is checked where the value enters, so from then on the
type is known rather than assumed.

Annotate a parameter or a variable with `:`, and a return type with `->`:

```
fn add(a: num, b: num) -> num {
    return a + b
}

fn greet(name: str, times: num = 2) -> str {
    let out: str = ""
    for (let i = 0; i < times; i = i + 1) { out = out + name + "!" }
    return out
}
```

The type names are `num`, `str`, `bool`, `list`, `dict`, `buffer`, `nil`, `fn`,
and `any` - plus any class name:

```
class Animal { fn init(n: str) { this.n = n } fn name() -> str { return this.n } }
class Dog extends Animal { }

fn describe(a: Animal) -> str { return a.name() }
print(describe(new Dog("rex")))    # rex -- a Dog is an Animal
```

A class annotation accepts instances of that class **and of anything deriving
from it**, so `describe` above takes a `Dog` without complaint. `any` is the
same as writing nothing.

A violated annotation is a runtime error, reported where the bad value came
from, with the usual stack trace:

```
fn f(a: num) -> num { return a }
f("hi")
# Runtime error: parameter 'a' of 'f' is declared num but got str
#   at f()      demo.bee:2
#   at <main>   demo.bee:2
```

Return types are checked on every path out, including falling off the end -
a function declared `-> num` that never returns anything yields `nil`, and that
is an error:

```
fn broken() -> num { let x = 1 }
# Runtime error: 'broken' is declared to return num but returned nil
```

Annotations may be mixed freely with unannotated code. A typed function can call
an untyped one and vice versa; only what is declared is enforced.

See [sized numeric types](numeric-types.md) for fixed-width integer and
float annotations (`i8`…`u64`, `f16`/`f32`/`f64`).
