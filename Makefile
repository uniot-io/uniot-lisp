CFLAGS=-std=gnu99 -g -O2 -Wall

.PHONY: clean test

minilisp: libminilisp.c minilisp.c

clean:
	rm -f minilisp *~

test: minilisp
	@./test.sh
