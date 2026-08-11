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

echo
echo "beec: unresolvable / unsupported imports are refused, not miscompiled"
check_unsupported missing_module 'import definitely_not_a_real_module_xyz'

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
