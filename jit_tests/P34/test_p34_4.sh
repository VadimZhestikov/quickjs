#!/bin/bash
# P34.4 test: qjsc --jit-hybrid end-to-end
#
# Run from quickjs/:
#   bash jit_tests/P34/test_p34_4.sh
set -e
cd "$(dirname "$0")/../.."   # cd to quickjs/

JS=/tmp/p34_4_testmod.js
HYBRID_C=/tmp/p34_4_testmod.c
HYBRID_SO=/tmp/p34_4_testmod.so
NOJIT_SO=/tmp/p34_4_testmod_nojit.so
RUNNER=/tmp/p34_4_run.js

# 1. Create a test module with several functions including recursion
cat > "$JS" << 'JSEOF'
export function add(a, b) { return a + b; }
export function mul(a, b) { return a * b; }
export function fact(n)   { return n <= 1 ? 1 : n * fact(n - 1); }
JSEOF

# 2. Generate hybrid C
./qjsc --jit-hybrid -o "$HYBRID_C" "$JS"
echo "PASS: --jit-hybrid generated C"

# 3. Dispatch table must be non-empty (inner functions present)
TABLE_COUNT=$(grep -c 'ULL, __jit_f_' "$HYBRID_C" || true)
[ "$TABLE_COUNT" -gt 0 ] || { echo "FAIL: dispatch table empty"; exit 1; }
echo "PASS: dispatch table has $TABLE_COUNT JIT function(s)"

# 4. Module body must NOT appear in dispatch table
# (module body is skipped because it uses module-specific opcodes)
EVAL_IN_TABLE=$(grep -c '"<eval>".*_jit_table\|_jit_table.*"<eval>"' "$HYBRID_C" || true)
[ "$EVAL_IN_TABLE" -eq 0 ] || { echo "FAIL: module body in dispatch table"; exit 1; }
echo "PASS: module body excluded from dispatch table"

# 5. Compile with CONFIG_JIT
gcc -O2 -shared -fPIC -DCONFIG_JIT -I. -o "$HYBRID_SO" "$HYBRID_C"
echo "PASS: compiled with CONFIG_JIT"

# 6. Compile without CONFIG_JIT (pure-bytecode fallback)
gcc -O2 -shared -fPIC -I. -o "$NOJIT_SO" "$HYBRID_C"
echo "PASS: compiled without CONFIG_JIT (bytecode fallback)"

# 7. Functional test — JIT version
cat > "$RUNNER" << 'JSEOF'
import { add, mul, fact } from "/tmp/p34_4_testmod.so";
var ok = true;
if (add(2, 3)  !== 5)   { print("FAIL: add");  ok = false; }
if (mul(4, 7)  !== 28)  { print("FAIL: mul");  ok = false; }
if (fact(5)    !== 120) { print("FAIL: fact"); ok = false; }
if (ok) print("PASS: JIT .so results correct");
JSEOF
./qjs -m "$RUNNER"

# 8. Functional test — nojit version
sed "s|p34_4_testmod\.so|p34_4_testmod_nojit.so|g" "$RUNNER" \
    > /tmp/p34_4_run_nojit.js
sed -i 's/JIT .so/nojit .so/' /tmp/p34_4_run_nojit.js
./qjs -m /tmp/p34_4_run_nojit.js

# 9. Run without source .js (key feature: no .js needed at runtime)
rm -f "$JS"
./qjs -m "$RUNNER"
echo "PASS: runs without source .js file"

echo ""
echo "ALL P34.4 TESTS PASSED"
