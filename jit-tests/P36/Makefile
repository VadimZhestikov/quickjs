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

all: /tmp/test_p36_1 /tmp/test_p36_2 /tmp/test_p36_3

/tmp/test_p36_1: test_p36_1.c $(OBJS)
	gcc $(CFLAGS) -rdynamic -o $@ $< $(OBJS) $(LIBS)

/tmp/test_p36_2: test_p36_2.c $(OBJS)
	gcc $(CFLAGS) -rdynamic -o $@ $< $(OBJS) $(LIBS)

/tmp/test_p36_3: test_p36_3.c $(OBJS)
	gcc $(CFLAGS) -rdynamic -o $@ $< $(OBJS) $(LIBS)

run: all
	@echo "=== P36.1: JIT address range registry ==="
	@/tmp/test_p36_1
	@echo "=== P36.2: SIGPROF sampler ==="
	@/tmp/test_p36_2
	@echo "=== P36.3: extended profile writer ==="
	@/tmp/test_p36_3
	@echo "=== P36.4: --jit-profile-time CLI flag ==="
	@QJS=$(QJS_DIR)/qjs sh $(QJS_DIR)/jit-tests/P36/test_p36_4.sh
	@echo "=== ALL P36 TESTS COMPLETE ==="

clean:
	rm -f /tmp/test_p36_1 /tmp/test_p36_2 /tmp/test_p36_3
