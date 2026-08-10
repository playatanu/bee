#!/usr/bin/env bash
# Differential test: every program is run twice, once on the register VM and
# once with BEE_NO_VM=1 forcing the tree-walker. The two engines must agree on
# stdout, stderr and the exit code -- that is the whole contract of compiling a
# function: it does the same thing, faster.
set -u
cd "$(dirname "$0")/.."
BEE=./bee
pass=0; fail=0
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT

check() {                                  # check <name> <file>
    local name=$1 file=$2
    BEE_NO_VM=1 $BEE "$file" >"$tmp/a.out" 2>"$tmp/a.err"; local ra=$?
    $BEE "$file" >"$tmp/b.out" 2>"$tmp/b.err"; local rb=$?
    if [ "$ra" != "$rb" ]; then
        echo "  FAIL $name: exit $ra (tree-walker) vs $rb (vm)"; fail=$((fail+1)); return
    fi
    if ! diff -q "$tmp/a.out" "$tmp/b.out" >/dev/null; then
        echo "  FAIL $name: stdout differs"; diff "$tmp/a.out" "$tmp/b.out" | head -6; fail=$((fail+1)); return
    fi
    if ! diff -q "$tmp/a.err" "$tmp/b.err" >/dev/null; then
        echo "  FAIL $name: stderr differs"; diff "$tmp/a.err" "$tmp/b.err" | head -6; fail=$((fail+1)); return
    fi
    echo "  ok   $name"; pass=$((pass+1))
}

prog() {                                   # prog <name> <<'EOF' ... EOF
    local name=$1
    cat > "$tmp/p.bee"
    check "$name" "$tmp/p.bee"
}

echo "differential: examples"
for f in examples/*.bee; do
    case "$f" in
        *14_input*) continue;;               # needs stdin
        # These print elapsed wall-clock times, which differ between any two
        # runs of the same engine, let alone two engines.
        *10_system*|*13_benchmark*) continue;;
    esac
    check "$(basename "$f")" "$f"
done

echo "differential: language cases"

prog "arithmetic and precedence" <<'EOF'
fn f(a, b) { return a + b * 2 - (a / 2) % 3 }
fn g(a) { return -a + ~3 + (a & 6) + (a | 1) + (a ^ 2) + (a << 2) + (a >> 1) }
for (let i = 1; i < 8; i = i + 1) { print(f(i, i + 1)) print(g(i)) }
EOF

prog "comparisons and logic" <<'EOF'
fn t(a, b) {
  print(a < b) print(a <= b) print(a > b) print(a >= b) print(a == b) print(a != b)
  print(a < b and b < 10) print(a > b or b > 0) print(not (a == b)) print(a and b) print(a or b)
}
t(1, 2) t(2, 2) t(3, 2)
fn s() { return "abc" < "abd" }
print(s())
EOF

prog "strings, lists, dicts" <<'EOF'
fn build(n) {
  let xs = []
  for (let i = 0; i < n; i = i + 1) { xs.push(i * i) }
  let d = {"a": 1, "b": xs}
  let s = ""
  for x in xs { s = s + str(x) + "," }
  return [xs, d, s, xs[2], d["a"], len(xs), xs[1:3], s[0:3]]
}
print(build(5))
fn cat(a, b) { return a + b }
print(cat("x", "y")) print(cat([1], [2])) print(cat(1, 2))
EOF

prog "control flow" <<'EOF'
fn f(n) {
  let out = []
  for (let i = 0; i < n; i = i + 1) {
    if (i % 3 == 0) { out.push("fizz") continue }
    if (i > 7) { break }
    let j = 0
    while (j < 2) { out.push(i * 10 + j) j = j + 1 }
  }
  return out
}
print(f(12))
fn m(v) { match v { case 1, 2 { return "low" } case 3 { return "three" } default { return "high" } } }
print([m(1), m(2), m(3), m(9)])
fn tern(a) { return a > 0 ? "pos" : a == 0 ? "zero" : "neg" }
print([tern(1), tern(0), tern(-1)])
EOF

prog "nested functions and closures" <<'EOF'
fn outer(n) {
  let acc = 0
  fn inner(k) { acc = acc + k return acc }
  for (let i = 0; i < n; i = i + 1) { inner(i) }
  return acc
}
print(outer(5))
fn counter() { let n = 0 fn bump() { n = n + 1 return n } return bump }
let c = counter()
print([c(), c(), c()])
EOF

prog "classes" <<'EOF'
class Animal {
  fn init(name) { this.name = name this.legs = 4 }
  fn speak() { return this.name + " makes a sound" }
  fn str() { return "Animal(" + this.name + ")" }
}
class Dog extends Animal {
  fn init(name) { super.init(name) this.tricks = [] }
  fn speak() { return this.name + " barks" }
  fn learn(t) { this.tricks.push(t) return len(this.tricks) }
}
let d = new Dog("Rex")
print(d.speak()) print(d.legs) print(d.learn("sit")) print(d.tricks) print(str(d))
let a = new Animal("Cat")
print(a.speak())
EOF

# Field layout is a shape, and property sites cache "this shape means slot N".
# These are the cases where that cache has to miss and refill correctly.
prog "shapes: field layout and inline caches" <<'EOF'
class C {
  fn init(flag) {
    # The same class reaching two different field orders, so one call site
    # sees two shapes.
    if (flag) { this.a = 1 this.b = 2 } else { this.b = 20 this.a = 10 }
  }
  fn sum() { return this.a + this.b }
  fn grow() { this.c = this.a + this.b return this.c }
}
let objs = [new C(true), new C(false), new C(true)]
for o in objs { print([o.a, o.b, o.sum()]) }
for o in objs { print(o.grow()) }
for o in objs { print([o.a, o.b, o.c, o.sum()]) }

# Fields added after construction, out of order.
fn late() {
  let a = new C(true)
  let b = new C(true)
  a.z = "az"  b.y = "by"  b.z = "bz"  a.y = "ay"
  return [a.y, a.z, b.y, b.z]
}
print(late())

# A field holding a callable shadows a method of the same name.
class S {
  fn init() { this.act = fn () { return "field" } }
  fn other() { return "method" }
}
let s = new S()
print(s.act())
print(s.other())

# A method pulled out as a value and called later still binds its receiver.
class Counter { fn init() { this.n = 0 } fn bump() { this.n = this.n + 1 return this.n } }
let c = new Counter()
let f = c.bump
print([f(), f(), c.bump(), c.n])

# Reading a field that does not exist is an error, not nil.
fn missing() { let x = new Counter() return x.nope }
try { print(missing()) } catch (e) { print("caught: " + e) }

# Inheritance: fields set by a base init, read through a derived instance.
class Base { fn init(v) { this.v = v } fn get() { return this.v } }
class Derived extends Base { fn init(v) { super.init(v) this.extra = v * 2 } fn get() { return super.get() + this.extra } }
let d = new Derived(5)
print([d.v, d.extra, d.get()])
EOF

prog "type annotations" <<'EOF'
fn add(a: num, b: num) -> num { return a + b }
fn greet(name: str, times: num = 2) -> str {
  let out: str = ""
  for (let i = 0; i < times; i = i + 1) { out = out + name + "!" }
  return out
}
fn total(xs: list) -> num { let s: num = 0 for x in xs { s = s + x } return s }
fn pick(d: dict, k: str) -> num { return d[k] }
fn flag(b: bool) -> bool { return not b }
fn apply(f: fn, v: num) -> num { return f(v) }
fn dbl(x: num) -> num { return x * 2 }
print(add(2, 3))
print(greet("bee"))
print(greet("bee", 3))
print(total([1, 2, 3]))
print(pick({"a": 7}, "a"))
print(flag(false))
print(apply(dbl, 21))

class Animal { fn init(n: str) { this.n = n } fn name() -> str { return this.n } }
class Dog extends Animal { fn speak() -> str { return super.name() + " barks" } }
fn describe(a: Animal) -> str { return a.name() }
let d = new Dog("rex")
print(describe(d))
print(d.speak())

fn scale(b: buffer, k: num) -> buffer {
  for (let i = 0; i < 4; i = i + 1) { b[i] = b[i] * k }
  return b
}
print(to_list(scale(buffer_from([1, 2, 3, 4], "f64"), 3)))

# Unannotated code keeps its dynamic behaviour.
fn loose(a, b) { return a + b }
print(loose("x", "y"))
print(loose(1, 2))
print(loose([1], [2]))
EOF

# Declared types let the compiler emit unchecked numeric opcodes, which are only
# safe while the annotation actually holds. These are the ways it could stop
# holding.
prog "typed opcodes stay honest" <<'EOF'
# A slot reused by a for-in variable after an annotated let had it: inheriting
# the `num` marking would run unchecked arithmetic on a string.
fn reuse(xs: list) -> str {
  { let a: num = 1  }
  let out: str = ""
  for x in xs { out = out + x }
  return out
}
print(reuse(["a", "b", "c"]))

# The same shape via a list comprehension.
fn reuse2(xs: list) -> num {
  { let a: num = 5  }
  let n: num = 0
  for y in [len(s) for s in xs] { n = n + y }
  return n
}
print(reuse2(["a", "bb", "ccc"]))

# Assignment must keep the annotation true, in a loop as well as once.
fn mutate(n: num) -> num {
  let acc: num = 0
  for (let i: num = 0; i < n; i = i + 1) { acc = acc + i * 2 }
  return acc
}
print(mutate(10))
try { print(mutate(3) + mutate("x")) } catch (e) { print("caught: " + e) }

# A default value has to satisfy the annotation too.
fn withDefault(a: num, b: num = 7) -> num { return a + b }
print(withDefault(1))
print(withDefault(1, 2))

# Typed arithmetic and buffer indexing against the same computation untyped.
fn tdot(a: buffer, b: buffer, n: num) -> num {
  let s: num = 0
  for (let i: num = 0; i < n; i = i + 1) { s = s + a[i] * b[i] }
  return s
}
fn udot(a, b, n) {
  let s = 0
  for (let i = 0; i < n; i = i + 1) { s = s + a[i] * b[i] }
  return s
}
let ba = buffer_from([1, 2, 3, 4], "f64")
let bb = buffer_from([5, 6, 7, 8], "f64")
print([tdot(ba, bb, 4), udot(ba, bb, 4)])

# Out-of-range on the typed buffer path reports the same error as the untyped.
try { print(tdot(ba, bb, 99)) } catch (e) { print("caught: " + e) }
try { print(udot(ba, bb, 99)) } catch (e) { print("caught: " + e) }

# Typed division and modulo keep their zero checks.
fn divs(a: num, b: num) -> num { return a / b }
fn mods(a: num, b: num) -> num { return a % b }
print([divs(9, 2), mods(9, 2)])
try { print(divs(1, 0)) } catch (e) { print("caught: " + e) }
try { print(mods(1, 0)) } catch (e) { print("caught: " + e) }
EOF

# The JIT compiles a function per argument signature and enters native code only
# when the arguments still match. These are the ways the guard has to hold: the
# wrong container, the wrong element type, an index off either end, and a buffer
# whose contents changed between calls.
prog "jit guards on buffer arguments" <<'EOF'
fn dot(a, b, n) {
  let s = 0
  for (let i = 0; i < n; i = i + 1) { s = s + a[i] * b[i] }
  return s
}
let ba = buffer_from([1, 2, 3, 4], "f64")
let bb = buffer_from([5, 6, 7, 8], "f64")

# Warm it up so the native version exists, then keep checking the answer.
for (let r = 0; r < 200; r = r + 1) { dot(ba, bb, 4) }
print(dot(ba, bb, 4))

# The same function with lists: no native version for that signature.
print(dot([1, 2, 3, 4], [5, 6, 7, 8], 4))

# And with an f32 buffer, which is not the layout the native code assumes.
let fa = buffer_from([1, 2, 3, 4], "f32")
print(dot(fa, bb, 4))

# And with plain numbers, where indexing is an error either way.
try { print(dot(1, 2, 4)) } catch (e) { print("caught: " + e) }

# Indexing off the end, and off the front.
try { print(dot(ba, bb, 99)) } catch (e) { print("caught: " + e) }
fn neg(a, n) { let s = 0 for (let i = 0; i < n; i = i + 1) { s = s + a[0 - 1 - i] } return s }
print(neg(ba, 4))
try { print(neg(ba, 99)) } catch (e) { print("caught: " + e) }

# Contents changed between calls: native code must see the new values.
fn total(a, n) { let s = 0 for (let i = 0; i < n; i = i + 1) { s = s + a[i] } return s }
for (let r = 0; r < 200; r = r + 1) { total(ba, 4) }
print(total(ba, 4))
ba[0] = 100
print(total(ba, 4))

# A buffer of a different length, same signature.
let bc = buffer_from([1, 1, 1, 1, 1, 1], "f64")
print(total(bc, 6))
try { print(total(ba, 6)) } catch (e) { print("caught: " + e) }

# Mixed: numeric first call, buffer second, of the same function.
fn poly(x, k) { return x * x + k }
print(poly(3, 1))
try { print(poly(ba, 1)) } catch (e) { print("caught: " + e) }
print(poly(4, 2))
EOF

prog "type violations" <<'EOF'
fn f(a: num) -> num { return a }
fn g() -> num { return "hi" }
fn h() -> num { let x = 1 }
fn i() { let x: num = "s" return x }
class A {} class B {}
fn j(a: A) -> num { return 1 }
try { print(f("hi")) }   catch (e) { print("1: " + e) }
try { print(f(nil)) }    catch (e) { print("2: " + e) }
try { print(f(true)) }   catch (e) { print("3: " + e) }
try { print(g()) }       catch (e) { print("4: " + e) }
try { print(h()) }       catch (e) { print("5: " + e) }
try { print(i()) }       catch (e) { print("6: " + e) }
try { print(j(new B())) } catch (e) { print("7: " + e) }
print(f(42))
EOF

prog "errors and traces" <<'EOF'
fn boom(n) { if (n == 0) { return 1 / 0 } return boom(n - 1) }
try { boom(3) } catch (e) { print("caught: " + e) }
fn thrower() { throw {"code": 42} }
try { thrower() } catch (e) { print(e["code"]) } finally { print("cleanup") }
fn idx() { let xs = [1, 2] return xs[9] }
try { idx() } catch (e) { print("caught: " + e) }
EOF

prog "uncaught error exits nonzero" <<'EOF'
fn a() { return b() }
fn b() { return c() }
fn c() { return 1 / 0 }
print("before")
a()
print("unreachable")
EOF

prog "higher-order builtins" <<'EOF'
fn dbl(x) { return x * 2 }
fn odd(x) { return x % 2 == 1 }
let xs = [5, 3, 1, 4, 2]
print(map(dbl, xs)) print(filter(odd, xs)) print(sort(xs)) print(reduce(fn (a, b) { return a + b }, xs, 0))
fn apply(f, v) { return f(v) }
print(apply(dbl, 21))
EOF

prog "recursion and defaults" <<'EOF'
fn fib(n) { if (n < 2) { return n } return fib(n - 1) + fib(n - 2) }
print(fib(18))
fn greet(name, greeting = "hello") { return greeting + ", " + name }
print(greet("bee")) print(greet("bee", "hi"))
fn total(...xs) { let s = 0 for x in xs { s = s + x } return s }
print(total(1, 2, 3, 4))
EOF

prog "list comprehensions and slices" <<'EOF'
fn f() {
  let xs = [1, 2, 3, 4, 5, 6]
  return [[n * n for n in xs if n % 2 == 0], xs[:3], xs[3:], xs[-2:], xs[:]]
}
print(f())
EOF

prog "buffers" <<'EOF'
fn f() {
  let b = zeros([2, 3], "f32")
  for (let i = 0; i < 6; i = i + 1) { b[i] = i * 1.5 }
  return [shape(b), dtype(b), to_list(b), sum(b)]
}
print(f())
EOF

prog "deep call chains and mutual recursion" <<'EOF'
fn even(n) { if (n == 0) { return true } return odd(n - 1) }
fn odd(n) { if (n == 0) { return false } return even(n - 1) }
print([even(100), odd(77)])
fn depth(n) { if (n == 0) { return 0 } return 1 + depth(n - 1) }
print(depth(500))
EOF

# Indexing straight off a call result compiles to an instruction whose
# destination register is also its object register. Getting that wrong frees
# the list before reading the element -- it produced denormal garbage for small
# inputs and a segfault for large ones.
prog "indexing a call result" <<'EOF'
fn mk() { return [10, 20, 30] }
fn dd() { return {"k": [1, 2, 3]} }
fn cmp(a, b) { return a < b }
fn big() { let xs = [] for (let i = 0; i < 200; i = i + 1) { xs.push(i * 3) } return xs }
print(mk()[0])
print(mk()[1] + mk()[2])
print(dd()["k"][2])
print(sort([3, 1, 2], cmp)[0])
print([[1, 2], [3, 4]][1][0])
fn run() {
  let s = 0
  for (let r = 0; r < 500; r = r + 1) { s = s + big()[199] + big()[0] }
  return s
}
print(run())
fn slice_of_call() { return mk()[1:] }
print(slice_of_call())
fn prop_of_call() { let xs = mk() return xs.len() }
print(prop_of_call())
EOF

prog "compound assignment" <<'EOF'
fn f() {
  let xs = [1, 2, 3]
  let d = {"a": 10}
  let n = 5
  n += 2  n -= 1  n *= 3  n /= 2
  xs[0] += 10  xs[1] *= 4  xs[2] -= 1
  d["a"] += 5  d["b"] = 1  d["b"] *= 7
  return [n, xs, d]
}
print(f())
fn bad() { let xs = [1] xs[0] += "s" return xs }
try { print(bad()) } catch (e) { print("caught: " + e) }
fn oob() { let xs = [1] xs[5] += 1 return xs }
try { print(oob()) } catch (e) { print("caught: " + e) }
EOF

prog "deeper inheritance and super chains" <<'EOF'
class A { fn init(v) { this.v = v } fn who() { return "A" } fn tag() { return "<" + this.who() + ">" } }
class B extends A { fn init(v) { super.init(v * 2) } fn who() { return "B" + super.who() } }
class C extends B { fn init(v) { super.init(v + 1) } fn who() { return "C" + super.who() } }
let c = new C(3)
print([c.v, c.who(), c.tag()])
EOF

prog "list comprehensions in functions" <<'EOF'
fn f(xs) { return [x * 2 for x in xs] }
fn g(xs) { return [x for x in xs if x % 3 == 0] }
fn h(s) { return [c + "!" for c in s] }
fn nested(n) { return [[j for j in [1,2,3] if j < i] for i in [1,2,3,4]] }
print(f([1,2,3])) print(g([1,3,6,7,9])) print(h("abc")) print(nested(3))
fn d(m) { return [k for k in m] }
print(d({"b": 1, "a": 2}))
EOF

prog "string interpolation" <<'EOF'
fn f(name, n) { return f"{name} has {n * 2} items and {n > 1 ? "many" : "one"}" }
print(f("bee", 3)) print(f("ant", 1))
EOF

# JIT parity: the native tier must produce byte-identical results to the
# interpreter. The differential cases above cannot see JIT bugs -- both the VM
# and the tree-walker share the same JIT, so a miscompile fails both identically.
# These force the interpreter with BEE_NO_JIT=1 as the reference. The loops run
# >40k trips so the loop JIT actually engages. Regression guard: a hot top-level
# loop that called a function crashed the loop JIT (its bail flag was read from
# the wrong argument), and a mid-loop bail dereferenced a garbage pointer.
echo "jit parity: hot loops vs BEE_NO_JIT"

jitcheck() {                               # jitcheck <name>  (program on stdin)
    local name=$1
    cat > "$tmp/j.bee"
    BEE_NO_JIT=1 $BEE "$tmp/j.bee" >"$tmp/a.out" 2>"$tmp/a.err"; local ra=$?
    $BEE "$tmp/j.bee" >"$tmp/b.out" 2>"$tmp/b.err"; local rb=$?
    if [ "$ra" != "$rb" ]; then
        echo "  FAIL $name: exit $ra (no jit) vs $rb (jit)"; fail=$((fail+1)); return
    fi
    if ! diff -q "$tmp/a.out" "$tmp/b.out" >/dev/null; then
        echo "  FAIL $name: stdout differs"; diff "$tmp/a.out" "$tmp/b.out" | head -6; fail=$((fail+1)); return
    fi
    if ! diff -q "$tmp/a.err" "$tmp/b.err" >/dev/null; then
        echo "  FAIL $name: stderr differs"; diff "$tmp/a.err" "$tmp/b.err" | head -6; fail=$((fail+1)); return
    fi
    echo "  ok   $name"; pass=$((pass+1))
}

jitcheck "hot loop calls a function" <<'EOF'
fn add(a, b) { return a + b }
let t = 0
for (let i = 0; i < 60000; i = i + 1) { t = add(t, 1) }
print(t)
EOF

jitcheck "hot loop calls a modulo helper" <<'EOF'
fn m(a, b) { return a % b }
let s = 0
for (let i = 1; i < 60000; i = i + 1) { s = s + m(i, 7) }
print(s)
EOF

jitcheck "hot loop hits a bail (division by zero)" <<'EOF'
let s = 0
for (let i = 1; i < 60000; i = i + 1) { s = s + 100 / (i - 30000) }
print(s)
EOF

echo
if [ "$fail" -eq 0 ]; then
    echo "all $pass differential checks passed"
else
    echo "$fail of $((pass+fail)) differential checks FAILED"
    exit 1
fi
