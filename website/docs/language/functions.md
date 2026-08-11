# Functions & closures

Declare with `fn`. `return` is optional; a function with no `return` yields `nil`.

```
fn add(a, b) {
    return a + b
}
print(add(2, 3))       # 5
```

Functions are first-class values and close over their defining scope:

```
fn make_counter() {
    let count = 0
    fn tick() {
        count += 1
        return count
    }
    return tick
}

let c = make_counter()
print(c())             # 1
print(c())             # 2
print(c())             # 3
```

The number of arguments must match the number of parameters (there are no
default or variadic parameters for user functions).
