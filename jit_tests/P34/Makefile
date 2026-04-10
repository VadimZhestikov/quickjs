# P34 JIT test suite — run from quickjs/ via:
#   make -C jit_tests/P34
# or from jit_tests/P34/ via:
#   make
#
# Requires a CONFIG_JIT build:   make CONFIG_JIT=y (in quickjs/)

QJS_DIR := $(realpath ../..)

CFLAGS := -g -O0 -DCONFIG_JIT -I$(QJS_DIR) -DJIT_INCLUDE_DIR='"$(QJS_DIR)"'
OBJS   := $(QJS_DIR)/.obj/quickjs.o \
           $(QJS_DIR)/.obj/quickjs-jit.o \
           $(QJS_DIR)/.obj/quickjs-libc.o \
           $(QJS_DIR)/.obj/dtoa.o \
           $(QJS_DIR)/.obj/libregexp.o \
           $(QJS_DIR)/.obj/libunicode.o \
           $(QJS_DIR)/.obj/cutils.o
LIBS   := -lm -lpthread -ldl

BINS := /tmp/test_p34_1 /tmp/test_p34_2 /tmp/test_p34_3

.PHONY: all run clean

all: $(BINS)

/tmp/test_p34_1: test_p34_1.c
	gcc $(CFLAGS) -o $@ $< $(OBJS) $(LIBS)

/tmp/test_p34_2: test_p34_2.c
	gcc $(CFLAGS) -o $@ $< $(OBJS) $(LIBS)

/tmp/test_p34_3: test_p34_3.c
	gcc $(CFLAGS) -o $@ $< $(OBJS) $(LIBS)

run: all
	@echo "=== P34.1: js_jit_hash_bytecode_pub ==="
	@rm -f ~/.cache/qjs-jit/*.so ~/.cache/qjs-jit/*.c ~/.cache/qjs-jit/*.skip
	@$(QJS_DIR)/qjs --jit-threshold-gcc=1 \
	    -e "function add(a,b){return a+b;} for(var i=0;i<5;i++) add(i,i+1);" \
	    2>/dev/null || true
	@/tmp/test_p34_1
	@echo ""
	@echo "=== P34.2: js_jit_walk_bytecodes ==="
	@/tmp/test_p34_2
	@echo ""
	@echo "=== P34.3: js_jit_gen_c_str ==="
	@/tmp/test_p34_3
	@echo ""
	@echo "=== P34.4: qjsc --jit-hybrid ==="
	@bash test_p34_4.sh
	@echo ""
	@echo "=== ALL P34 TESTS COMPLETE ==="

clean:
	rm -f $(BINS) /tmp/p34_4_* /tmp/test_p34_3_*
