# jit_tests — top-level JIT test suite runner
#
# Runs all JIT sub-suites in order:
#   C harness tests  (P1, P2, P5, P8, P9, P10, P34, P35)
#   JS level tests   (jit-tests/js/)
#
# Run from quickjs/:
#   make -C jit-tests run
# or from jit-tests/:
#   make run
#
# Prerequisites: a CONFIG_JIT=y build in quickjs/ (make CONFIG_JIT=y).

QJS_DIR := $(realpath ..)

.PHONY: all run clean \
        run-p1 run-p2 run-p5 run-p8 run-p9 run-p10 run-p34 run-p35 run-p37 run-js

all: run

run: run-p1 run-p2 run-p5 run-p8 run-p9 run-p10 run-p34 run-p35 run-p37 run-js
	@echo ""
	@echo "=== ALL JIT TESTS COMPLETE ==="

run-p1:
	@make -C P1 run

run-p2:
	@make -C P2 run

run-p5:
	@make -C P5 run

run-p8:
	@make -C P8 run

run-p9:
	@make -C P9 run

run-p10:
	@make -C P10 run

run-p34:
	@make CONFIG_JIT=y -C P34 run

run-p35:
	@make -C P35 run

run-p37:
	@make -C P37 run

run-js:
	@make -C js run

clean:
	@make -C P1  clean
	@make -C P2  clean
	@make -C P5  clean
	@make -C P8  clean
	@make -C P9  clean
	@make -C P10 clean
	@make -C P34 clean
	@make -C P35 clean
	@make -C P37 clean
