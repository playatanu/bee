# Error handling

Raise an error with `throw`, and handle it with `try` / `catch` / `finally`.

```
try {
    risky()
} catch (e) {
    print("failed:", e)
} finally {
    print("always runs")
}
```

- `throw expr` raises `expr` - it can be any value (a string, a dict, an
  instance, …).
- `catch` binds the thrown value. The binding is optional: `catch (e) { }`,
  `catch e { }`, or bare `catch { }`.
- Built-in runtime errors (division by zero, index out of range, …) are catchable
  too; they arrive as their **message string**.
- `finally` runs on every exit path - normal completion, a handled or rethrown
  error, or a `return`/`break`/`continue` leaving the block.
- At least one of `catch` / `finally` is required.

```
# throw a structured error and inspect it
fn parse_age(s) {
    let n = num(s)
    if n < 0 { throw {"kind": "range", "msg": "age cannot be negative"} }
    return n
}

try {
    parse_age("-5")
} catch (err) {
    print(err.kind, "-", err.msg)     # range - age cannot be negative
}
```

There is no automatic re-raise; if `catch` doesn't handle everything, `throw` the
value again inside the handler.
