# Every build product goes to build/, which is the one directory `clean` removes and
# the only one the release workflow reads. Sources live where they belong: the
# interpreter in src/, the REPL and benchmarks under tools/, the WebAssembly target
# in wasm/, the suites in test/.
B = build
CFLAGS=-std=gnu99 -g -O2 -Wall -I src

REPL_SRC  = tools/repl/repl.c tools/repl/line_edit.c
REPL_DEPS = $(REPL_SRC) tools/repl/line_edit.h src/libminilisp.c src/libminilisp.h \
            src/gc_copying.inc src/gc_marksweep.inc
LIB       = src/libminilisp.c

$(B):
	@mkdir -p $(B)

# `wasm` names both a target and a directory, so without this Make sees the directory,
# decides the target is up to date and builds nothing.
.PHONY: bench check-version clean fuzz server test test-32 test-gc test-mark-sweep test-repl \
        test-san test-wasm wasm

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
$(B)/repl: $(REPL_DEPS) | $(B)
	$(CC) $(CFLAGS) -D MINILISP_GC_DEBUG=1 -o $@ $(REPL_SRC) $(LIB)

# The same interpreter over the other collector. Everything above the strategy seam is
# shared, so whatever the suite asks of one has to hold for the other. Both are named
# explicitly rather than one relying on the default, so that changing the default cannot
# quietly leave the matrix testing the same collector twice.
$(B)/repl-copying: $(REPL_DEPS) | $(B)
	$(CC) $(CFLAGS) -D MINILISP_GC_DEBUG=1 -D MINILISP_GC=MINILISP_GC_COPYING -o $@ $(REPL_SRC) $(LIB)

$(B)/repl-mark-sweep: $(REPL_DEPS) | $(B)
	$(CC) $(CFLAGS) -D MINILISP_GC_DEBUG=1 -D MINILISP_GC=MINILISP_GC_MARK_SWEEP -o $@ $(REPL_SRC) $(LIB)

# The work stack cut to a single entry, so that every collection overflows it and the
# recovery path runs over and over instead of only being entered once.
$(B)/repl-mark-sweep-small: $(REPL_DEPS) | $(B)
	$(CC) $(CFLAGS) -D MINILISP_GC_DEBUG=1 -D MINILISP_GC=MINILISP_GC_MARK_SWEEP \
		-D MINILISP_GC_MARK_STACK=1 -o $@ $(REPL_SRC) $(LIB)

# The strategy is fixed when the library is compiled, so comparing two of them takes two
# binaries. Built without the verifier: it would charge one strategy for work the other
# does not do, and the point here is the cost of the real thing.
BENCH_CFLAGS=$(CFLAGS) -D MINILISP_GC_STATS=1

$(B)/bench-copying: tools/bench/bench.c $(LIB) src/libminilisp.h | $(B)
	$(CC) $(BENCH_CFLAGS) -D MINILISP_GC=MINILISP_GC_COPYING -o $@ tools/bench/bench.c $(LIB)

$(B)/bench-mark-sweep: tools/bench/bench.c $(LIB) src/libminilisp.h | $(B)
	$(CC) $(BENCH_CFLAGS) -D MINILISP_GC=MINILISP_GC_MARK_SWEEP -o $@ tools/bench/bench.c $(LIB)

bench: $(B)/bench-copying $(B)/bench-mark-sweep
	@./tools/bench/bench.sh

clean:
	rm -rf $(B)

# Both files that state the version must agree, and match the tag when one is given.
check-version:
	@./test/check_version.sh $(VERSION)

test: $(B)/repl
	@./test/check_version.sh
	@./test/test.sh

# Differential fuzzing. There is no oracle for what a generated program should print,
# and writing one would mean writing a second interpreter. What there is instead is four
# builds that must agree -- two collectors, two word sizes, and a sanitizer build -- so a
# program any two of them disagree on has found a bug with no expected answer written
# down. Verified non-vacuous against a deliberately broken build.
fuzz: $(B)/repl-mark-sweep $(B)/repl-copying $(B)/repl32 $(B)/repl-san
	@python3 ./test/fuzz.py $(N)

N ?= 300

# Line editing, which the shell suite cannot reach: it pipes its input, and the editor
# turns itself off when the input is not a terminal.
test-repl: $(B)/repl
	@python3 ./test/test_repl.py $(B)/repl

# The suite under AddressSanitizer and UndefinedBehaviorSanitizer. UBSan is the one
# that matters here: the arithmetic used to overflow, which is undefined, and the check
# that was supposed to catch it multiplied first and divided back to see -- so the
# compiler was entitled to assume it had not happened and delete the test, which it did
# at every optimisation level above -O0. UBSan reports it where -Wall cannot.
#
# The eval stack budget is raised because ASan's redzones make every C frame larger, so
# the two tests that pin it to an ESP8266's 2048 bytes would fail for the wrong reason.
$(B)/repl-san: $(REPL_DEPS) | $(B)
	$(CC) -std=gnu99 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
		-I src -D MINILISP_GC_DEBUG=1 -o $@ $(REPL_SRC) $(LIB)

test-san: $(B)/repl-san
	@MINILISP_SANITIZED=1 MINILISP_MAX_EVAL_STACK=1048576 REPL=$(B)/repl-san ./test/test.sh
	@for f in examples/*.lisp; do \
		MINILISP_MAX_EVAL_STACK=1048576 $(B)/repl-san < $$f > /dev/null 2>/tmp/.san.$$$$ || true; \
		if grep -q "runtime error\|AddressSanitizer" /tmp/.san.$$$$; then \
			echo "$$f:"; cat /tmp/.san.$$$$; fi; rm -f /tmp/.san.$$$$; \
	done
	@echo "no sanitizer diagnostics"

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

$(B)/repl32: $(REPL_DEPS) | $(B)
	$(CC) -m32 $(CFLAGS) -D MINILISP_GC_DEBUG=1 -o $@ $(REPL_SRC) $(LIB)

$(B)/repl32-copying: $(REPL_DEPS) | $(B)
	$(CC) -m32 $(CFLAGS) -D MINILISP_GC_DEBUG=1 -D MINILISP_GC=MINILISP_GC_COPYING -o $@ $(REPL_SRC) $(LIB)

$(B)/repl32-mark-sweep: $(REPL_DEPS) | $(B)
	$(CC) -m32 $(CFLAGS) -D MINILISP_GC_DEBUG=1 -D MINILISP_GC=MINILISP_GC_MARK_SWEEP -o $@ $(REPL_SRC) $(LIB)

# The heap the firmware runs with. This is the point of the 32-bit build: at 64 bits the
# same suite runs out of memory here, and does so for a reason that does not exist on the
# hardware.
DEVICE_HEAP ?= 7000

test-32:
ifeq ($(HAVE_M32),yes)
	@$(MAKE) --no-print-directory $(B)/repl32 $(B)/repl32-copying $(B)/repl32-mark-sweep
	@echo "--- 32-bit, copying ---"                && REPL=$(B)/repl32-copying ./test/test.sh
	@echo "--- 32-bit, copying, verified ---"      && MINILISP_VERIFY_GC=1 REPL=$(B)/repl32-copying ./test/test.sh
	@echo "--- 32-bit, mark-sweep ---"             && REPL=$(B)/repl32-mark-sweep ./test/test.sh
	@echo "--- 32-bit, mark-sweep, verified ---"   && MINILISP_VERIFY_GC=1 REPL=$(B)/repl32-mark-sweep ./test/test.sh
	@echo "--- 32-bit, mark-sweep, $(DEVICE_HEAP) byte heap ---" && \
		MINILISP_HEAP_SIZE=$(DEVICE_HEAP) REPL=$(B)/repl32-mark-sweep ./test/test.sh
	@python3 ./test/test_repl.py $(B)/repl32
else
	@echo "skipped: no 32-bit libc (apt install gcc-multilib libc6-dev-i386)"
endif

test-mark-sweep: $(B)/repl-mark-sweep
	@REPL=$(B)/repl-mark-sweep ./test/test.sh

# Every strategy, plainly and with the heap checked after each collection. Running the
# suite without verification still matters: it is the only way a failure shows up as a
# wrong answer rather than as an abort, which is what a user would actually see.
test-gc: $(B)/repl-copying $(B)/repl-mark-sweep $(B)/repl-mark-sweep-small
	@echo "--- copying ---"                 && REPL=$(B)/repl-copying ./test/test.sh
	@echo "--- copying, verified ---"       && MINILISP_VERIFY_GC=1 REPL=$(B)/repl-copying ./test/test.sh
	@echo "--- mark-sweep ---"              && REPL=$(B)/repl-mark-sweep ./test/test.sh
	@echo "--- mark-sweep, verified ---"    && MINILISP_VERIFY_GC=1 REPL=$(B)/repl-mark-sweep ./test/test.sh
	@echo "--- mark-sweep, 1-entry stack ---" && MINILISP_VERIFY_GC=1 REPL=$(B)/repl-mark-sweep-small ./test/test.sh

# Sourcing emsdk_env.sh puts the emsdk root on PATH, and that directory contains a
# `node` subdirectory which shadows the interpreter for PATH lookups. Resolve a regular
# executable file rather than trusting the first hit.
NODE ?= $(shell for d in $$(echo $$PATH | tr ':' ' '); do if [ -f "$$d/node" ] && [ -x "$$d/node" ]; then echo "$$d/node"; break; fi; done)

test-wasm: wasm
	@test -n "$(NODE)" || { echo "node not found in PATH"; exit 1; }
	@$(NODE) test/test_wasm.cjs

server:
	emrun --no_browser --port 8000 wasm


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
		--pre-js wasm/pre.js \
		$(LIB) wasm/wasm.c \
		-o build/unlisp.js

	cp build/unlisp.js build/unlisp-local.js
	echo 'export default Module' >> build/unlisp.js
	echo '/* eslint-disable */' | cat - build/unlisp.js > build/unlisp.js.tmp && mv build/unlisp.js.tmp build/unlisp.js

# useful options
# -s ABORTING_MALLOC=0
# -s ALLOW_MEMORY_GROWTH=1
# -s BINARYEN_MEM_MAX=2147418112
# -s ASYNCIFY
