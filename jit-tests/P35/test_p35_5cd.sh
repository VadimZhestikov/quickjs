#!/bin/sh
# P35.5-C+D test: qjsc --jit-pgo=<file> PGO filtering and pragma emission
#
# Verifies that:
#   A) With --jit-pgo, the generated C is produced and functions are included.
#   B) Called functions appear in generated C.
#   C) High-call-count functions get a higher-level #pragma GCC optimize.
#   D) Low-call-count functions get a lower-level #pragma GCC optimize.
#   E) The generated C compiles cleanly (no GCC errors).
#   F) PGO filter: a function absent from the profile is excluded from generated C.
#
# Profile collection note: jit_call_count stops incrementing after the JIT
# threshold is reached and GCC compilation is queued.  To collect true call
# counts for PGO, use --jit-threshold-gcc=<large-N> so the count accumulates.
#
# Usage: sh test_p35_5cd.sh <path-to-qjs> <path-to-qjsc>
#
set -e

QJS="${1:-../../qjs}"
QJSC="${2:-../../qjsc}"
PASS=0
FAIL=0
PROF="/tmp/p35_5cd_profile.json"

check() {
    local tag="$1" cond="$2"
    if [ "$cond" = "1" ]; then
        echo "PASS $tag"
        PASS=$((PASS+1))
    else
        echo "FAIL $tag"
        FAIL=$((FAIL+1))
    fi
}

# Write a module with two functions: hot (called 20000x) and cold (called 1x).
# Use --jit-threshold-gcc=1000000 so the call count is not capped at the JIT
# threshold during the profiling run.
cat > /tmp/p35_5cd_mod.js << 'EOF'
export function hot(n) { return n + 1; }
export function cold(n) { return n * 2; }
EOF

cat > /tmp/p35_5cd_entry.mjs << 'EOF'
import { hot, cold } from '/tmp/p35_5cd_mod.js';
for (var i = 0; i < 20000; i++) hot(i);
cold(1);
EOF

# Collect profile with a high JIT threshold so counts accumulate
rm -f "$PROF"
"$QJS" --jit-threshold-gcc=1000000 --jit-profile="$PROF" /tmp/p35_5cd_entry.mjs

# Verify profile was written and contains expected entries
if [ ! -f "$PROF" ]; then
    echo "FAIL: profile not written"
    exit 1
fi

json=$(cat "$PROF")

# hot should have calls >= 10000 (to get O3 pragma); cold should have calls = 1
hot_calls=$(echo "$json" | grep '"name":"hot"' | grep -oE '"calls":[0-9]+' | grep -oE '[0-9]+' || echo 0)
cold_calls=$(echo "$json" | grep '"name":"cold"' | grep -oE '"calls":[0-9]+' | grep -oE '[0-9]+' || echo 0)

if [ -z "$hot_calls" ] || [ "$hot_calls" -lt 10000 ]; then
    echo "INFO: hot_calls=$hot_calls cold_calls=$cold_calls"
    echo "INFO: profile: $(cat "$PROF")"
    echo "FAIL: hot function call count too low (got $hot_calls, need >=10000)"
    # Don't exit — continue to show which other tests pass/fail
    hot_ok=0
else
    hot_ok=1
fi
if [ -z "$cold_calls" ] || [ "$cold_calls" -ne 1 ]; then
    cold_ok=0
else
    cold_ok=1
fi
check "pre (hot calls >= 10000)" "$hot_ok"
check "pre (cold calls == 1)" "$cold_ok"

# Generate C with --jit-hybrid-app + --jit-pgo
OUT_C="/tmp/p35_5cd_out.c"
"$QJSC" --jit-hybrid-app --jit-pgo="$PROF" -o "$OUT_C" \
    /tmp/p35_5cd_mod.js /tmp/p35_5cd_entry.mjs

# --- Test A: generated C file exists ---
check "A (generated C exists)" "$([ -f "$OUT_C" ] && echo 1 || echo 0)"

# --- Test B: called functions appear in generated C ---
has_jit_fn=$(grep -c '__jit_f_' "$OUT_C" || true)
check "B (JIT function bodies present)" "$([ "$has_jit_fn" -ge 1 ] && echo 1 || echo 0)"

# --- Test C: hot function gets O3 pragma (calls >= 10000) ---
has_o3=$(grep -c '"O3"' "$OUT_C" || true)
check "C (O3 pragma for hot function)" "$([ "$has_o3" -ge 1 ] && echo 1 || echo 0)"

# --- Test D: cold function gets O0 pragma (calls == 1) ---
has_o0=$(grep -c '"O0"' "$OUT_C" || true)
check "D (O0 pragma for cold function)" "$([ "$has_o0" -ge 1 ] && echo 1 || echo 0)"

# --- Test E: generated C compiles without errors ---
INC_DIR="$(dirname "$QJSC")"
if gcc -g -O2 -DCONFIG_JIT -I"$INC_DIR" -c "$OUT_C" -o /tmp/p35_5cd_out.o 2>/tmp/p35_5cd_gcc.err; then
    check "E (generated C compiles)" "1"
else
    echo "GCC errors:"
    cat /tmp/p35_5cd_gcc.err
    check "E (generated C compiles)" "0"
fi

# --- Test F: PGO filter — module with un-profiled function generates no JIT body ---
# Create a new module whose functions are NOT in the profile
cat > /tmp/p35_5cd_new_mod.js << 'EOF'
export function unknown_fn(n) { return n * n; }
EOF
OUT_F="/tmp/p35_5cd_f_out.c"
"$QJSC" --jit-hybrid-app --jit-pgo="$PROF" -o "$OUT_F" \
    /tmp/p35_5cd_new_mod.js 2>/dev/null || true
if [ -f "$OUT_F" ]; then
    has_jit=$(grep -c '__jit_f_' "$OUT_F" || true)
    check "F (un-profiled function absent from PGO output)" \
          "$([ "$has_jit" -eq 0 ] && echo 1 || echo 0)"
else
    check "F (PGO output file created)" "0"
fi

# Cleanup
rm -f /tmp/p35_5cd_mod.js /tmp/p35_5cd_entry.mjs \
      /tmp/p35_5cd_new_mod.js "$PROF" "$OUT_C" "$OUT_F" \
      /tmp/p35_5cd_out.o /tmp/p35_5cd_gcc.err

echo ""
if [ $FAIL -eq 0 ]; then
    echo "=== ALL P35.5-C+D TESTS PASSED ($PASS passed) ==="
    exit 0
else
    echo "=== P35.5-C+D TESTS FAILED: $FAIL failed, $PASS passed ==="
    exit 1
fi
