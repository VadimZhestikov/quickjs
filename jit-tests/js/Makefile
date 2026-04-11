# jit_tests/js — JS-level JIT behavioural tests
#
# Each file tests one or more JIT phases end-to-end by running JS code
# that exercises the relevant opcodes and checking computed values.
# Assertions use throw-on-failure; a non-zero exit code means failure.
#
# Run from quickjs/:
#   make -C jit_tests/js run
# or from jit_tests/js/:
#   make run
#
# Prerequisites: a CONFIG_JIT=y qjs build in quickjs/ (make CONFIG_JIT=y).
#
# THRESHOLD controls how quickly functions are JIT-compiled.
# Default 1 means every function compiles after the first call, which is
# the most aggressive and exercises the JIT path in every test.

QJS_DIR   := $(realpath ../..)
QJS       := $(QJS_DIR)/qjs
THRESHOLD ?= 1
QJS_FLAGS := --jit-threshold-gcc=$(THRESHOLD)

JS_TESTS := \
    test_jit_p12_generators.js \
    test_jit_p13_closures.js \
    test_jit_p14_try_catch.js \
    test_jit_p15_iterators.js \
    test_jit_p15b_special_object.js \
    test_jit_p16_delete.js \
    test_jit_p17_apply.js \
    test_jit_p18.js \
    test_jit_p19p20.js \
    test_jit_p21p22p23.js \
    test_jit_p25p26p27p28.js \
    test_jit_p29p30.js \
    test_jit_p31p32.js \
    test_jit_p33.js \
    test_jit_p123_async.js \
    test_jit_p124_closures_async_gen.js \
    test_jit_p43_ic.js \
    test_jit_p43_bitops.js

PASS_COUNT = 0
FAIL_COUNT = 0

.PHONY: all run $(JS_TESTS)

all: run

run: $(JS_TESTS)
	@echo "=== ALL JS JIT TESTS COMPLETE ==="

$(JS_TESTS):
	@printf "%-50s" "$@ ..."
	@if $(QJS) $(QJS_FLAGS) $(CURDIR)/$@ 2>/dev/null; then \
	    echo " PASS"; \
	else \
	    echo " FAIL"; \
	    $(QJS) $(QJS_FLAGS) $(CURDIR)/$@; \
	    exit 1; \
	fi
