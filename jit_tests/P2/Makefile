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

all: /tmp/test_p2

/tmp/test_p2: test_p2.c
	gcc $(CFLAGS) -rdynamic -o $@ $< $(OBJS) $(LIBS)

run: all
	@echo "=== P2: basic arithmetic JIT correctness ==="
	@/tmp/test_p2
	@echo "=== ALL P2 TESTS COMPLETE ==="

clean:
	rm -f /tmp/test_p2
