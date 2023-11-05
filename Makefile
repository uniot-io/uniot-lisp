CFLAGS=-std=gnu99 -g -O2 -Wall -I src

VERSION=$$(git rev-list HEAD --count)

.PHONY: clean test

emsdk:
	git clone https://github.com/emscripten-core/emsdk.git
	(cd ./emsdk && ./emsdk install latest)
	(cd ./emsdk && ./emsdk activate latest)

env: 
	$(info RUN THIS COMMAND:)
	$(info )
	$(info source ./emsdk/emsdk_env.sh --build=Release)
	$(info )

repl: src/libminilisp.c repl.c

clean:
	rm -f repl
	rm -f build/*

test: repl
	@./test.sh

server:
	emrun --no_browser --port 8000 .

wasm:
	emcc -O3 \
		-D LIB_VERSION=$(VERSION) \
		-I src \
		-s WASM=1 \
		-s EXPORTED_FUNCTIONS='["_malloc", "_free"]' \
		-s ASYNCIFY \
		-s INITIAL_MEMORY=32MB \
		-s 'ASYNCIFY_IMPORTS=["js_handle_lisp"]' \
		-s EXTRA_EXPORTED_RUNTIME_METHODS='["cwrap", "AsciiToString", "writeAsciiToMemory"]' \
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
