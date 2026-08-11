# A complete example

```
# fizzbuzz, plus a tiny class

fn fizzbuzz(n) {
    let out = []
    for i in range(1, n + 1) {
        if i % 15 == 0 {
            out.push("FizzBuzz")
        } else if i % 3 == 0 {
            out.push("Fizz")
        } else if i % 5 == 0 {
            out.push("Buzz")
        } else {
            out.push(str(i))
        }
    }
    return out
}

print(fizzbuzz(15))

class Stack {
    init() { this.items = [] }
    push(x) { this.items.push(x) }
    pop()   { return this.items.pop() }
    size()  { return len(this.items) }
}

let s = Stack()
s.push(1)
s.push(2)
print(s.size())     # 2
print(s.pop())      # 2
```

The `examples/` directory has more complete programs.
