CFLAGS=-std=gnu99 -g -O2 -Wall -I src

.PHONY: bench clean test test-32 test-gc test-mark-sweep test-repl test-wasm

EMSDK_VERSION=4.0.22

emsdk:
	git clone https://github.com/emscripten-core/emsdk.git
	(cd ./emsdk && ./emsdk install $(EMSDK_VERSION))
	(cd ./emsdk && ./emsdk activate $(EMSDK_VERSION))

env: 
	$(info RUN THIS COMMAND:)
	$(info )
	$(info source ./emsdk/emsdk_env.sh --build=Release)
	$(info )

# The REPL is the test harness, so it is built with the heap verifier compiled in.
repl: repl.c line_edit.c line_edit.h src/libminilisp.c src/libminilisp.h
	$(CC) $(CFLAGS) -D MINILISP_GC_DEBUG=1 -o $@ repl.c line_edit.c src/libminilisp.c

# The same interpreter over the other collector. Everything above the strategy seam is
# shared, so whatever the suite asks of one has to hold for the other. Both are named
# explicitly rather than one relying on the default, so that changing the default cannot
# quietly leave the matrix testing the same collector twice.
repl-copying: repl.c line_edit.c line_edit.h src/libminilisp.c src/libminilisp.h
	$(CC) $(CFLAGS) -D MINILISP_GC_DEBUG=1 -D MINILISP_GC=MINILISP_GC_COPYING -o $@ repl.c line_edit.c src/libminilisp.c

repl-mark-sweep: repl.c line_edit.c line_edit.h src/libminilisp.c src/libminilisp.h
	$(CC) $(CFLAGS) -D MINILISP_GC_DEBUG=1 -D MINILISP_GC=MINILISP_GC_MARK_SWEEP -o $@ repl.c line_edit.c src/libminilisp.c

# The work stack cut to a single entry, so that every collection overflows it and the
# recovery path runs over and over instead of only being entered once.
repl-mark-sweep-small: repl.c line_edit.c line_edit.h src/libminilisp.c src/libminilisp.h
	$(CC) $(CFLAGS) -D MINILISP_GC_DEBUG=1 -D MINILISP_GC=MINILISP_GC_MARK_SWEEP \
		-D MINILISP_GC_MARK_STACK=1 -o $@ repl.c line_edit.c src/libminilisp.c

# The strategy is fixed when the library is compiled, so comparing two of them takes two
# binaries. Built without the verifier: it would charge one strategy for work the other
# does not do, and the point here is the cost of the real thing.
BENCH_CFLAGS=$(CFLAGS) -D MINILISP_GC_STATS=1

bench-copying: bench.c src/libminilisp.c src/libminilisp.h
	$(CC) $(BENCH_CFLAGS) -D MINILISP_GC=MINILISP_GC_COPYING -o $@ bench.c src/libminilisp.c

bench-mark-sweep: bench.c src/libminilisp.c src/libminilisp.h
	$(CC) $(BENCH_CFLAGS) -D MINILISP_GC=MINILISP_GC_MARK_SWEEP -o $@ bench.c src/libminilisp.c

bench: bench-copying bench-mark-sweep
	@./bench.sh

clean:
	rm -f bench-copying
	rm -f bench-mark-sweep
	rm -f repl
	rm -f repl-copying
	rm -f repl-mark-sweep
	rm -f repl-mark-sweep-small
	rm -f repl32
	rm -f repl32-copying
	rm -f repl32-mark-sweep
	rm -f build/*

test: repl
	@./test.sh

# Line editing, which the shell suite cannot reach: it pipes its input, and the editor
# turns itself off when the input is not a terminal.
test-repl: repl
	@python3 ./test_repl.py ./repl

# A 32-bit host build, which is the only way this suite can run against the object layout
# the devices actually use. A pointer is four bytes there, so the header is four and not
# eight, an integer is 8 bytes and not 12, a cell is 12 and not 24 -- the same numbers as
# an ESP8266 and as wasm32. On a 64-bit host every object is twice the size, so a heap of
# the size a device really has cannot hold what the tests ask of it, and the configuration
# that ships was untestable here.
#
# Needs a 32-bit libc: apt install gcc-multilib libc6-dev-i386. Skipped rather than failed
# where it is absent, since nothing else in this repository wants it.
HAVE_M32 := $(shell printf 'int main(void){return 0;}' > /tmp/.m32probe.c 2>/dev/null && \
	$(CC) -m32 /tmp/.m32probe.c -o /tmp/.m32probe >/dev/null 2>&1 && echo yes)

repl32: repl.c line_edit.c line_edit.h src/libminilisp.c src/libminilisp.h
	$(CC) -m32 $(CFLAGS) -D MINILISP_GC_DEBUG=1 -o $@ repl.c line_edit.c src/libminilisp.c

repl32-copying: repl.c line_edit.c line_edit.h src/libminilisp.c src/libminilisp.h
	$(CC) -m32 $(CFLAGS) -D MINILISP_GC_DEBUG=1 -D MINILISP_GC=MINILISP_GC_COPYING -o $@ repl.c line_edit.c src/libminilisp.c

repl32-mark-sweep: repl.c line_edit.c line_edit.h src/libminilisp.c src/libminilisp.h
	$(CC) -m32 $(CFLAGS) -D MINILISP_GC_DEBUG=1 -D MINILISP_GC=MINILISP_GC_MARK_SWEEP -o $@ repl.c line_edit.c src/libminilisp.c

# The heap the firmware runs with. This is the point of the 32-bit build: at 64 bits the
# same suite runs out of memory here, and does so for a reason that does not exist on the
# hardware.
DEVICE_HEAP ?= 7000

test-32:
ifeq ($(HAVE_M32),yes)
	@$(MAKE) --no-print-directory repl32 repl32-copying repl32-mark-sweep
	@echo "--- 32-bit, copying ---"                && REPL=./repl32-copying ./test.sh
	@echo "--- 32-bit, copying, verified ---"      && MINILISP_VERIFY_GC=1 REPL=./repl32-copying ./test.sh
	@echo "--- 32-bit, mark-sweep ---"             && REPL=./repl32-mark-sweep ./test.sh
	@echo "--- 32-bit, mark-sweep, verified ---"   && MINILISP_VERIFY_GC=1 REPL=./repl32-mark-sweep ./test.sh
	@echo "--- 32-bit, mark-sweep, $(DEVICE_HEAP) byte heap ---" && \
		MINILISP_HEAP_SIZE=$(DEVICE_HEAP) REPL=./repl32-mark-sweep ./test.sh
	@python3 ./test_repl.py ./repl32
else
	@echo "skipped: no 32-bit libc (apt install gcc-multilib libc6-dev-i386)"
endif

test-mark-sweep: repl-mark-sweep
	@REPL=./repl-mark-sweep ./test.sh

# Every strategy, plainly and with the heap checked after each collection. Running the
# suite without verification still matters: it is the only way a failure shows up as a
# wrong answer rather than as an abort, which is what a user would actually see.
test-gc: repl-copying repl-mark-sweep repl-mark-sweep-small
	@echo "--- copying ---"                 && REPL=./repl-copying ./test.sh
	@echo "--- copying, verified ---"       && MINILISP_VERIFY_GC=1 REPL=./repl-copying ./test.sh
	@echo "--- mark-sweep ---"              && REPL=./repl-mark-sweep ./test.sh
	@echo "--- mark-sweep, verified ---"    && MINILISP_VERIFY_GC=1 REPL=./repl-mark-sweep ./test.sh
	@echo "--- mark-sweep, 1-entry stack ---" && MINILISP_VERIFY_GC=1 REPL=./repl-mark-sweep-small ./test.sh

# Sourcing emsdk_env.sh puts the emsdk root on PATH, and that directory contains a
# `node` subdirectory which shadows the interpreter for PATH lookups. Resolve a regular
# executable file rather than trusting the first hit.
NODE ?= $(shell for d in $$(echo $$PATH | tr ':' ' '); do if [ -f "$$d/node" ] && [ -x "$$d/node" ]; then echo "$$d/node"; break; fi; done)

test-wasm: wasm
	@test -n "$(NODE)" || { echo "node not found in PATH"; exit 1; }
	@$(NODE) test_wasm.cjs

server:
	emrun --no_browser --port 8000 .


wasm:
	mkdir -p build
	emcc -O3 \
		-I src \
		-s WASM=1 \
		-s EXPORTED_FUNCTIONS='["_malloc", "_free"]' \
		-s ASYNCIFY \
		-s INITIAL_MEMORY=32MB \
		-s 'ASYNCIFY_IMPORTS=["js_handle_lisp"]' \
		-s EXPORTED_RUNTIME_METHODS='["cwrap", "AsciiToString", "writeAsciiToMemory"]' \
		src/libminilisp.c wasm.c \
		-o build/unlisp.js

	cp build/unlisp.js build/unlisp-local.js
	echo 'export default Module' >> build/unlisp.js
	echo '/* eslint-disable */' | cat - build/unlisp.js > build/unlisp.js.tmp && mv build/unlisp.js.tmp build/unlisp.js

# useful options
# -s ABORTING_MALLOC=0
# -s ALLOW_MEMORY_GROWTH=1
# -s BINARYEN_MEM_MAX=2147418112
# -s ASYNCIFY
