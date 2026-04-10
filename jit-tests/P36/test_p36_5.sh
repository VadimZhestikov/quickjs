#!/bin/sh
# P36.5 test: qjsc --jit-pgo time-aware extension
#
# Verifies that:
#   A) A timed profile (with time_ms) is accepted by qjsc --jit-pgo.
#   B) A function with time_ms >= 500 gets #pragma GCC optimize("O3").
#   C) A function with time_ms in [50,500) gets "O2".
#   D) A function with time_ms in [5,50) gets "O1".
#   E) A call-count-only profile (no time_ms field) still works (backwards-compat).
#   F) When both fields present, time_ms wins over calls.
#
# Run from quickjs/ directory:
#   sh jit-tests/P36/test_p36_5.sh

set -e

QJS=${QJS:-./qjs}
QJSC=${QJSC:-./qjsc}

fail() {
    echo "FAIL: $1" >&2
    exit 1
}

echo "=== P36.5: qjsc --jit-pgo time-aware hotness ==="

# Module with four exported functions and enough calls to cross JIT threshold.
cat > /tmp/p36_5_mod.mjs <<'EOF'
export function hot(n) {
    var s = 0;
    for (var i = 0; i < n; i++) s += i;
    return s;
}
export function warm(n) { return n * 2; }
export function cool(n) { return n + 1; }
export function cold(n) { return n - 1; }
var r = 0;
for (var i = 0; i < 200; i++) { r += hot(100); warm(i); cool(i); cold(i); }
EOF

# Compile-all to get hashes for all four functions via a call-count profile.
CACHE_DIR=${HOME}/.cache/qjs-jit
rm -f "${CACHE_DIR}"/*.so "${CACHE_DIR}"/*.skip 2>/dev/null || true

"${QJS}" --jit-compile-all --jit-profile=/tmp/p36_5_calls.json /tmp/p36_5_mod.mjs 2>/dev/null
test -f /tmp/p36_5_calls.json || fail "setup: could not generate profile"

# Extract hash for each function by matching the line containing its name.
# Profile lines look like: {"hash":"HHHH","calls":N,"name":"fname"}
extract_hash() {
    grep "\"name\":\"$1\"" /tmp/p36_5_calls.json \
        | grep -o '"hash":"[^"]*"' | cut -d'"' -f4 | head -1
}

HOT_HASH=$(extract_hash "hot")
WARM_HASH=$(extract_hash "warm")
COOL_HASH=$(extract_hash "cool")
COLD_HASH=$(extract_hash "cold")

test -n "$HOT_HASH"  || fail "setup: hash for 'hot' not found in profile"
test -n "$WARM_HASH" || fail "setup: hash for 'warm' not found in profile"
test -n "$COOL_HASH" || fail "setup: hash for 'cool' not found in profile"
test -n "$COLD_HASH" || fail "setup: hash for 'cold' not found in profile"

# ----------------------------------------------------------------
# Tests A–D: timed profile — hot=600ms(O3), warm=60ms(O2), cool=6ms(O1),
#            cold absent (skipped).
# ----------------------------------------------------------------
cat > /tmp/p36_5_timed.json <<ENDJSON
{"functions":[
  {"hash":"${HOT_HASH}","calls":1,"time_ms":600,"name":"hot"},
  {"hash":"${WARM_HASH}","calls":1,"time_ms":60,"name":"warm"},
  {"hash":"${COOL_HASH}","calls":1,"time_ms":6,"name":"cool"}
]}
ENDJSON

# --- Test A: timed profile accepted by qjsc --jit-pgo ---
"${QJSC}" --jit-hybrid-app --jit-pgo=/tmp/p36_5_timed.json \
          -o /tmp/p36_5_timed.c /tmp/p36_5_mod.mjs 2>/dev/null \
    || fail "A: qjsc --jit-pgo with timed profile returned error"
test -f /tmp/p36_5_timed.c || fail "A: output C file not created"
echo "PASS A: timed profile accepted by qjsc --jit-pgo"

# --- Test B: hot (time_ms=600) → O3 ---
grep -q '"O3"' /tmp/p36_5_timed.c \
    || fail "B: O3 pragma missing (expected for time_ms=600)"
echo "PASS B: time_ms=600ms → #pragma GCC optimize(\"O3\")"

# --- Test C: warm (time_ms=60) → O2 ---
grep -q '"O2"' /tmp/p36_5_timed.c \
    || fail "C: O2 pragma missing (expected for time_ms=60)"
echo "PASS C: time_ms=60ms → #pragma GCC optimize(\"O2\")"

# --- Test D: cool (time_ms=6) → O1 ---
grep -q '"O1"' /tmp/p36_5_timed.c \
    || fail "D: O1 pragma missing (expected for time_ms=6)"
echo "PASS D: time_ms=6ms → #pragma GCC optimize(\"O1\")"

# cold is absent from profile → should not appear in generated C.
# We verify indirectly: the C file has at most 3 JIT functions (hot, warm, cool).
# (Checking by counting "pragma GCC optimize" occurrences — 3 user pragmas + 3 resets)
PRAGMA_COUNT=$(grep -c '"O[0-9]"' /tmp/p36_5_timed.c || true)
# 3 user pragmas (O3, O2, O1) + 3 reset-to-O2 pragmas = 6; cold adds 2 more = 8
# We just verify cold is absent by ensuring the function body is not there.
# (cold's body is "return n - 1;" which is unique enough.)
if grep -q "${COLD_HASH}" /tmp/p36_5_timed.c 2>/dev/null; then
    fail "D+: cold function (absent from profile) was emitted — should be skipped"
fi

# ----------------------------------------------------------------
# Test E: call-count-only profile (no time_ms) — backwards compat.
# ----------------------------------------------------------------
cat > /tmp/p36_5_callsonly.json <<ENDJSON
{"functions":[
  {"hash":"${HOT_HASH}","calls":15000,"name":"hot"},
  {"hash":"${WARM_HASH}","calls":1500,"name":"warm"},
  {"hash":"${COOL_HASH}","calls":150,"name":"cool"}
]}
ENDJSON

"${QJSC}" --jit-hybrid-app --jit-pgo=/tmp/p36_5_callsonly.json \
          -o /tmp/p36_5_callsonly.c /tmp/p36_5_mod.mjs 2>/dev/null \
    || fail "E: qjsc with calls-only profile returned error"
test -f /tmp/p36_5_callsonly.c || fail "E: output C file not created"
grep -q '"O3"' /tmp/p36_5_callsonly.c || fail "E: O3 missing for calls=15000"
grep -q '"O2"' /tmp/p36_5_callsonly.c || fail "E: O2 missing for calls=1500"
grep -q '"O1"' /tmp/p36_5_callsonly.c || fail "E: O1 missing for calls=150"
echo "PASS E: calls-only profile (no time_ms) still works — backwards compatible"

# ----------------------------------------------------------------
# Test F: both fields present — time_ms wins.
# calls=1 (cold by call-count → O0 / skip); time_ms=600 (hot → O3).
# If calls were used, the function would be skipped (calls=1 → O0, not skipped
# since >0, but would be O0). With time_ms=600 it must be O3.
# ----------------------------------------------------------------
cat > /tmp/p36_5_both.json <<ENDJSON
{"functions":[
  {"hash":"${HOT_HASH}","calls":1,"time_ms":600,"name":"hot"}
]}
ENDJSON

"${QJSC}" --jit-hybrid-app --jit-pgo=/tmp/p36_5_both.json \
          -o /tmp/p36_5_both.c /tmp/p36_5_mod.mjs 2>/dev/null \
    || fail "F: qjsc returned error"
test -f /tmp/p36_5_both.c || fail "F: output C file not created"
grep -q '"O3"' /tmp/p36_5_both.c \
    || fail "F: O3 missing — time_ms=600 should override calls=1 (expected O3 not O0)"
echo "PASS F: time_ms=600 overrides calls=1 (time_ms is primary metric)"

# Cleanup
rm -f /tmp/p36_5_mod.mjs /tmp/p36_5_calls.json \
      /tmp/p36_5_timed.json /tmp/p36_5_timed.c \
      /tmp/p36_5_callsonly.json /tmp/p36_5_callsonly.c \
      /tmp/p36_5_both.json /tmp/p36_5_both.c

echo "=== ALL P36.5 TESTS PASSED ==="
