#!/usr/bin/env bash
#
# Complexity guards, not micro-benchmarks.
#
# Each case is an idiom that has a linear implementation and a quadratic one
# that is easy to fall back into -- string building lost its in-place append
# when function bodies moved to the bytecode VM, and went from 0.06 s to 363 s
# without a single test noticing, because every test only checked the answer.
#
# The budgets are deliberately loose (10x-plus headroom on a slow machine). A
# failure here means the complexity changed, not that something got 20% slower;
# use bench/run.sh for that.
#
set -u
cd "$(dirname "$0")/.."
BEE=./bee
pass=0; fail=0
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT

guard() {                                  # guard <name> <budget-seconds> <expected-output>
    local name=$1 budget=$2 want=$3
    cat > "$tmp/p.bee"
    local start end elapsed got
    start=$(date +%s.%N)
    got=$($BEE "$tmp/p.bee" 2>&1)
    end=$(date +%s.%N)
    elapsed=$(awk -v a="$start" -v b="$end" 'BEGIN{printf "%.2f", b-a}')
    if [ "$got" != "$want" ]; then
        echo "  FAIL $name: wrong result: $got (wanted $want)"; fail=$((fail+1)); return
    fi
    if awk -v e="$elapsed" -v b="$budget" 'BEGIN{exit !(e > b)}'; then
        echo "  FAIL $name: took ${elapsed}s, budget ${budget}s -- complexity regression?"
        fail=$((fail+1)); return
    fi
    echo "  ok   $name (${elapsed}s, budget ${budget}s)"; pass=$((pass+1))
}

echo "complexity guards"

# Quadratic if `s = s + x` reallocates instead of growing in place: ~200 s.
guard "string append stays linear" 5 "3000000" <<'EOF'
fn run(n) {
  let s = ""
  for (let i = 0; i < n; i = i + 1) { s = s + "abcdefghij" }
  return len(s)
}
print(run(300000))
EOF

# Same idiom at the top level, which is the tree-walker's own fast path.
guard "top-level string append stays linear" 5 "2000000" <<'EOF'
let s = ""
for (let i = 0; i < 200000; i = i + 1) { s += "abcdefghij" }
print(len(s))
EOF

# Quadratic if push copies the list each time.
guard "list push stays linear" 5 "500000" <<'EOF'
fn run(n) {
  let xs = []
  for (let i = 0; i < n; i = i + 1) { xs.push(i) }
  return len(xs)
}
print(run(500000))
EOF

# Quadratic if a dict copies on write.
guard "dict insert stays linear" 5 "200000" <<'EOF'
fn run(n) {
  let d = {}
  for (let i = 0; i < n; i = i + 1) { d[str(i)] = i }
  return len(d)
}
print(run(200000))
EOF

# Quadratic if `+=` on a list element re-reads or rebuilds the list.
guard "index compound assignment stays linear" 5 "1000000" <<'EOF'
fn run(n) {
  let xs = []
  for (let i = 0; i < 1000; i = i + 1) { xs.push(0) }
  for (let i = 0; i < n; i = i + 1) { xs[i % 1000] += 1 }
  let s = 0
  for x in xs { s = s + x }
  return s
}
print(run(1000000))
EOF

echo
if [ "$fail" -eq 0 ]; then
    echo "all $pass complexity guards passed"
else
    echo "$fail of $((pass+fail)) complexity guards FAILED"
    exit 1
fi
