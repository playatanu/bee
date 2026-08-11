#!/usr/bin/env bash
#
# AOT compiler (beec) end-to-end tests: compile Bee programs to native
# executables and check their output matches the interpreter's exactly.
#
set -u
cd "$(dirname "$0")/.."

BEE=./bee
BEEC=./beec
if [ ! -x "$BEEC" ]; then echo "aot_test: ./beec not built (run make)"; exit 1; fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

pass=0; fail=0

# check <name> <bee-source>
check() {
    local name="$1" src="$2"
    local f="$TMP/$name.bee"
    printf '%s\n' "$src" > "$f"
    local iout aout
    iout=$("$BEE" "$f" 2>&1); local irc=$?
    if ! "$BEEC" "$f" -o "$TMP/$name" >/dev/null 2>&1; then
        echo "  fail  $name (beec failed to compile)"; fail=$((fail+1)); return
    fi
    aout=$("$TMP/$name" 2>&1); local arc=$?
    if [ "$iout" = "$aout" ] && [ "$irc" -eq "$arc" ]; then
        echo "  ok    $name"; pass=$((pass+1))
    else
        echo "  fail  $name (interp rc=$irc aot rc=$arc)"
        diff <(printf '%s' "$iout") <(printf '%s' "$aout") | head -6
        fail=$((fail+1))
    fi
}

# check_unsupported <name> <bee-source>: beec should refuse, exit non-zero.
check_unsupported() {
    local name="$1" src="$2"
    local f="$TMP/$name.bee"
    printf '%s\n' "$src" > "$f"
    if "$BEEC" "$f" -o "$TMP/$name" >/dev/null 2>&1; then
        echo "  fail  $name (expected beec to reject it)"; fail=$((fail+1))
    else
        echo "  ok    $name (correctly rejected)"; pass=$((pass+1))
    fi
}

# check_exact <name> <bee-source> <expected-output>: the compiled binary must
# print exactly <expected>. Used where AOT is deliberately more precise than the
# interpreter -- sized integers are exact two's-complement in a native binary,
# whereas the interpreter computes through double and rounds past 2^53.
check_exact() {
    local name="$1" src="$2" want="$3"
    local f="$TMP/$name.bee"
    printf '%s\n' "$src" > "$f"
    if ! "$BEEC" "$f" -o "$TMP/$name" >/dev/null 2>&1; then
        echo "  fail  $name (beec failed to compile)"; fail=$((fail+1)); return
    fi
    local aout; aout=$("$TMP/$name" 2>&1)
    if [ "$aout" = "$want" ]; then
        echo "  ok    $name"; pass=$((pass+1))
    else
        echo "  fail  $name (want '$want' got '$aout')"; fail=$((fail+1))
    fi
}

echo "beec: native output matches the interpreter"
check hello        'print("Hello, " + "world" + "!")'
check arithmetic   'print(7 + 2 * 3, 7 % 3, -5, 2.5 * 4)'
check strings      'let s="hello"; print(s.upper(), s[1:3], s[-1], len(s))'
check interp       'let n=5; print(f"{n} squared is {n*n}")'
check lists        'let x=[3,1,2]; x.push(4); print(x, x.pop(), x.contains(2))'
check dicts        'let d={"a":1}; d["b"]=2; print(d["a"], d.keys(), d.has("b"))'
check control      $'let t = 0\nfor (let i = 0; i < 5; i = i + 1) {\n  if i == 2 { continue }\n  t += i\n}\nprint(t)'
check forin        $'for c in "abc" { write(c) }\nprint("")'
check closures     $'fn c() {\n  let n = 0\n  fn t() { n += 1; return n }\n  return t\n}\nlet f = c()\nprint(f(), f(), f())'
check recursion    $'fn fib(n) {\n  if n < 2 { return n }\n  return fib(n-1) + fib(n-2)\n}\nprint(fib(15))'
check comprehension 'print([x*x for x in range(5) if x % 2 == 0])'
check errors       'try { throw {"m":"boom"} } catch(e) { print("caught", e.m) } finally { print("fin") }'
check builtins     'print(abs(-3), sqrt(9), max([2,5,1]), type([]), str(42))'
check classes      $'class Animal {\n  init(n) { this.name = n }\n  speak() { return this.name + " makes a sound" }\n  str() { return "A(" + this.name + ")" }\n}\nclass Dog extends Animal {\n  speak() { return super.speak() + ": woof" }\n}\nlet d = Dog("Rex")\nprint(d.speak(), d.name, d)'
check match        $'fn f(n) {\n  match n {\n    case 0 { return "zero" }\n    case 1, 2, 3 { return "small" }\n    default { return "big" }\n  }\n}\nprint(f(0), f(2), f(99))'
check destructure  $'let [a, b, c] = [1, 2, 3]\nlet {x, y} = {"x": 10, "y": 20}\nprint(a, b, c, x, y)'

# Optimisations: cached global slots, native range() loops, inline numeric ops.
# These must stay behaviour-identical to the interpreter on the tricky cases.
check globals_hot  $'let total = 0\nfor i in range(1000) { for j in range(1000) { total = total + 1 } }\nprint(total)'
check range_desc   'for i in range(10, 0, -2) { write(str(i) + " ") } print("")'
check range_float  'for i in range(0.0, 1.0, 0.25) { write(str(i) + " ") } print("")'
check range_empty  $'for i in range(5, 2) { print("no") }\nprint("empty ok")'
check range_stepzero 'for i in range(1, 5, 0) { print(i) }'   # errors identically
check range_nonnum 'for i in range("a") { print(i) }'         # errors identically
check range_shadow $'fn range(n) { return [7, 8] }\nfor i in range(3) { print(i) }'
check range_closure $'let fns = []\nfor i in range(3) { push(fns, fn() { return i }) }\nfor f in fns { print(f()) }'
check num_ops      'print(7 / 2, 7 % 3, 2 < 3, 3 <= 3, 5 & 3, 1 << 4)'
check div_zero     'print(1 / 0)'                             # errors identically
check mixed_ops    'print("a" + "b", [1] + [2], "hi" * 2)'    # non-numeric fall back

# Sized numeric types (i8..u64, f16/f32/f64): the AOT output must wrap/round
# exactly as the tree-walker does at every typed store.
check sized_wrap_u8  $'let a: u8 = 300\nlet b: u8 = 255 + 1\nprint(a, b)'
check sized_wrap_i8  $'let a: i8 = 200\nlet b: i8 = 127 + 1\nprint(a, b)'
check sized_assign   $'let x: i32 = 0\nx = 4294967296 + 5\nprint(x)'
check sized_trunc    $'let s: i8 = 5.9\nlet t: i8 = -5.9\nlet u: u8 = -1\nprint(s, t, u)'
check sized_param    $'fn w(n: u8): u8 { return n }\nprint(w(258), w(-1))'
check sized_return   $'fn f(): i16 { return 100000 }\nprint(f())'
check sized_float    $'let f: f32 = 1.0 / 3.0\nprint(f)'
check sized_half     $'let h: f16 = 1.0 / 3.0\nlet k: f16 = 65504.0\nprint(h, k)'
check sized_loop     $'let acc: u8 = 0\nfor i in range(1000) { acc = acc + 1 }\nprint(acc)'
# Native-local storage (Phase 2b) must stay behaviour-identical, including the
# escape case: a sized local captured by a closure must keep shared mutation.
check sized_native   $'fn r(): i32 {\n  let acc: i32 = 0\n  for i in range(100000) { acc = acc + 1 }\n  return acc\n}\nprint(r())'
check sized_capture  $'fn r() {\n  let acc: i32 = 10\n  let get = fn() { return acc }\n  acc = acc + 5\n  return get()\n}\nprint(r())'
check sized_native_f $'fn r(): f32 {\n  let s: f32 = 0.0\n  for i in range(10) { s = s + 0.1 }\n  return s\n}\nprint(r())'
check sized_native_dyn $'fn dbl(x) { return x * 2 }\nfn r() {\n  let a: u8 = 200\n  a = a + 100\n  return dbl(a)\n}\nprint(r())'
# Native comparison path (conditions over sized-numeric locals compile to native
# C++ comparisons). The nested loop with an == guard exercises for/if conditions.
check native_cmp     $'fn r(): i64 {\n  let sum: i64 = 0\n  for (let i: i64 = 0; i < 300; i += 1) {\n    for (let j: i64 = 0; j < 300; j += 1) {\n      if i == j { sum += i * j }\n    }\n  }\n  return sum\n}\nprint(r())'
# &&/||/! over native comparisons, plus <=/>= boundaries, must match the runtime.
check native_cmp_bool $'fn r(): i32 {\n  let n: i32 = 0\n  for (let i: i32 = 0; i <= 20; i += 1) {\n    if i >= 5 and i <= 15 and not (i == 10) { n += 1 }\n  }\n  return n\n}\nprint(r())'
# NaN edge: Bee treats unordered <=/>= as true (unlike native C++), and the AOT
# comparison path must preserve that. inf-inf builds a NaN through native floats.
check native_cmp_nan $'fn r() {\n  let a: f64 = 1.0\n  for (let k = 0; k < 400; k += 1) { a = a * 10.0 }\n  let nan: f64 = a - a\n  let x: f64 = 5.0\n  print(nan < x, nan > x, nan <= x, nan >= x, nan == nan, nan != nan)\n}\nr()'

echo
echo "beec: sized integers are exact two's-complement (native binary), even where"
echo "      the interpreter's double arithmetic would round past 2^53"
# i32/u32 products whose exact result exceeds 2^53: the compiled binary wraps the
# exact integer product; the interpreter rounds through double first, so these are
# pinned to the exact value rather than compared against the interpreter.
check_exact int_exact_i32 $'fn r(): i32 {\n  let a: i32 = 2000000000\n  return a * a\n}\nprint(r())' '-1651507200'
check_exact int_exact_u32 $'fn r(): u32 {\n  let a: u32 = 4000000000\n  return a * a\n}\nprint(r())' '1983905792'
echo "beec: unresolvable / unsupported imports are refused, not miscompiled"
check_unsupported missing_module 'import definitely_not_a_real_module_xyz'

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
