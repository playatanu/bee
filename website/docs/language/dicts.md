# Dicts

String-keyed maps. Keys that aren't strings are converted with `str()`.

```
let user = {"name": "Ada", "age": 36}
user["role"] = "engineer"     # add / update
print(user["name"])           # Ada
print(user["missing"])        # nil   (missing keys read as nil)
print(len(user))              # 3
```

Dot access is sugar for keys, **but built-in method names take precedence**:

```
let d = {"u": {"name": "Ada"}}
print(d.u)          # {"name": "Ada"}   (key access via dot)
print(d.keys())     # ["u"]             (method, not a key named "keys")
```

To read a key that collides with a method name, use `d["keys"]`.

Iterating a dict yields its keys:

```
for k in {"a": 1, "b": 2} {
    print(k)        # a, then b
}
```

See [dict methods](type-methods.md) for `keys`, `values`, `has`, `get`, `remove`.
