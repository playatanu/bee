# Truthiness

Only `nil` and `false` are falsey. **Everything else is truthy**, including
`0`, `""`, and `[]`.

```
if 0 { print("zero is truthy") }        # prints
if "" { print("empty str is truthy") }  # prints
if [] { print("empty list is truthy") } # prints
if nil { } else { print("nil is falsey") }  # prints
```
