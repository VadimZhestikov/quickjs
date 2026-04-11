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

all: /tmp/test_p37_1 /tmp/test_p37_2 /tmp/test_p37_3 /tmp/test_p37_4

/tmp/test_p37_1: test_p37_1.c $(OBJS)
	gcc $(CFLAGS) -rdynamic -o $@ $< $(OBJS) $(LIBS)

/tmp/test_p37_2: test_p37_2.c $(OBJS)
	gcc $(CFLAGS) -rdynamic -o $@ $< $(OBJS) $(LIBS)

/tmp/test_p37_3: test_p37_3.c $(OBJS)
	gcc $(CFLAGS) -rdynamic -o $@ $< $(OBJS) $(LIBS)

/tmp/test_p37_4: test_p37_4.c $(OBJS)
	gcc $(CFLAGS) -rdynamic -o $@ $< $(OBJS) $(LIBS)

run: all
	@echo "=== P37.1: atom check removal ==="
	@/tmp/test_p37_1
	@echo "=== P37.2: cross-runtime guard ==="
	@/tmp/test_p37_2
	@echo "=== P37.3: refcount correctness ==="
	@/tmp/test_p37_3
	@echo "=== P37.4: bimorphic IC ==="
	@/tmp/test_p37_4
	@echo "=== ALL P37 TESTS COMPLETE ==="

clean:
	rm -f /tmp/test_p37_1 /tmp/test_p37_2 /tmp/test_p37_3 /tmp/test_p37_4
