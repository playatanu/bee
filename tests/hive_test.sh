#!/usr/bin/env bash
#
# End-to-end tests for `hive`, the BeeLang package manager, and for the
# interpreter's side of the contract (finding installed packages).
#
# Run from anywhere:  bash tests/hive_test.sh
# Requires `bee` and `hive` to be built already (`make`).
#
# Everything happens inside a temporary directory: a fake registry is served
# straight off the filesystem, and HIVE_HOME is redirected, so the tests never
# touch the network or the real ~/.hive.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BEE="$ROOT/bee"
HIVE="$ROOT/hive"

[ -x "$BEE" ]  || { echo "tests: $BEE not built -- run 'make' first" >&2; exit 1; }
[ -x "$HIVE" ] || { echo "tests: $HIVE not built -- run 'make' first" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
export HIVE_HOME="$WORK/hivehome"
REG="$WORK/registry"

PASS=0
FAIL=0

ok()   { PASS=$((PASS+1)); printf '  ok   %s\n' "$1"; }
bad()  { FAIL=$((FAIL+1)); printf '  FAIL %s\n' "$1"; [ -n "${2:-}" ] && printf '       %s\n' "$2"; }

# check <name> <expected-substring> <command...>
check() {
    local name="$1" want="$2"; shift 2
    local out
    out="$("$@" 2>&1)"
    if [[ "$out" == *"$want"* ]]; then ok "$name"; else bad "$name" "expected '$want' in: $out"; fi
}

# check_fails <name> <expected-substring> <command...>
check_fails() {
    local name="$1" want="$2"; shift 2
    local out status
    out="$("$@" 2>&1)"; status=$?
    if [ $status -eq 0 ]; then
        bad "$name" "command unexpectedly succeeded: $out"
    elif [[ "$out" == *"$want"* ]]; then ok "$name"
    else bad "$name" "expected '$want' in: $out"; fi
}

# ---------------------------------------------------------------------------
# A registry with two packages: logger 1.0.0 / 1.1.0, and greet 1.2.0 which
# depends on logger ^1.0.0.
# ---------------------------------------------------------------------------
mkdir -p "$REG/packages" "$REG/files" "$WORK/src"

make_pkg() {  # make_pkg <name> <version> <deps-json> <body>
    local name="$1" version="$2" deps="$3" body="$4"
    local dir="$WORK/src/$name-$version"
    mkdir -p "$dir"
    cat > "$dir/hive.json" <<EOF
{
  "name": "$name",
  "version": "$version",
  "description": "test package $name",
  "license": "MIT",
  "main": "init.bee",
  "dependencies": $deps
}
EOF
    printf '%s\n' "$body" > "$dir/init.bee"
    "$HIVE" pack "$dir" -q -o "$REG/files/$name-$version.hive" >/dev/null || return 1
}

publish() {  # publish <name> <version...> -- writes registry metadata
    local name="$1"; shift
    local entries=""
    for v in "$@"; do
        local sha
        sha="$(sha256sum "$REG/files/$name-$v.hive" | cut -d' ' -f1)"
        local deps
        deps="$(sed -n 's/.*"dependencies": \(.*\)/\1/p' "$WORK/src/$name-$v/hive.json")"
        [ -z "$deps" ] && deps="{}"
        [ -n "$entries" ] && entries="$entries,"
        entries="$entries\"$v\":{\"url\":\"files/$name-$v.hive\",\"sha256\":\"$sha\",\"dependencies\":$deps}"
    done
    cat > "$REG/packages/$name.json" <<EOF
{"name":"$name","description":"test package $name","versions":{$entries}}
EOF
}

make_pkg logger 1.0.0 '{}' 'fn log(m) { print("[1.0.0] " + m) }'
make_pkg logger 1.1.0 '{}' 'fn log(m) { print("[1.1.0] " + m) }'
make_pkg greet  1.2.0 '{ "logger": "^1.0.0" }' 'import logger

fn hello(name) {
    logger.log("greeting " + name)
    return "Hello, " + name + "!"
}'
publish logger 1.0.0 1.1.0
publish greet 1.2.0
cat > "$REG/index.json" <<'EOF'
{"packages":[{"name":"logger","version":"1.1.0","description":"test package logger"},
             {"name":"greet","version":"1.2.0","description":"test package greet"}]}
EOF

R=(--registry "$REG")

echo "hive: archives and manifests"
check "pack reports the package"        "packed logger@1.0.0" "$HIVE" pack "$WORK/src/logger-1.0.0" -o "$WORK/scratch.hive"
check_fails "pack rejects a bad name"   "invalid package name" \
    bash -c "mkdir -p '$WORK/badname' && printf '{\"name\":\"Bad Name\",\"version\":\"1.0.0\"}' > '$WORK/badname/hive.json' && '$HIVE' pack '$WORK/badname'"
check_fails "pack needs an entry module" "entry module 'init.bee' is missing" \
    bash -c "mkdir -p '$WORK/noentry' && printf '{\"name\":\"noentry\",\"version\":\"1.0.0\"}' > '$WORK/noentry/hive.json' && '$HIVE' pack '$WORK/noentry'"

echo "hive: install from a registry"
APP="$WORK/app"
mkdir -p "$APP"
check "init writes hive.json"           "hive.json"     "$HIVE" init "$APP"
( cd "$APP" && "$HIVE" install greet "${R[@]}" >/dev/null 2>&1 )
[ -f "$APP/hive_modules/greet/init.bee" ]  && ok "installs the requested package"   || bad "installs the requested package"
[ -f "$APP/hive_modules/logger/init.bee" ] && ok "installs transitive dependencies" || bad "installs transitive dependencies"
[ -f "$APP/hive.lock" ]                    && ok "writes a lockfile"                || bad "writes a lockfile"
grep -q '"greet": "\^1.2.0"' "$APP/hive.json" && ok "saves the dependency in hive.json" || bad "saves the dependency in hive.json"
grep -q '"logger"' "$APP/hive.json" && bad "does not save transitive deps" || ok "does not save transitive deps"

# greet asks for logger ^1.0.0 and 1.1.0 exists, so that is what should land.
check "picks the newest matching version" '"version": "1.1.0"' \
    bash -c "grep '\"version\"' '$APP/hive_modules/logger/hive.json'"
check "list shows both packages"         "logger" "$HIVE" list -C "$APP"
check "list marks dependencies"          "(dependency)" "$HIVE" list -C "$APP"
check "reinstall is a no-op"             "up to date" bash -c "cd '$APP' && '$HIVE' install ${R[*]}"

echo "bee: importing installed packages"
cat > "$APP/main.bee" <<'EOF'
import greet
from logger import log

print(greet.hello("BeeLang"))
log("done")
EOF
check "import finds a package"           "Hello, BeeLang!" "$BEE" "$APP/main.bee"
check "a package imports its own deps"   "greeting BeeLang" "$BEE" "$APP/main.bee"

# A local module of the same name must win, or a project could never override a
# package it has installed.
printf 'fn log(m) { print("local " + m) }\n' > "$APP/logger.bee"
check "local modules shadow packages"    "local shadowed" bash -c "printf 'from logger import log\nlog(\"shadowed\")\n' > '$APP/s.bee' && '$BEE' '$APP/s.bee'"
rm -f "$APP/logger.bee" "$APP/s.bee"

check_fails "missing modules hint at hive" "hive install nosuchmodule" \
    bash -c "printf 'import nosuchmodule\n' > '$APP/m.bee' && '$BEE' '$APP/m.bee'"

echo "hive: install from a .hive file"
FAPP="$WORK/fileapp"
mkdir -p "$FAPP"
"$HIVE" init "$FAPP" -q
check "installs a local archive"         "+ greet@1.2.0" \
    bash -c "cd '$FAPP' && '$HIVE' install '$REG/files/greet-1.2.0.hive' ${R[*]}"
[ -f "$FAPP/hive_modules/logger/init.bee" ] && ok "resolves a file package's deps" || bad "resolves a file package's deps"

echo "hive: constraints and conflicts"
check "installs an exact version"        "+ logger@1.0.0" \
    bash -c "mkdir -p '$WORK/pin' && '$HIVE' init '$WORK/pin' -q && cd '$WORK/pin' && '$HIVE' install logger@1.0.0 ${R[*]}"
check "understands ~ constraints"        "+ logger@1.1.0" \
    bash -c "mkdir -p '$WORK/tilde' && '$HIVE' init '$WORK/tilde' -q && cd '$WORK/tilde' && '$HIVE' install 'logger@~1.1.0' ${R[*]}"
check_fails "reports unsatisfiable constraints" "no version of 'logger' satisfies" \
    bash -c "mkdir -p '$WORK/conflict' && '$HIVE' init '$WORK/conflict' -q && cd '$WORK/conflict' && '$HIVE' install 'logger@>=9.0.0' ${R[*]}"
check_fails "rejects an unknown package" "cannot find 'nosuch' in the registry" \
    bash -c "cd '$APP' && '$HIVE' install nosuch ${R[*]}"

echo "hive: integrity"
cp "$REG/files/logger-1.0.0.hive" "$WORK/trailing.hive"
printf 'X' >> "$WORK/trailing.hive"
check_fails "rejects trailing bytes"     "unexpected trailing bytes" \
    bash -c "cd '$APP' && '$HIVE' install '$WORK/trailing.hive' ${R[*]}"

python3 - "$REG/files/logger-1.0.0.hive" "$WORK/flipped.hive" <<'PY'
import sys
data = open(sys.argv[1], 'rb').read()
i = data.rfind(b'[1.0.0]')
open(sys.argv[2], 'wb').write(data[:i] + b'[9.9.9]' + data[i+7:])
PY
check_fails "rejects a modified file"    "checksum mismatch" \
    bash -c "cd '$APP' && '$HIVE' install '$WORK/flipped.hive' ${R[*]}"

# A crafted archive must not be able to write outside its package directory.
python3 - "$WORK/traversal.hive" <<'PY'
import hashlib, json, sys
data = b'print("escaped")\n'
header = json.dumps({
    "format": 1,
    "manifest": {"name": "evil", "version": "1.0.0", "main": "init.bee"},
    "files": [{"path": "../../escaped.bee", "size": len(data),
               "sha256": hashlib.sha256(data).hexdigest()}],
}, separators=(',', ':')).encode()
open(sys.argv[1], 'wb').write(b"HIVE1\n%d\n" % len(header) + header + b"\n" + data)
PY
check_fails "rejects paths escaping the package" "unsafe path" \
    bash -c "cd '$APP' && '$HIVE' install '$WORK/traversal.hive' ${R[*]}"
[ -f "$WORK/escaped.bee" ] && bad "traversal wrote outside the package" || ok "traversal wrote nothing"

# A registry that lies about a hash must not get its bytes installed.
mkdir -p "$WORK/badreg/packages"
cp -r "$REG/files" "$WORK/badreg/files"
printf '{"name":"logger","versions":{"1.0.0":{"url":"files/logger-1.0.0.hive","sha256":"%064d","dependencies":{}}}}' 0 \
    > "$WORK/badreg/packages/logger.json"
check_fails "verifies downloads against the registry hash" "checksum mismatch" \
    bash -c "mkdir -p '$WORK/badapp' && '$HIVE' init '$WORK/badapp' -q && cd '$WORK/badapp' && '$HIVE' install logger --registry '$WORK/badreg'"

# Installing a package inside its own source tree is a mistyped directory, not a
# request: it would create logger/hive_modules/logger.
check_fails "refuses to install a package into itself" "is this package" \
    bash -c "cd '$WORK/src/logger-1.0.0' && '$HIVE' install '$REG/files/logger-1.0.0.hive' ${R[*]}"
[ -d "$WORK/src/logger-1.0.0/hive_modules" ] \
    && bad "no hive_modules inside the package itself" \
    || ok "no hive_modules inside the package itself"
check "--force allows it anyway" "+ logger@1.0.0" \
    bash -c "cd '$WORK/src/logger-1.0.0' && '$HIVE' install '$REG/files/logger-1.0.0.hive' --force ${R[*]}"
# Check the dependencies object specifically: the manifest's own "name" field
# also contains the package name.
if python3 -c "
import json, sys
deps = json.load(open('$WORK/src/logger-1.0.0/hive.json')).get('dependencies', {})
sys.exit(0 if 'logger' not in deps else 1)"; then
    ok "self-dependency stays out of hive.json"
else
    bad "self-dependency stays out of hive.json"
fi

echo "hive: uninstall and global installs"
check "uninstall removes a package"      "- greet@1.2.0" bash -c "cd '$APP' && '$HIVE' uninstall greet"
[ -d "$APP/hive_modules/greet" ] && bad "uninstall deletes the directory" || ok "uninstall deletes the directory"
grep -q '"greet"' "$APP/hive.json" && bad "uninstall updates hive.json" || ok "uninstall updates hive.json"
check_fails "uninstall reports unknown packages" "is not installed" bash -c "cd '$APP' && '$HIVE' uninstall nosuch"

# A directory hive did not create must not be silently replaced.
mkdir -p "$WORK/hand/hive_modules/logger"
"$HIVE" init "$WORK/hand" -q
printf 'fn log(m) { print("handwritten") }\n' > "$WORK/hand/hive_modules/logger/init.bee"
check_fails "refuses to clobber foreign directories" "not installed by hive" \
    bash -c "cd '$WORK/hand' && '$HIVE' install logger ${R[*]}"
check "--force overwrites them"          "+ logger@1.1.0" \
    bash -c "cd '$WORK/hand' && '$HIVE' install logger --force ${R[*]}"

check "installs globally"                "+ logger@1.1.0" "$HIVE" install logger -g "${R[@]}"
mkdir -p "$WORK/bare"
check "bee finds global packages"        "[1.1.0] global" \
    bash -c "printf 'from logger import log\nlog(\"global\")\n' > '$WORK/bare/g.bee' && '$BEE' '$WORK/bare/g.bee'"

echo "hive: registry queries and cache"
check "info lists versions"              "1.0.0, 1.1.0" "$HIVE" info logger "${R[@]}"
check "info shows dependencies"          "logger@^1.0.0" "$HIVE" info greet "${R[@]}"
check "search matches descriptions"      "greet"         "$HIVE" search greet "${R[@]}"
check "cache dir is under HIVE_HOME"     "$HIVE_HOME"    "$HIVE" cache dir

# The lockfile pins versions and hashes, and the cache holds the bytes, so a
# repeat install needs no network at all. An unreachable registry proves it.
DEAD=(--registry http://127.0.0.1:9/nope)
check "offline install needs no registry" "up to date" \
    bash -c "cd '$WORK/tilde' && '$HIVE' install --offline ${DEAD[*]}"
check "offline reinstall uses the cache"  "+ logger@1.1.0" \
    bash -c "rm -rf '$WORK/tilde/hive_modules' && cd '$WORK/tilde' && '$HIVE' install --offline ${DEAD[*]}"

# Clearing the cache leaves an offline install with nothing to unpack.
check "cache clean works"                "cache cleared" "$HIVE" cache clean
check_fails "offline reports a cache miss" "not in the cache" \
    bash -c "rm -rf '$WORK/tilde/hive_modules' && cd '$WORK/tilde' && '$HIVE' install --offline ${DEAD[*]}"

echo
if [ "$FAIL" -eq 0 ]; then
    echo "all $PASS checks passed"
    exit 0
fi
echo "$PASS passed, $FAIL failed"
exit 1
