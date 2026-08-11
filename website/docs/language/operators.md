# Operators

**Arithmetic:** `+  -  *  /  %`

```
print(7 + 2)       # 9
print(7 / 2)       # 3.5
print(7 % 3)       # 1
print(2 * 3)       # 6
```

`+` is overloaded:

```
print(1 + 2)              # 3          (numbers add)
print("foo" + "bar")      # foobar     (strings concatenate)
print("n=" + 5)           # n=5        (if either side is a string, the other is stringified)
print([1, 2] + [3, 4])    # [1, 2, 3, 4]   (lists concatenate)
```

`*` also repeats strings and lists:

```
print("ab" * 3)           # ababab
print([0] * 4)            # [0, 0, 0, 0]
```

Dividing or taking modulo by zero is a runtime error.

**Comparison:** `==  !=  <  >  <=  >=`

```
print(3 < 5)              # true
print("abc" < "abd")      # true   (lexicographic; both operands must be strings)
print([1, 2] == [1, 2])   # true   (lists/dicts compare by value, deeply)
print(1 == "1")           # false  (different types are never equal)
```

`<  >  <=  >=` require both operands to be numbers, or both to be strings.

**Logical:** `and` / `&&`, `or` / `||`, `not` / `!`

```
print(true and false)     # false
print(true or false)      # true
print(not true)           # false
print(a && b || c)        # && and || are aliases for and / or
```

`and` / `or` short-circuit and return one of their operands (not a coerced bool):

```
let name = user_name or "anonymous"
```

**Assignment:** `=`, and compound `+=  -=  *=  /=`

```
let n = 10
n += 5      # 15
n -= 2      # 13
n *= 2      # 26
n /= 2      # 13
```

> Compound assignment works on **variables** and is desugared to `x = x <op> y`.
> It is **not** supported on properties or index targets - write those out in
> full: `obj.count = obj.count + 1`, `a[i] = a[i] + 1`.
