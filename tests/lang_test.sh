#!/usr/bin/env bash
#
# Language-level tests for the `bee` interpreter, focused on diagnostics: every
# error should say what went wrong, in which file, on which line, and how the
# program got there.
#
# Run from anywhere:  bash tests/lang_test.sh
# Requires `bee` to be built already (`make`).
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BEE="$ROOT/bee"
[ -x "$BEE" ] || { echo "tests: $BEE not built -- run 'make' first" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cd "$WORK" || exit 1

PASS=0
FAIL=0
ok()  { PASS=$((PASS+1)); printf '  ok   %s\n' "$1"; }
bad() { FAIL=$((FAIL+1)); printf '  FAIL %s\n' "$1"; [ -n "${2:-}" ] && printf '       %s\n' "$2"; }

# want <name> <expected-substring> -- reads the program from stdin, runs it,
# and checks the combined output. Multi-line expectations are fine. Runs of
# spaces collapse on both sides, because a trace pads function names to the
# longest one in that particular trace.
want() {
    local name="$1" expect="$2" src out raw
    src="$(cat)"
    printf '%s\n' "$src" > prog.bee
    raw="$("$BEE" prog.bee 2>&1)"
    out="$(printf '%s' "$raw" | sed 's/  */ /g')"
    expect="$(printf '%s' "$expect" | sed 's/  */ /g')"
    if [[ "$out" == *"$expect"* ]]; then ok "$name"
    else bad "$name" "expected '$expect' in:
$(printf '%s' "$raw" | sed 's/^/         | /')"; fi
}

echo "traces: locations"
want "reports the file and line" "  at <main>  prog.bee:2" <<'EOF'
let a = 1
print(undefinedThing)
EOF

want "names the failing function" "  at boom()  prog.bee:1" <<'EOF'
fn boom() { return nil + 1 }
boom()
EOF

# Each frame on its own line, innermost first, with the line the call was on.
want "shows the whole call chain" "at inner()   prog.bee:1" <<'EOF'
fn inner()  { return nil + 1 }
fn middle() { return inner() }
fn outer()  { return middle() }
outer()
EOF
want "each frame shows where it was executing" "at outer() prog.bee:3" <<'EOF'
fn inner()  { return nil + 1 }
fn middle() { return inner() }
fn outer()  { return middle() }
outer()
EOF

want "reports methods by name" "at Duck.quack()" <<'EOF'
class Duck {
    quack() { return nil + 1 }
}
new Duck().quack()
EOF

want "reports anonymous functions" "at <anonymous>()" <<'EOF'
let f = fn() { return nil + 1 }
f()
EOF

echo "traces: across files"
mkdir -p pkg
cat > pkg/util.bee <<'EOF'
fn broken() { return nil + 1 }
fn wrapper() { return broken() }
EOF
want "points inside the imported module" "pkg/util.bee:1" <<'EOF'
import pkg.util
util.wrapper()
EOF
want "keeps the caller's file in the chain" "at <main> prog.bee:2" <<'EOF'
import pkg.util
util.wrapper()
EOF

echo "traces: built-ins and throws"
want "built-in errors get a location" "at load()  prog.bee:1" <<'EOF'
fn load() { return read_file("/nonexistent/nope.txt") }
load()
EOF

want "errors inside a callback keep their own trace" "at bad()" <<'EOF'
fn bad(x) { return x + nil }
print(map(bad, [1, 2]))
EOF

want "uncaught throws carry a trace" "Uncaught: boom
  at raise()" <<'EOF'
fn raise() { throw "boom" }
raise()
EOF

want "caught throws stay silent" "caught boom" <<'EOF'
fn raise() { throw "boom" }
try { raise() } catch (e) { print("caught " + e) }
EOF

# A handler usually prints the error inside a message of its own, so the value
# it binds must stay one line -- while still saying where the fault was.
want "caught errors are one line with a location" \
    "handled: Runtime error: division by zero (prog.bee:1)" <<'EOF'
fn div(a, b) { return a / b }
try { div(1, 0) } catch (e) { print("handled: " + e) }
EOF
want "caught errors carry no trace" "no trace" <<'EOF'
try { nil + 1 } catch (e) {
    if e.contains("  at ") { print("trace leaked") } else { print("no trace") }
}
EOF

echo "diagnostics: parse and lex errors"
want "parse errors name the file" "Parse error: expected expression
  at prog.bee:" <<'EOF'
let x = (1 +
EOF

cat > pkg/broken.bee <<'EOF'
fn f() { return ( }
EOF
want "parse errors in a module name that module" "at pkg/broken.bee:1" <<'EOF'
import pkg.broken
EOF

want "unterminated strings are reported" "Lex error" <<'EOF'
let s = "no closing quote
EOF

echo "diagnostics: runaway recursion"
cat > deep.bee <<'EOF'
fn f(s) { return f(s + "x") }
f("a")
EOF
out="$("$BEE" deep.bee 2>&1)"; status=$?
if [ $status -eq 139 ]; then
    bad "unbounded recursion doesn't crash" "the process died on a signal"
elif [[ "$out" == *"call stack overflow"* ]]; then ok "unbounded recursion doesn't crash"
else bad "unbounded recursion doesn't crash" "$out"; fi
[[ "$out" == *"more frames ..."* ]] && ok "deep traces are truncated" || bad "deep traces are truncated"

cat > bounded.bee <<'EOF'
fn countdown(n, tag) { if n <= 0 { return tag } return countdown(n - 1, tag) }
print(countdown(1200, "deep recursion ok"))
EOF
out="$("$BEE" bounded.bee 2>&1)"
[[ "$out" == "deep recursion ok" ]] && ok "legitimate deep recursion still runs" \
    || bad "legitimate deep recursion still runs" "$out"

out="$(BEE_MAX_DEPTH=50 "$BEE" bounded.bee 2>&1)"
[[ "$out" == *"deeper than 50 nested calls"* ]] && ok "BEE_MAX_DEPTH sets the limit" \
    || bad "BEE_MAX_DEPTH sets the limit" "$out"

echo "strings: interpolation"
want "interpolates variables" "n=5 name=Ada" <<'EOF'
let n = 5
let name = "Ada"
print(f"n={n} name={name}")
EOF
want "interpolates expressions" "5 squared is 25" <<'EOF'
let n = 5
print(f"{n} squared is {n * n}")
EOF
want "allows quotes inside braces" "val and 1" <<'EOF'
let d = {"key": "val"}
let xs = [1, 2]
print(f"{d["key"]} and {xs[0]}")
EOF
want "allows method calls inside braces" "ADA" <<'EOF'
print(f"{"ada".upper()}")
EOF
want "doubles braces for literals" "{literal} }" <<'EOF'
print(f"{{literal}} }}")
EOF
want "stringifies non-strings" "n is 3 xs is [1, 2]" <<'EOF'
print(f"n is {1 + 2} xs is {[1, 2]}")
EOF
want "leaves plain strings alone" "braces {n} stay" <<'EOF'
let n = 1
print("braces {n} stay")
EOF
want "leaves f as an identifier alone" "called f" <<'EOF'
fn f(s) { return "called " + s }
print(f ("f"))
EOF
want "rejects empty braces" "empty '{}'" <<'EOF'
print(f"nothing here: {}")
EOF
want "reports errors inside braces" "in interpolated expression" <<'EOF'
print(f"{1 +}")
EOF
want "reports an unclosed brace" "is a '{' or a quote unclosed?" <<'EOF'
print(f"{oops")
EOF

echo "collections: slicing"
want "slices a list" "[2, 3]" <<'EOF'
print([1, 2, 3, 4, 5][1:3])
EOF
want "omits the start" "[1, 2]" <<'EOF'
print([1, 2, 3][:2])
EOF
want "omits the end" "[3, 4]" <<'EOF'
print([1, 2, 3, 4][2:])
EOF
want "copies with a bare colon" "[1, 2]" <<'EOF'
print([1, 2][:])
EOF
want "counts negative bounds from the end" "[4, 5]" <<'EOF'
print([1, 2, 3, 4, 5][-2:])
EOF
want "clamps out-of-range bounds" "[1, 2]" <<'EOF'
print([1, 2][0:100])
EOF
want "returns empty for an inverted range" "[]" <<'EOF'
print([1, 2, 3][3:1])
EOF
want "slices strings" "hello" <<'EOF'
print("hello world"[0:5])
EOF
want "slices strings from the end" "world" <<'EOF'
print("hello world"[-5:])
EOF
want "copies rather than aliases" "[1, 2, 3] / [9, 2]" <<'EOF'
let xs = [1, 2, 3]
let part = xs[0:2]
part[0] = 9
print(str(xs) + " / " + str(part))
EOF
want "rejects non-number bounds" "slice bounds must be numbers" <<'EOF'
print([1, 2]["a":])
EOF
want "rejects slicing a dict" "only lists and strings can be sliced" <<'EOF'
print({"a": 1}[0:1])
EOF
want "indexing still works" "2" <<'EOF'
print([1, 2, 3][1])
EOF

echo "entry points: -e, stdin, repl"
out="$("$BEE" -e 'print(f"eval {1 + 1}")' 2>&1)"
[[ "$out" == "eval 2" ]] && ok "-e runs code" || bad "-e runs code" "$out"
out="$(printf 'let n = 21\nprint(n * 2)\n' | "$BEE" 2>&1)"
[[ "$out" == "42" ]] && ok "a program can come from stdin" || bad "a program can come from stdin" "$out"
out="$("$BEE" -e 'print(nosuch)' 2>&1)"
[[ "$out" == *"at <main>  <eval>:1"* ]] && ok "-e errors name their source" || bad "-e errors name their source" "$out"

# The REPL only starts on a terminal, so drive it through a pty.
if command -v script >/dev/null 2>&1; then
    repl_out="$(script -qec "$BEE" /dev/null <<'REPLEOF' 2>&1
1 + 1
let x = 10
x * 3
f"x is {x}"
fn fact(n) {
    if n <= 1 { return 1 }
    return n * fact(n - 1)
}
fact(5)
nosuchname
"still alive"
exit
REPLEOF
)"
    [[ "$repl_out" == *">>> 2"* ]]   && ok "repl echoes expression values"  || bad "repl echoes expression values" "$repl_out"
    [[ "$repl_out" == *"30"* ]]      && ok "repl keeps state across lines"  || bad "repl keeps state across lines"
    [[ "$repl_out" == *"\"x is 10\""* ]] && ok "repl handles f-strings"   || bad "repl handles f-strings"
    [[ "$repl_out" == *"... "* ]]    && ok "repl continues an open block"   || bad "repl continues an open block"
    [[ "$repl_out" == *"120"* ]]     && ok "repl defines and calls functions" || bad "repl defines and calls functions"
    [[ "$repl_out" == *"undefined variable 'nosuchname'"* && "$repl_out" == *'"still alive"'* ]] \
        && ok "repl survives an error" || bad "repl survives an error"
else
    echo "  skip repl checks (no 'script' command for a pty)"
fi

echo "diagnostics: exit codes"
"$BEE" missing-file.bee >/dev/null 2>&1
[ $? -eq 70 ] && ok "a missing script exits 70" || bad "a missing script exits 70"
printf 'print("hi")\n' > fine.bee
"$BEE" fine.bee >/dev/null 2>&1
[ $? -eq 0 ] && ok "a clean run exits 0" || bad "a clean run exits 0"

echo
if [ "$FAIL" -eq 0 ]; then
    echo "all $PASS checks passed"
    exit 0
fi
echo "$PASS passed, $FAIL failed"
exit 1
