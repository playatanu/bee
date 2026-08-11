# Classes & inheritance

Declare with `class`. `init` is the constructor. Methods refer to the instance
with `this`. The `fn` keyword before a method is optional.

```
class Animal {
    init(name) {
        this.name = name
    }
    speak() {
        return this.name + " makes a sound"
    }
    str() {                      # optional: used by print() / str()
        return "Animal(" + this.name + ")"
    }
}

let a = Animal("generic")        # `new` is optional sugar: new Animal("generic")
print(a.speak())                 # generic makes a sound
print(a)                         # Animal(generic)   (via str())
print(a.name)                    # generic           (field access)
```

Inherit with `extends`; call an overridden parent method with `super`:

```
class Dog extends Animal {
    speak() {
        return super.speak() + ": woof!"
    }
}

let d = Dog("Rex")
print(d.speak())                 # Rex makes a sound: woof!
```

- `new C(args)` and `C(args)` are equivalent.
- If a class defines `init`, constructor arguments are passed to it.
- Defining a method named `str` that returns a string customizes how instances
  print.
