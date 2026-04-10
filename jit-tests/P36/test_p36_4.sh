#!/bin/sh
# P36.4 test: --jit-profile-time=<file>[,Hz] CLI flag
#
# Verifies that:
#   A) --jit-profile-time produces a JSON profile with "time_ms" field
#   B) The profile contains a non-zero time_ms for a hot function
#   C) --jit-profile-time=<file>,Hz syntax parses the Hz correctly
#   D) Global script functions (not in any module) appear in the profile
#      via the registry second pass added in P36.4
#   E) Module functions also appear in the profile (module walk path)
#
# Run from quickjs/ directory:
#   sh jit-tests/P36/test_p36_4.sh

set -e

QJS=${QJS:-./qjs}
CACHE_DIR=${HOME}/.cache/qjs-jit

fail() {
    echo "FAIL: $1" >&2
    exit 1
}

# Clean the JIT cache before each subtest to avoid cache-hit interference
clean_cache() {
    rm -f "${CACHE_DIR}"/*.so "${CACHE_DIR}"/*.skip 2>/dev/null || true
}

echo "=== P36.4: --jit-profile-time CLI flag ==="

# --- Test A: basic timed profile contains time_ms field ---
clean_cache
echo "function fib(n){return n<=1?n:fib(n-1)+fib(n-2);}var r=0;for(var i=0;i<200;i++)r+=fib(22);" > /tmp/p36_4_fib.js
"${QJS}" --jit-compile-all --jit-profile-time=/tmp/p36_4a.json,1000 /tmp/p36_4_fib.js
test -f /tmp/p36_4a.json || fail "A: profile file not created"
grep -q '"time_ms"' /tmp/p36_4a.json    || fail "A: time_ms field missing from profile"
grep -q '"functions"' /tmp/p36_4a.json  || fail "A: functions key missing"
rm -f /tmp/p36_4a.json
echo "PASS A: --jit-profile-time produces JSON with time_ms field"

# --- Test B: at least one time_ms > 0 for a hot function ---
clean_cache
"${QJS}" --jit-compile-all --jit-profile-time=/tmp/p36_4b.json,1000 /tmp/p36_4_fib.js
test -f /tmp/p36_4b.json || fail "B: profile file not created"
# grep for "time_ms":N where N > 0 (any non-zero value after the colon)
if ! grep -qE '"time_ms":[1-9]' /tmp/p36_4b.json; then
    fail "B: all time_ms are 0 — sampler produced no samples (profile: $(cat /tmp/p36_4b.json))"
fi
rm -f /tmp/p36_4b.json
echo "PASS B: profile has time_ms > 0 for hot function"

# --- Test C: Hz=500 parses correctly (no time_ms field conflict with hz=0) ---
clean_cache
"${QJS}" --jit-compile-all --jit-profile-time=/tmp/p36_4c.json,500 /tmp/p36_4_fib.js
test -f /tmp/p36_4c.json || fail "C: profile file not created"
grep -q '"time_ms"' /tmp/p36_4c.json || fail "C: time_ms missing with Hz=500"
rm -f /tmp/p36_4c.json
echo "PASS C: Hz=500 syntax accepted, time_ms present"

# --- Test D: global script functions appear in profile (registry second pass) ---
clean_cache
cat > /tmp/p36_4_global.js <<'EOF'
function hot(n) {
    var s = 0;
    for (var i = 0; i < n; i++) s += i;
    return s;
}
var r = 0;
for (var j = 0; j < 300000; j++) r += hot(100);
EOF
"${QJS}" --jit-compile-all --jit-profile-time=/tmp/p36_4d.json,1000 /tmp/p36_4_global.js
test -f /tmp/p36_4d.json || fail "D: profile file not created"
grep -q '"name":"hot"' /tmp/p36_4d.json || fail "D: global-script function 'hot' missing from profile"
rm -f /tmp/p36_4d.json
echo "PASS D: global-script function appears in profile via registry pass"

# --- Test E: module functions also appear in profile (module walk path) ---
clean_cache
cat > /tmp/p36_4_mod.mjs <<'EOF'
export function fib_iter(n) {
    var a = 0, b = 1, c, i;
    for (i = 0; i < n; i++) { c = a + b; a = b; b = c; }
    return a;
}
var r = 0;
for (var j = 0; j < 500000; j++) r += fib_iter(30);
EOF
"${QJS}" --jit-compile-all --jit-profile-time=/tmp/p36_4e.json,1000 /tmp/p36_4_mod.mjs
test -f /tmp/p36_4e.json || fail "E: profile file not created"
grep -q '"name":"fib_iter"' /tmp/p36_4e.json || fail "E: module function 'fib_iter' missing from profile"
grep -q '"time_ms"' /tmp/p36_4e.json          || fail "E: time_ms missing from module profile"
rm -f /tmp/p36_4e.json /tmp/p36_4_mod.mjs
echo "PASS E: module function appears in profile via module walk path"

# Cleanup
rm -f /tmp/p36_4_fib.js /tmp/p36_4_global.js

echo "=== ALL P36.4 TESTS PASSED ==="
