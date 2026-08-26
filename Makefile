CFLAGS=-std=gnu99 -g -O2 -Wall -I src

VERSION=$$(git rev-list HEAD --count)

.PHONY: bench clean test test-gc test-mark-sweep test-repl test-wasm

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
	rm -f build/*

test: repl
	@./test.sh

# Line editing, which the shell suite cannot reach: it pipes its input, and the editor
# turns itself off when the input is not a terminal.
test-repl: repl
	@python3 ./test_repl.py ./repl

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
		-D LIB_VERSION=$(VERSION) \
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
