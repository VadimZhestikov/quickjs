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

all: /tmp/test_p8

/tmp/test_p8: test_p8.c
	gcc $(CFLAGS) -rdynamic -o $@ $< $(OBJS) $(LIBS)

run: all
	@echo "=== P8: js_jit_call() direct JIT dispatch ==="
	@/tmp/test_p8
	@echo "=== ALL P8 TESTS COMPLETE ==="

clean:
	rm -f /tmp/test_p8
