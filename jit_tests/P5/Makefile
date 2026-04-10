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

all: /tmp/test_p5

/tmp/test_p5: test_p5.c
	gcc $(CFLAGS) -o $@ $< $(OBJS) $(LIBS)

run: all
	@echo "=== P5: IC fill/check/read/write ==="
	@/tmp/test_p5
	@echo "=== ALL P5 TESTS COMPLETE ==="

clean:
	rm -f /tmp/test_p5
