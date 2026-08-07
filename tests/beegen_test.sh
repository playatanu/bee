#!/usr/bin/env bash
#
# End-to-end tests for `beegen`, the binding generator: parse a C++ header,
# generate a native module and a BeeLang wrapper, compile them, and call the
# real C++ from BeeLang.
#
# Run from anywhere:  bash tests/beegen_test.sh
# Requires `bee` and `beegen` to be built (`make`), plus a C++ compiler and a
# libclang shared library. Without those last two the suite skips rather than
# fails, since neither is needed to build BeeLang itself.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BEE="$ROOT/bee"
BEEGEN="$ROOT/beegen"

[ -x "$BEE" ]    || { echo "tests: $BEE not built -- run 'make' first" >&2; exit 1; }
[ -x "$BEEGEN" ] || { echo "tests: $BEEGEN not built -- run 'make' first" >&2; exit 1; }

PASS=0
FAIL=0
ok()  { PASS=$((PASS+1)); printf '  ok   %s\n' "$1"; }
bad() { FAIL=$((FAIL+1)); printf '  FAIL %s\n' "$1"; [ -n "${2:-}" ] && printf '       %s\n' "$2"; }

# libclang is loaded at run time, so its absence is a skip, not a failure.
if ! "$BEEGEN" --version >/dev/null 2>&1; then
    echo "beegen: cannot run the binary at all"
    exit 1
fi
probe="$(mktemp -d)"
printf 'int probe_fn(int a);\n' > "$probe/probe.hpp"
if ! "$BEEGEN" "$probe/probe.hpp" -m probe -o "$probe" >/dev/null 2>&1; then
    echo "  skip all beegen checks -- no usable libclang found"
    echo "       (Debian/Ubuntu: sudo apt install libclang-dev, or set LIBCLANG_PATH)"
    rm -rf "$probe"
    exit 0
fi
rm -rf "$probe"

CXX="${CXX:-g++}"
HAVE_CXX=1
command -v "$CXX" >/dev/null 2>&1 || HAVE_CXX=0

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cd "$WORK" || exit 1

# ---------------------------------------------------------------------------
# A header covering what the mapper is supposed to handle, and several things it
# is supposed to refuse.
# ---------------------------------------------------------------------------
cat > shapes.hpp <<'EOF'
#pragma once
#include <string>

enum Color { COLOR_RED = 0, COLOR_GREEN = 1, COLOR_BLUE = 7 };

int add(int a, int b);
double scale(double v, double factor);
bool is_even(int n);
const char* library_name();
const char* maybe_null(bool give);
std::string greet(const std::string& who);
void nothing();

class Rect {
public:
    Rect(double w, double h);
    double area() const;
    void grow(double by);
    std::string describe() const;
    static double unit_area();
    double width;
    double height;
    Color color;
};

double rect_area_of(const Rect& r);      // handle parameter

template <typename T> T identity(T v);   // skipped: template
int sum_all(int count, ...);             // skipped: variadic
struct Unbound;                          // skipped: forward declaration only
void take_unbound(Unbound* u);           // skipped: unbound pointer
void out_param(int& result);             // skipped: non-const reference
EOF

cat > shapes.cpp <<'EOF'
#include "shapes.hpp"
int add(int a, int b) { return a + b; }
double scale(double v, double factor) { return v * factor; }
bool is_even(int n) { return n % 2 == 0; }
const char* library_name() { return "shapes 1.0"; }
const char* maybe_null(bool give) { return give ? "here" : nullptr; }
std::string greet(const std::string& who) { return "Hello, " + who + "!"; }
void nothing() {}
Rect::Rect(double w, double h) : width(w), height(h), color(COLOR_RED) {}
double Rect::area() const { return width * height; }
void Rect::grow(double by) { width += by; height += by; }
std::string Rect::describe() const { return "Rect"; }
double Rect::unit_area() { return 1.0; }
double rect_area_of(const Rect& r) { return r.area(); }
EOF

echo "beegen: generating"
gen_out="$("$BEEGEN" shapes.hpp --module shapes --bee-src "$ROOT/src" 2>&1)"
gen_status=$?
[ $gen_status -eq 0 ] && ok "generates without error" || bad "generates without error" "$gen_out"

for f in shapes_native.cpp shapes.bee build.sh hive.json; do
    [ -f "$f" ] && ok "wrote $f" || bad "wrote $f"
done

# The report of what was left out is the feature, not a footnote: a binding that
# silently drops half a library is worse than one that says so.
echo "beegen: reports what it skipped"
for pattern in "template function identity" "function sum_all" "take_unbound" "out_param"; do
    [[ "$gen_out" == *"$pattern"* ]] && ok "reports $pattern" || bad "reports $pattern" "$gen_out"
done
[[ "$gen_out" == *"variadic"* ]]              && ok "explains the variadic skip"  || bad "explains the variadic skip"
[[ "$gen_out" == *"unbound type 'Unbound'"* ]] && ok "explains the unbound skip"  || bad "explains the unbound skip"
[[ "$gen_out" == *"non-const reference"* ]]   && ok "explains the out-param skip" || bad "explains the out-param skip"

echo "beegen: wrapper shape"
grep -q "^import shapes_native" shapes.bee            && ok "wrapper imports the native module" || bad "wrapper imports the native module"
grep -q "^class Rect {" shapes.bee                    && ok "class becomes a Bee class"         || bad "class becomes a Bee class"
grep -q "let Color = {" shapes.bee                    && ok "enum becomes a dict"               || bad "enum becomes a dict"
grep -q "fn Rect_unit_area()" shapes.bee              && ok "static method becomes a function"  || bad "static method becomes a function"
grep -q "free()" shapes.bee                           && ok "class gets free()"                 || bad "class gets free()"
grep -q "Rect_get_width" shapes.bee                   && ok "public field gets an accessor"     || bad "public field gets an accessor"

if [ "$HAVE_CXX" -eq 0 ]; then
    echo "  skip compile-and-run checks (no $CXX)"
else
    echo "beegen: the generated module compiles and runs"
    build_log="$("$CXX" -std=c++17 -O2 -fPIC -c shapes.cpp -o shapes.o 2>&1 &&
                 "$CXX" -std=c++17 -O2 -fPIC -shared -I"$ROOT/src" shapes_native.cpp shapes.o \
                        -o shapes_native.so 2>&1)"
    if [ -f shapes_native.so ]; then ok "compiles into a shared library"
    else bad "compiles into a shared library" "$build_log"; fi

    if [ -f shapes_native.so ]; then
        cat > use.bee <<'EOF'
import shapes
from shapes import Rect, Color

print(shapes.add(2, 3))
print(shapes.scale(2.5, 4))
print(shapes.is_even(10))
print(shapes.library_name())
print(shapes.greet("BeeLang"))
print(shapes.nothing())
print(shapes.maybe_null(false))
print(Color.COLOR_BLUE)

let r = new Rect(3, 4)
print(r.area())
r.grow(1)
print(r.get_width())
r.set_width(10)
print(r.area())
print(shapes.rect_area_of(r._handle))
print(shapes.Rect_unit_area())
r.free()
EOF
        run_out="$("$BEE" use.bee 2>&1)"
        expect_lines=("5" "10" "true" "shapes 1.0" "Hello, BeeLang!" "nil" "nil" "7" "12" "4" "50" "50" "1")
        i=0
        matched=1
        while IFS= read -r line; do
            [ "$line" = "${expect_lines[$i]:-}" ] || matched=0
            i=$((i+1))
        done <<< "$run_out"
        [ "$matched" -eq 1 ] && [ "$i" -eq "${#expect_lines[@]}" ] \
            && ok "every generated call returns the right value" \
            || bad "every generated call returns the right value" "got:
$run_out"

        # A C++ null pointer must arrive as nil, not as an empty string: they
        # mean different things in every C API.
        out="$("$BEE" -e 'import shapes; print(shapes.maybe_null(true))' 2>&1)"
        [[ "$out" == "here" ]] && ok "non-null const char* becomes a string" || bad "non-null const char* becomes a string" "$out"

        echo "beegen: mistakes are reported as Bee errors"
        out="$("$BEE" -e 'import shapes; shapes.add("x", 1)' 2>&1)"
        [[ "$out" == *"must be a number, got a string"* ]] && ok "wrong argument type" || bad "wrong argument type" "$out"
        out="$("$BEE" -e 'import shapes; shapes.add(1)' 2>&1)"
        [[ "$out" == *"argument"* ]] && ok "wrong argument count" || bad "wrong argument count" "$out"
        out="$("$BEE" -e 'import shapes; shapes.add(1.5, 2)' 2>&1)"
        [[ "$out" == *"whole number"* ]] && ok "a fraction where an int is wanted" || bad "a fraction where an int is wanted" "$out"
        out="$("$BEE" -e 'import shapes; shapes.rect_area_of({"__handle": 1, "__type": "Wrong"})' 2>&1)"
        [[ "$out" == *"must be a Rect handle"* ]] && ok "a handle of the wrong type" || bad "a handle of the wrong type" "$out"
        out="$("$BEE" -e '
import shapes
from shapes import Rect
let r = new Rect(1, 1)
r.free()
r.area()' 2>&1)"
        [[ "$out" == *"handle"* ]] && ok "use after free" || bad "use after free" "$out"
        out="$("$BEE" -e '
import shapes
from shapes import Rect
let r = new Rect(1, 1)
r.free()
r.free()
print("double free survived")' 2>&1)"
        [[ "$out" == *"double free survived"* ]] && ok "free() twice is harmless" || bad "free() twice is harmless" "$out"

        # Errors from a native module carry a trace like any other.
        out="$("$BEE" -e 'import shapes
fn wrapper() { return shapes.add("x", 1) }
wrapper()' 2>&1)"
        [[ "$out" == *"at wrapper()"* ]] && ok "native errors get a stack trace" || bad "native errors get a stack trace" "$out"
    fi
fi

# ---------------------------------------------------------------------------
# The capabilities a real ML binding needs: zero-copy buffers, std::vector,
# default arguments, and abstract interfaces made by a factory. This header is
# shaped like ONNX Runtime, TensorRT and OpenCV headers are.
# ---------------------------------------------------------------------------
echo "beegen: capabilities for native library APIs"
cat > infer.hpp <<'EOF'
#pragma once
#include <string>
#include <vector>
#include "bee_buffer.h"

double tensor_sum(BeeBuffer t);
void tensor_scale(BeeBuffer t, double factor);

std::vector<long long> input_shape(int index);
std::vector<std::string> output_names();
double dot(const std::vector<double>& a, const std::vector<double>& b);

double resize_cost(int width, int height, double scale = 1.0, int interpolation = 2);

class ILogger {
public:
    virtual ~ILogger() = default;
    virtual int level() const = 0;
};

class ConsoleLogger : public ILogger {
public:
    ConsoleLogger(int level);
    int level() const override;
};

class IEngine {
public:
    virtual ~IEngine() = default;
    virtual bool run(BeeBuffer input, BeeBuffer output) = 0;
    virtual void destroy() = 0;
};

IEngine* create_engine(ILogger* logger, int batch = 1);
int level_through(ILogger* logger);
EOF
cat > infer.cpp <<'EOF'
#include "infer.hpp"
#include <cstring>
double tensor_sum(BeeBuffer t) {
    double total = 0;
    const float* p = (const float*)t.data;
    for (long long i = 0, n = bee_buffer_count(&t); i < n; ++i) total += p[i];
    return total;
}
void tensor_scale(BeeBuffer t, double factor) {
    float* p = (float*)t.data;
    for (long long i = 0, n = bee_buffer_count(&t); i < n; ++i) p[i] *= (float)factor;
}
std::vector<long long> input_shape(int index) { return {1, 3, 224, (long long)224 + index}; }
std::vector<std::string> output_names() { return {"logits", "probs"}; }
double dot(const std::vector<double>& a, const std::vector<double>& b) {
    double s = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) s += a[i] * b[i];
    return s;
}
double resize_cost(int w, int h, double scale, int interp) { return w * h * scale * interp; }
ConsoleLogger::ConsoleLogger(int level) : level_(level) {}
int ConsoleLogger::level() const { return level_; }
namespace {
struct Engine : IEngine {
    int batch;
    explicit Engine(int b) : batch(b) {}
    bool run(BeeBuffer in, BeeBuffer out) override {
        if (in.bytes != out.bytes) return false;
        const float* i = (const float*)in.data;
        float* o = (float*)out.data;
        for (long long k = 0, n = bee_buffer_count(&in); k < n; ++k) o[k] = i[k] * 2.0f;
        return true;
    }
    void destroy() override { delete this; }
};
}
IEngine* create_engine(ILogger*, int batch) { return new Engine(batch); }
int level_through(ILogger* l) { return l->level(); }
EOF
# ConsoleLogger needs the member the header hides; keep the fixture compiling.
sed -i 's/    int level() const override;/    int level() const override;\nprivate:\n    int level_;/' infer.hpp

cap_out="$("$BEEGEN" infer.hpp -m infer --bee-src "$ROOT/src" -I"$ROOT/src" -o "$WORK/cap" 2>&1)"
[[ "$cap_out" == *"skipped"* ]] && bad "maps every declaration in an ML-shaped header" "$cap_out" \
                               || ok "maps every declaration in an ML-shaped header"
grep -q "bufferView" "$WORK/cap/infer_native.cpp"    && ok "BeeBuffer becomes a buffer argument" || bad "BeeBuffer becomes a buffer argument"
grep -q "toVector<"  "$WORK/cap/infer_native.cpp"    && ok "std::vector becomes a list argument" || bad "std::vector becomes a list argument"
grep -q "fromVector" "$WORK/cap/infer_native.cpp"    && ok "std::vector is returned as a list"   || bad "std::vector is returned as a list"
grep -q "registerUpcast" "$WORK/cap/infer_native.cpp" && ok "base classes register an upcast"    || bad "base classes register an upcast"
grep -q "resize_cost__2" "$WORK/cap/infer_native.cpp" && ok "defaults become one entry per arity" || bad "defaults become one entry per arity"
grep -q "IEngine is abstract" "$WORK/cap/infer_native.cpp" && ok "abstract classes get no constructor" || bad "abstract classes get no constructor"
grep -q "IEngine_free" "$WORK/cap/infer_native.cpp" && bad "no delete through an abstract base" || ok "no delete through an abstract base"
# Every wrapper now adopts a handle from a factory as well as constructing one,
# which is how imread()/create_engine()/CreateSession() results get used at all.
grep -q 'type(args\[0\]) == "dict"' "$WORK/cap/infer.bee" && ok "a wrapper adopts a factory handle" || bad "a wrapper adopts a factory handle"
grep -q "only a factory can make one" "$WORK/cap/infer.bee" && ok "an abstract wrapper refuses to construct" || bad "an abstract wrapper refuses to construct"
grep -q "len(rest) == 1" "$WORK/cap/infer.bee"      && ok "the wrapper dispatches on arity"     || bad "the wrapper dispatches on arity"

if [ "$HAVE_CXX" -ne 0 ]; then
    cp infer.hpp infer.cpp "$WORK/cap/" 2>/dev/null
    ( cd "$WORK/cap" &&
      "$CXX" -std=c++17 -O2 -fPIC -c infer.cpp -I"$ROOT/src" -o infer.o >/dev/null 2>&1 &&
      "$CXX" -std=c++17 -O2 -fPIC -shared -I"$ROOT/src" infer_native.cpp infer.o \
             -o infer_native.so >/dev/null 2>&1 )
    if [ -f "$WORK/cap/infer_native.so" ]; then
        ok "the capability module compiles"
        cat > "$WORK/cap/run.bee" <<'EOF'
import infer
from infer import ConsoleLogger, IEngine

let t = buffer([2, 2], "f32")
fill(t, 3)
print(infer.tensor_sum(t))          # 12 -- read through the raw pointer
infer.tensor_scale(t, 2)
print(to_list(t))                   # mutated in place, no copy back
print(infer.input_shape(0))
print(infer.output_names())
print(infer.dot([1, 2, 3], [4, 5, 6]))
print(infer.resize_cost(10, 10))
print(infer.resize_cost(10, 10, 0.5))
print(infer.resize_cost(10, 10, 0.5, 4))
let lg = new ConsoleLogger(3)
print(infer.level_through(lg._handle))
let eng = new IEngine(infer.create_engine(lg._handle))
let out = buffer([2, 2], "f32")
print(eng.run(t, out))
print(to_list(out))
eng.destroy()
EOF
        run="$("$BEE" "$WORK/cap/run.bee" 2>&1)"
        expected="12
[[6, 6], [6, 6]]
[1, 3, 224, 224]
[\"logits\", \"probs\"]
32
200
100
200
3
true
[[12, 12], [12, 12]]"
        [ "$run" = "$expected" ] && ok "every capability behaves correctly at run time" \
            || bad "every capability behaves correctly at run time" "got:
$run"
    else
        bad "the capability module compiles"
    fi
fi

# `in`, `class` and `from` are ordinary parameter names in C++ and keywords in
# BeeLang; generated code has to stay parseable.
printf '#include "bee_buffer.h"\nint copy_into(BeeBuffer in, int from, int class_);\n' > kw.hpp
"$BEEGEN" kw.hpp -m kw -o "$WORK/kw" -I"$ROOT/src" >/dev/null 2>&1
if grep -q "fn copy_into(in_, from_, class_)" "$WORK/kw/kw.bee" 2>/dev/null; then
    ok "keyword-named parameters are renamed"
else
    bad "keyword-named parameters are renamed" "$(grep 'fn copy_into' "$WORK/kw/kw.bee" 2>&1)"
fi

echo "beegen: option handling"
out="$("$BEEGEN" shapes.hpp -m only_funcs --no-classes --no-enums -o "$WORK/of" 2>&1)"
grep -q "class Rect" "$WORK/of/only_funcs.bee" 2>/dev/null && bad "--no-classes drops classes" || ok "--no-classes drops classes"
grep -q "let Color" "$WORK/of/only_funcs.bee" 2>/dev/null && bad "--no-enums drops enums" || ok "--no-enums drops enums"

out="$("$BEEGEN" shapes.hpp -m pref --prefix Rect --prefix rect_ -o "$WORK/pref" 2>&1)"
grep -q "fn rect_area_of" "$WORK/pref/pref.bee" 2>/dev/null && ok "--prefix keeps matching names" || bad "--prefix keeps matching names" "$out"
grep -q "class Rect" "$WORK/pref/pref.bee" 2>/dev/null && ok "--prefix keeps a matching class" || bad "--prefix keeps a matching class"
grep -q "fn add" "$WORK/pref/pref.bee" 2>/dev/null && bad "--prefix drops other names" || ok "--prefix drops other names"

# Filtering out a class also unbinds anything that takes one -- which must be
# reported, not silently dropped.
out="$("$BEEGEN" shapes.hpp -m pref2 --prefix rect_ -o "$WORK/pref2" 2>&1)"
[[ "$out" == *"rect_area_of"* && "$out" == *"unbound type 'Rect'"* ]] \
    && ok "explains a skip caused by a filtered-out type" \
    || bad "explains a skip caused by a filtered-out type" "$out"

out="$("$BEEGEN" shapes.hpp -m skipped --skip add -o "$WORK/sk" 2>&1)"
grep -q "fn add(" "$WORK/sk/skipped.bee" 2>/dev/null && bad "--skip removes a function" || ok "--skip removes a function"

out="$("$BEEGEN" no-such-header.hpp -m nope 2>&1)"
[[ "$out" == *"no such header"* ]] && ok "a missing header is an error" || bad "a missing header is an error" "$out"

printf 'namespace inner { int only_here(int a); }\nint outside(int a);\n' > ns.hpp
out="$("$BEEGEN" ns.hpp -m nsmod --namespace inner -o "$WORK/ns" 2>&1)"
grep -q "only_here" "$WORK/ns/nsmod.bee" 2>/dev/null && ok "--namespace keeps that namespace" || bad "--namespace keeps that namespace" "$out"
grep -q "fn outside" "$WORK/ns/nsmod.bee" 2>/dev/null && bad "--namespace drops the rest" || ok "--namespace drops the rest"

printf '// nothing bindable here\n' > empty.hpp
out="$("$BEEGEN" empty.hpp -m emptymod -o "$WORK/em" 2>&1)"
[[ "$out" == *"nothing to bind"* ]] && ok "an empty header says so" || bad "an empty header says so" "$out"

echo
if [ "$FAIL" -eq 0 ]; then
    echo "all $PASS checks passed"
    exit 0
fi
echo "$PASS passed, $FAIL failed"
exit 1
