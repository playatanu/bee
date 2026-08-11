# Variables

Declare with `let`. Assignment (`=`) targets an existing binding.

```
let x = 10
let name = "Ada"
let empty          # defaults to nil
x = x + 1          # reassign
```

Variables are lexically scoped. Blocks, functions, and loops introduce new
scopes; inner scopes can shadow outer names.

```
let x = 1
{
    let x = 2
    print(x)       # 2
}
print(x)           # 1
```
