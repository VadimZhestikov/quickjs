#!/bin/sh
# P35.4 test: qjsc --standalone self-contained binary
#
# Verifies that:
#   A) A simple global script compiles to a standalone binary and produces
#      correct output without any source files present at runtime.
#   B) A script with functions produces correct results (JIT tier 2 from
#      first call — verified via correct output, not tier introspection).
#   C) A module with ES import (entry + lib) runs correctly when compiled
#      as standalone; binary works after source files are removed.
#   D) std module (print, scriptArgs) is available in the standalone binary.
#
# Usage: sh test_p35_4.sh <path-to-qjsc>
#
set -e

QJSC="${1:-../../qjsc}"
PASS=0
FAIL=0

check() {
    local tag="$1"; shift
    local expected="$1"; shift
    local actual="$1"
    if [ "$actual" = "$expected" ]; then
        echo "PASS $tag"
        PASS=$((PASS+1))
    else
        echo "FAIL $tag: expected='$expected' got='$actual'"
        FAIL=$((FAIL+1))
    fi
}

# --- Test A: simple global script ---
cat > /tmp/p35_4_a.js << 'EOF'
var x = 6 * 7;
print("A:" + x);
EOF
"$QJSC" --standalone -o /tmp/p35_4_a /tmp/p35_4_a.js
rm /tmp/p35_4_a.js
out=$(/tmp/p35_4_a)
check "A (simple global script)" "A:42" "$out"

# --- Test B: functions produce correct results ---
cat > /tmp/p35_4_b.js << 'EOF'
function fib(n) { return n <= 1 ? n : fib(n-1) + fib(n-2); }
function add(a, b) { return a + b; }
print("B:" + fib(10) + "," + add(3, 4));
EOF
"$QJSC" --standalone -o /tmp/p35_4_b /tmp/p35_4_b.js
rm /tmp/p35_4_b.js
out=$(/tmp/p35_4_b)
check "B (functions correct result)" "B:55,7" "$out"

# --- Test C: ES module with import ---
cat > /tmp/p35_4_clib.js << 'EOF'
export function mul(a, b) { return a * b; }
export function greet(s) { return "Hello " + s; }
EOF
cat > /tmp/p35_4_c.mjs << 'EOF'
import { mul, greet } from '/tmp/p35_4_clib.js';
print("C:" + greet("world") + "," + mul(6, 7));
EOF
"$QJSC" --standalone -o /tmp/p35_4_c /tmp/p35_4_c.mjs
# Remove source files to confirm binary is truly self-contained
rm /tmp/p35_4_clib.js /tmp/p35_4_c.mjs
out=$(/tmp/p35_4_c)
check "C (ES module import, no source files)" "C:Hello world,42" "$out"

# --- Test D: std library available (print works, scriptArgs available) ---
cat > /tmp/p35_4_d.js << 'EOF'
var args = scriptArgs || [];
print("D:ok:" + args.length);
EOF
"$QJSC" --standalone -o /tmp/p35_4_d /tmp/p35_4_d.js
rm /tmp/p35_4_d.js
out=$(/tmp/p35_4_d)
check "D (std library, scriptArgs)" "D:ok:1" "$out"

# Cleanup
rm -f /tmp/p35_4_a /tmp/p35_4_b /tmp/p35_4_c /tmp/p35_4_d

echo ""
if [ $FAIL -eq 0 ]; then
    echo "=== ALL P35.4 TESTS PASSED ($PASS passed) ==="
    exit 0
else
    echo "=== P35.4 TESTS FAILED: $FAIL failed, $PASS passed ==="
    exit 1
fi
