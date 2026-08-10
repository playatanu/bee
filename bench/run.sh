#!/usr/bin/env bash
#
# Bee's benchmark runner.
#
#   bench/run.sh                          # time the current ./bee
#   bench/run.sh --vs /path/to/old/bee    # and compare against another build
#   bench/run.sh --python                 # and against CPython, where an
#                                         # equivalent .py exists
#   bench/run.sh --go                     # and against Go, likewise. Each .go
#                                         # is compiled once, up front, so what
#                                         # is timed is the program and not the
#                                         # Go compiler.
#   bench/run.sh loop_ call_              # only programs matching a prefix
#
# Every contender runs interleaved, round by round, and each keeps its *best*
# time. That matters more than it sounds: on a laptop the CPU's boost state
# drifts as a run heats it up, and back-to-back suites can differ by 2x for
# reasons that have nothing to do with the code. Interleaving makes both builds
# see the same conditions; best-of-N drops the samples where something else on
# the machine got in the way.
#
set -u
cd "$(dirname "$0")/.."

BEE=./bee
REF=""
PYTHON=0
GO=0
RUNS=5
FILTERS=()

while [ $# -gt 0 ]; do
    case "$1" in
        --vs)     REF="$2"; shift 2;;
        --python) PYTHON=1; shift;;
        --go)     GO=1; shift;;
        --all)    PYTHON=1; GO=1; shift;;
        -n)       RUNS="$2"; shift 2;;
        --bee)    BEE="$2"; shift 2;;
        -h|--help)
            sed -n '2,22p' "$0" | sed 's/^# \?//'
            exit 0;;
        *)        FILTERS+=("$1"); shift;;
    esac
done

[ -x "$BEE" ] || { echo "no interpreter at $BEE -- run make first" >&2; exit 1; }
if [ -n "$REF" ] && [ ! -x "$REF" ]; then
    echo "reference build $REF is not executable" >&2; exit 1
fi

matches() {                                  # matches <name>
    [ ${#FILTERS[@]} -eq 0 ] && return 0
    local f
    for f in "${FILTERS[@]}"; do case "$1" in $f*) return 0;; esac; done
    return 1
}

progs=()
for f in bench/programs/*.bee; do
    name=$(basename "$f" .bee)
    matches "$name" && progs+=("$name")
done
[ ${#progs[@]} -gt 0 ] || { echo "no benchmarks matched" >&2; exit 1; }

# Go is compiled ahead of the timing loop -- the comparison is against the
# program Go produces, not against the Go compiler.
gobin=""
if [ "$GO" = 1 ]; then
    if ! command -v go >/dev/null; then
        echo "no go toolchain on PATH -- skipping Go" >&2
        GO=0
    else
        gobin=$(mktemp -d); trap 'rm -rf "$gobin"' EXIT
        built=0
        for p in "${progs[@]}"; do
            [ -f "bench/programs/$p.go" ] || continue
            if go build -o "$gobin/$p" "bench/programs/$p.go" 2>"$gobin/err"; then
                built=$((built + 1))
            else
                echo "  go build failed for $p:"; sed 's/^/    /' "$gobin/err"
            fi
        done
        echo "compiled $built Go programs"
    fi
fi

# One timed run, in seconds to three decimals. Output is discarded but a
# non-zero exit is not: a benchmark that errors would otherwise look fast.
#
# Timed here rather than with /usr/bin/time, whose two decimals bottom out at
# 0.00s -- which is exactly where a compiled Go program lands on work that takes
# Bee a quarter of a second, and printing 0.00 tells you nothing about how
# far apart they are.
time_one() {
    local start end
    start=$(date +%s%N)
    if ! "$@" >/dev/null 2>&1; then echo "FAILED"; return; fi
    end=$(date +%s%N)
    awk -v s="$start" -v e="$end" 'BEGIN{printf "%.3f", (e-s)/1000000000}'
}

declare -A cur ref py go
for p in "${progs[@]}"; do cur[$p]=999999; ref[$p]=999999; py[$p]=999999; go[$p]=999999; done

# Every measurement includes the runtime's own startup, and the runtimes differ
# by an order of magnitude: bee links libLLVM, which costs ~12 ms of dynamic
# linking before a line of Bee runs, against ~12 ms for CPython and ~1 ms for a
# Go binary. On a benchmark that does 10 ms of work that is not a rounding
# error, it is most of the number -- and comparing totals would say more about
# process startup than about either language. So each runtime's floor is
# measured once and reported, and the `work` column has it subtracted.
mkdir -p "$(dirname "$0")/.tmp"
noop_bee="$(dirname "$0")/.tmp/noop.bee"
echo 'print(1)' > "$noop_bee"
floor() { local b=999999 t; for _ in 1 2 3 4 5; do
    t=$(time_one "$@"); [ "$t" = FAILED ] && { echo 0; return; }
    b=$(awk -v a="$t" -v b="$b" 'BEGIN{print (a<b)?a:b}'); done; echo "$b"; }
beeFloor=$(floor "$BEE" "$noop_bee")
refFloor=0; [ -n "$REF" ] && refFloor=$(floor "$REF" "$noop_bee")
pyFloor=0;  [ "$PYTHON" = 1 ] && pyFloor=$(floor python3 -c 'print(1)')
goFloor=0
if [ "$GO" = 1 ]; then
    goFloor=0.001   # a static Go binary's exec cost, below this timer's noise
fi
printf 'startup floors: bee %ss' "$beeFloor"
[ -n "$REF" ] && printf ', ref %ss' "$refFloor"
[ "$PYTHON" = 1 ] && printf ', python %ss' "$pyFloor"
[ "$GO" = 1 ] && printf ', go ~%ss' "$goFloor"
echo

echo "running ${#progs[@]} benchmarks, best of $RUNS, interleaved"
for ((round = 1; round <= RUNS; round++)); do
    printf '  round %d/%d\r' "$round" "$RUNS"
    for p in "${progs[@]}"; do
        t=$(time_one "$BEE" "bench/programs/$p.bee")
        [ "$t" = FAILED ] && cur[$p]=FAILED || \
            cur[$p]=$(awk -v a="$t" -v b="${cur[$p]}" 'BEGIN{print (a<b)?a:b}')
        if [ -n "$REF" ]; then
            t=$(time_one "$REF" "bench/programs/$p.bee")
            [ "$t" = FAILED ] && ref[$p]=FAILED || \
                ref[$p]=$(awk -v a="$t" -v b="${ref[$p]}" 'BEGIN{print (a<b)?a:b}')
        fi
        if [ "$PYTHON" = 1 ] && [ -f "bench/programs/$p.py" ]; then
            t=$(time_one python3 "bench/programs/$p.py")
            [ "$t" = FAILED ] && py[$p]=FAILED || \
                py[$p]=$(awk -v a="$t" -v b="${py[$p]}" 'BEGIN{print (a<b)?a:b}')
        fi
        if [ "$GO" = 1 ] && [ -x "$gobin/$p" ]; then
            t=$(time_one "$gobin/$p")
            [ "$t" = FAILED ] && go[$p]=FAILED || \
                go[$p]=$(awk -v a="$t" -v b="${go[$p]}" 'BEGIN{print (a<b)?a:b}')
        fi
    done
done
printf '                    \r'

# Time with the runtime's startup floor removed, never below zero.
net() {                                      # net <time> <floor>
    case "$1" in *FAILED*) echo FAILED; return;; esac
    awk -v t="$1" -v f="$2" 'BEGIN{ d = t - f; printf "%.3f", (d > 0) ? d : 0 }'
}
ratio() {                                    # ratio <numerator> <denominator>
    case "$1$2" in *FAILED*) echo "-"; return;; esac
    awk -v a="$1" -v b="$2" 'BEGIN{ if (b+0 == 0) print "-"; else printf "%.2f", a/b }'
}

header="%-16s %9s %9s"
row="%-16s %8ss %8ss"
cols=(benchmark bee work)
[ -n "$REF" ] && { header="$header %9s %8s"; row="$row %8ss %7sx"; cols+=(ref speedup); }
[ "$PYTHON" = 1 ] && { header="$header %9s %8s"; row="$row %8ss %7sx"; cols+=(python vs-py); }
[ "$GO" = 1 ] && { header="$header %9s %8s"; row="$row %8ss %7sx"; cols+=(go vs-go); }
# shellcheck disable=SC2059
printf "$header\n" "${cols[@]}"

for p in "${progs[@]}"; do
    beeNet=$(net "${cur[$p]}" "$beeFloor")
    args=("$p" "${cur[$p]}" "$beeNet")
    [ -n "$REF" ] && args+=("${ref[$p]}" "$(ratio "$(net "${ref[$p]}" "$refFloor")" "$beeNet")")
    if [ "$PYTHON" = 1 ]; then
        if [ "${py[$p]}" = 999999 ]; then args+=("-" "-")
        else args+=("${py[$p]}" "$(ratio "$beeNet" "$(net "${py[$p]}" "$pyFloor")")"); fi
    fi
    if [ "$GO" = 1 ]; then
        if [ "${go[$p]}" = 999999 ]; then args+=("-" "-")
        else args+=("${go[$p]}" "$(ratio "$beeNet" "$(net "${go[$p]}" "$goFloor")")"); fi
    fi
    # shellcheck disable=SC2059
    printf "$row\n" "${args[@]}"
done

if [ -n "$REF" ]; then
    echo
    awk -v n="${#progs[@]}" 'BEGIN{}' </dev/null
    total=1; count=0
    for p in "${progs[@]}"; do
        r=$(ratio "${ref[$p]}" "${cur[$p]}")
        [ "$r" = "-" ] && continue
        total=$(awk -v t="$total" -v r="$r" 'BEGIN{print t*r}')
        count=$((count + 1))
    done
    [ "$count" -gt 0 ] && \
        echo "geometric mean speedup vs reference: $(awk -v t="$total" -v c="$count" 'BEGIN{printf "%.2fx", exp(log(t)/c)}')"
fi
