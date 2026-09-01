#!/bin/bash
# T0 — reproducible measurement of JIT-introduced test262 failures.
#
# For each target directory, runs test262 with the JIT forced on
# (--jit-threshold-gcc=1: compile on 1st call, JIT from 2nd) and reports the
# errors NOT already in the interpreter baseline (test262_errors.txt, 72 known)
# — i.e. the JIT delta ("N new"). A threshold=0 control run confirms the dir is
# clean interpreted. Aggregates a categorized report.
#
# Usage: ./t0-measure-jit.sh [dir ...]      (defaults to the COMCON-relevant set)
# Env:   PER_DIR_TIMEOUT (default 200s), OUT (report path), NO_CONTROL=1 to skip
#        the interpreter control run.
#
# NOTE: threshold=1 leaves a function's FIRST call interpreted, so single-call
# test bodies can mask a first-call-only miscompile. The exhaustive variant is a
# --jit-link warmup -> --jit-aot sweep (every call JIT'd); this target is the
# fast per-directory delta that catches the common (multi-call / crash-masked)
# cases. Requires: make CONFIG_JIT=y run-test262.
set -u
cd "$(dirname "$0")"

RT=./run-test262
CONF=test262.conf
TIMEOUT="${PER_DIR_TIMEOUT:-200}"
OUT="${OUT:-jit-test262-results/t0-delta.txt}"
mkdir -p "$(dirname "$OUT")"

if [ ! -x "$RT" ]; then echo "build first: make CONFIG_JIT=y run-test262"; exit 2; fi

# Default target families: the previously-crashing + COMCON-relevant surface.
DIRS=("$@")
if [ ${#DIRS[@]} -eq 0 ]; then
  DIRS=(
    built-ins/TypedArray/prototype/map   built-ins/TypedArray/prototype/set
    built-ins/TypedArray/prototype/fill  built-ins/TypedArray/prototype/forEach
    built-ins/TypedArray/prototype/filter built-ins/TypedArray/prototype/some
    built-ins/Array/from                 built-ins/Array/prototype/some
    built-ins/Array/prototype/map        built-ins/Array/prototype/reduce
    built-ins/Array/prototype/filter     built-ins/Array/prototype/sort
    built-ins/Map/prototype              built-ins/DataView/prototype
    built-ins/Promise/all                built-ins/Atomics
    language/expressions/arrow-function  language/statements/for-of
  )
fi

# Clear the JIT cache so results reflect the CURRENT binary, not a stale build.
rm -f ~/.cache/qjs-jit/* 2>/dev/null

: > "$OUT"
total_new=0
echo "# T0 JIT delta measurement — $(date -u +%FT%TZ)" | tee -a "$OUT"
echo "# binary: $(git rev-parse --short HEAD 2>/dev/null) ; baseline: test262_errors.txt ($(wc -l < test262_errors.txt) known)" | tee -a "$OUT"
echo "# columns: NEW  (control)  DIR" | tee -a "$OUT"

for d in "${DIRS[@]}"; do
  T="test262/test/$d"
  if [ ! -d "$T" ]; then printf '  %-6s %-10s %s (MISSING)\n' "-" "" "$d" | tee -a "$OUT"; continue; fi
  jerr=$(timeout "$TIMEOUT" "$RT" -c "$CONF" --jit-threshold-gcc=1 -d "$T" 2>/tmp/t0.err 1>/tmp/t0.out); jrc=$?
  new=$(grep -oE '[0-9]+ new' /tmp/t0.err | tail -1 | grep -oE '[0-9]+'); new=${new:-0}
  if [ $jrc -ne 0 ] && [ $jrc -ne 1 ]; then new="CRASH/rc=$jrc"; fi
  ctrl="skip"
  if [ "${NO_CONTROL:-0}" != "1" ]; then
    timeout "$TIMEOUT" "$RT" -c "$CONF" --jit-threshold-gcc=0 -d "$T" 2>/tmp/t0c.err 1>/dev/null
    ctrl=$(grep -oE '[0-9]+ new' /tmp/t0c.err | tail -1 | grep -oE '[0-9]+'); ctrl="int+${ctrl:-0}"
  fi
  printf '  %-6s %-10s %s\n' "$new" "($ctrl)" "$d" | tee -a "$OUT"
  # append the new-error test paths for this dir
  grep -E '\.js' /tmp/t0.out | sed 's/^/      /' >> "$OUT"
  case "$new" in ''|*[!0-9]*) : ;; *) total_new=$((total_new+new));; esac
done
echo "# TOTAL JIT-new (numeric dirs only): $total_new" | tee -a "$OUT"
echo "# report: $OUT"
