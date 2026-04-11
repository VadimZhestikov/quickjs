CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -I../.. -I../../.obj
LDFLAGS = ../../libquickjs.a -lm -ldl -lpthread

TESTS = test_p39_1 test_p39_2 test_p39_3

all: $(TESTS)

%: %.c
	$(CC) $(CFLAGS) $< $(LDFLAGS) -o $@

run: all
	@passed=0; failed=0; \
	for t in $(TESTS); do \
	    if ./$$t; then passed=$$((passed+1)); \
	    else echo "  FAIL: $$t"; failed=$$((failed+1)); fi; \
	done; \
	echo "P39: $$passed passed, $$failed failed"

clean:
	rm -f $(TESTS)
