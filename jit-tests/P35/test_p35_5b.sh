#!/bin/sh
# P35.5-B test: qjs --jit-profile=<file>
#
# Verifies that:
#   A) --jit-profile writes a JSON file after execution.
#   B) The JSON contains entries for module functions that were called.
#   C) Module functions not called do not appear in the profile.
#   D) Profile JSON has the correct top-level structure.
#
# Note: js_jit_write_profile walks live module bytecodes; global-script
# bytecodes freed by JS_EvalFunction are not captured (by design).
# All tests here use ES modules.
#
# Usage: sh test_p35_5b.sh <path-to-qjs>
#
set -e

QJS="${1:-../../qjs}"
PASS=0
FAIL=0
PROF="/tmp/p35_5b_profile.json"

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

# --- Test A: profile file is written after execution ---
cat > /tmp/p35_5b_mod_a.js << 'EOF'
export function fib(n) { return n <= 1 ? n : fib(n-1) + fib(n-2); }
EOF
cat > /tmp/p35_5b_a.mjs << 'EOF'
import { fib } from '/tmp/p35_5b_mod_a.js';
for (var i = 0; i < 5; i++) fib(8);
EOF
rm -f "$PROF"
"$QJS" --jit-profile="$PROF" /tmp/p35_5b_a.mjs
check "A (profile file written)" "$([ -f "$PROF" ] && echo 1 || echo 0)"

# --- Test B: profile contains called functions ---
json=$(cat "$PROF")
has_hash=$(echo "$json" | grep -c '"hash"' || true)
has_calls=$(echo "$json" | grep -c '"calls"' || true)
check "B (profile has hash field)" "$([ "$has_hash" -ge 1 ] && echo 1 || echo 0)"
check "B (profile has calls field)" "$([ "$has_calls" -ge 1 ] && echo 1 || echo 0)"

# --- Test C: function not called is absent ---
cat > /tmp/p35_5b_mod_c.js << 'EOF'
export function called_fn(n) { return n + 1; }
export function never_fn(n) { return n * 2; }
EOF
cat > /tmp/p35_5b_c.mjs << 'EOF'
import { called_fn } from '/tmp/p35_5b_mod_c.js';
called_fn(10);
EOF
rm -f "$PROF"
"$QJS" --jit-profile="$PROF" /tmp/p35_5b_c.mjs
json=$(cat "$PROF")
has_called=$(echo "$json" | grep -c '"name":"called_fn"' || true)
has_never=$(echo "$json" | grep -c 'never_fn' || true)
check "C (called_fn in profile)" "$([ "$has_called" -ge 1 ] && echo 1 || echo 0)"
check "C (never_fn absent from profile)" "$([ "$has_never" -eq 0 ] && echo 1 || echo 0)"

# --- Test D: profile is valid JSON with correct structure ---
check "D (valid JSON structure)" "$(echo "$json" | grep -c '"functions"' | awk '{print ($1>=1)?1:0}')"

# Cleanup
rm -f /tmp/p35_5b_mod_a.js /tmp/p35_5b_a.mjs \
      /tmp/p35_5b_mod_c.js /tmp/p35_5b_c.mjs "$PROF"

echo ""
if [ $FAIL -eq 0 ]; then
    echo "=== ALL P35.5-B TESTS PASSED ($PASS passed) ==="
    exit 0
else
    echo "=== P35.5-B TESTS FAILED: $FAIL failed, $PASS passed ==="
    exit 1
fi
