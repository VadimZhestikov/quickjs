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

all: /tmp/test_p35_1 /tmp/test_p35_2

/tmp/test_p35_1: test_p35_1.c
	gcc $(CFLAGS) -rdynamic -o $@ $< $(OBJS) $(LIBS)

/tmp/test_p35_2: test_p35_2.c
	gcc $(CFLAGS) -rdynamic -o $@ $< $(OBJS) $(LIBS)

run: all
	@echo "=== P35.1: bytecode size cap ==="
	@/tmp/test_p35_1
	@echo "=== P35.2: module graph walkers ==="
	@/tmp/test_p35_2
	@echo "=== ALL P35 TESTS COMPLETE ==="

clean:
	rm -f /tmp/test_p35_1 /tmp/test_p35_2
