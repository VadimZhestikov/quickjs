QJS_DIR := $(realpath ../..)

CFLAGS := -g -O0 -DCONFIG_JIT -I$(QJS_DIR)
OBJS   := $(QJS_DIR)/.obj/quickjs.o \
           $(QJS_DIR)/.obj/quickjs-jit.o \
           $(QJS_DIR)/.obj/quickjs-libc.o \
           $(QJS_DIR)/.obj/dtoa.o \
           $(QJS_DIR)/.obj/libregexp.o \
           $(QJS_DIR)/.obj/libunicode.o \
           $(QJS_DIR)/.obj/cutils.o
LIBS   := -lm -lpthread -ldl

.PHONY: all run clean

all: /tmp/test_p38_1 /tmp/test_p38_2 /tmp/test_p38_3

/tmp/test_p38_1: test_p38_1.c $(OBJS)
	gcc $(CFLAGS) -rdynamic -o $@ $< $(OBJS) $(LIBS)

/tmp/test_p38_2: test_p38_2.c $(OBJS)
	gcc $(CFLAGS) -rdynamic -o $@ $< $(OBJS) $(LIBS)

/tmp/test_p38_3: test_p38_3.c $(OBJS)
	gcc $(CFLAGS) -rdynamic -o $@ $< $(OBJS) $(LIBS)

run: all
	@echo "=== P38.1: array-obj refcount elision ==="
	@/tmp/test_p38_1
	@echo "=== P38.2: typed index fast path ==="
	@/tmp/test_p38_2
	@echo "=== P38.3: inline array.length ==="
	@/tmp/test_p38_3
	@echo "=== ALL P38 TESTS COMPLETE ==="

clean:
	rm -f /tmp/test_p38_1 /tmp/test_p38_2 /tmp/test_p38_3
