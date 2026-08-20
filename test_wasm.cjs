#!/usr/bin/env node
/*
 * Smoke test for the WebAssembly build.
 *
 * Drives build/unlisp-local.js the same way index.html does, so the JavaScript
 * entry points are exercised and not just compiled. Run it with `make test-wasm`,
 * which builds the module first, or with `node test_wasm.cjs` once build/ exists.
 *
 * The .cjs extension keeps this loading as CommonJS whatever a package.json
 * elsewhere in the tree may say.
 */

const fs = require('fs')
const path = require('path')

const MODULE_PATH = path.join(__dirname, 'build', 'unlisp-local.js')
if (!fs.existsSync(MODULE_PATH)) {
  console.error(`${MODULE_PATH} not found. Run 'make wasm' first.`)
  process.exit(1)
}

const RED = '\x1b[1;31m'
const RESET = '\x1b[0m'
const HEAP = 8000
const TASK_LIMIT = 200
// Matches an ESP8266, the tightest real target.
const EVAL_DEPTH = 16

let failures = 0

function check(name, ok, detail) {
  process.stdout.write(`Testing wasm ${name} ... `)
  if (ok) {
    console.log('ok')
  } else {
    console.log('FAILED')
    console.error(`${RED}[ERROR]${RESET} ${detail}`)
    failures++
  }
}

const Module = require(MODULE_PATH)

Module.onRuntimeInitialized = async () => {
  const api = {
    version: Module.cwrap('version', 'number', []),
    lisp_evaluate: Module.cwrap(
      'lisp_evaluate',
      'number',
      ['number', 'number', 'number', 'number', 'number', 'number'],
      { async: true }
    )
  }

  const library_ptr = Module._malloc(1024)
  const input_ptr = Module._malloc(4096)
  const output_ptr = Module._malloc(1048576)
  const handler_out_ptr = Module._malloc(64)

  // Answers every (defjs ...) call with 42, which exercises the tojs round trip.
  Module.lisp_handler = (value, wakeUp) => {
    Module.writeAsciiToMemory('42', handler_out_ptr, false)
    wakeUp(handler_out_ptr)
  }

  async function evaluate(script, library = '(defjs led (i s))') {
    Module.writeAsciiToMemory(script, input_ptr, false)
    Module.writeAsciiToMemory(library, library_ptr, false)
    const size = await api.lisp_evaluate(HEAP, EVAL_DEPTH, library_ptr, input_ptr, output_ptr, TASK_LIMIT)
    const raw = Module.AsciiToString(output_ptr)
    let json = null
    try {
      json = JSON.parse(raw)
    } catch (e) {
      /* leave json null so the caller reports it */
    }
    return { size, raw, json }
  }

  check('version', typeof api.version() === 'number' && api.version() > 0,
    `expected a positive version, got ${api.version()}`)

  let r = await evaluate('(+ 1 2)')
  check('arithmetic', r.json && r.json.out[0] === '3',
    `expected out ["3"], got ${r.raw}`)

  r = await evaluate('(list 1 2 3)')
  check('list result', r.json && r.json.out[0] === '(1 2 3)',
    `expected out ["(1 2 3)"], got ${r.raw}`)

  r = await evaluate('(print (list 1 2 3))')
  check('print', r.json && r.json.out.includes('(1 2 3)'),
    `expected "(1 2 3)" among out, got ${r.raw}`)

  // A result longer than SYMBOL_MAX_LEN used to overflow a fixed stack buffer.
  // It must now come back truncated and marked, with the module still usable.
  r = await evaluate(
    '(define l (list 1 2 3 4 5 6 7 8 9 10))' +
    '(define m (list l l l l l l l l l l))' +
    '(print m)')
  check('long output is truncated, not overflowed',
    r.json && r.json.out.some(o => o.endsWith('...')),
    `expected a truncated entry ending in "...", got ${r.raw}`)

  r = await evaluate('(led 1 2)')
  check('tojs round trip',
    r.json && r.json.states.length === 1 &&
    r.json.states[0].ask === 'led 1 2' && r.json.states[0].answer === '42' &&
    r.json.out[0] === '42',
    `expected one state {ask:"led 1 2", answer:"42"}, got ${r.raw}`)

  // Error messages are formatted into a bounded buffer; a long symbol name used
  // to overflow it.
  const longName = 'some-undefined-symbol-with-a-fairly-long-name'
  r = await evaluate(`(${longName})`)
  check('error path with a long symbol',
    r.json && r.json.err && typeof r.json.err.msg === 'string' &&
    r.json.err.msg.includes(longName) && r.size < 0,
    `expected an error naming the symbol and a negative size, got size=${r.size} ${r.raw}`)

  // The emulator reports what a script actually cost, so the editor can warn before a
  // script is pushed to hardware.
  r = await evaluate('(+ 1 2)')
  check('meta reports eval depth',
    r.json && r.json.meta && r.json.meta.eval &&
    typeof r.json.meta.eval.depth_max === 'number' &&
    r.json.meta.eval.depth_max > 0 &&
    r.json.meta.eval.depth_limit === EVAL_DEPTH,
    `expected meta.eval with a positive depth_max and depth_limit ${EVAL_DEPTH}, got ${r.raw}`)

  // The caller asks for an ESP8266-sized limit, so recursion that would overrun that
  // device is refused here too rather than appearing to work in the browser.
  r = await evaluate('(defun f (n) (if (= n 0) 0 (f (+ n -1)))) (f 30)')
  check('emulated ESP8266 recursion limit',
    r.json && r.json.err && typeof r.json.err.msg === 'string' &&
    r.json.err.msg.includes('Recursion too deep'),
    `expected a recursion error, got ${r.raw}`)

  // The module must still evaluate after the failures above.
  r = await evaluate('(+ 20 22)')
  check('usable after errors', r.json && r.json.out[0] === '42',
    `expected out ["42"], got ${r.raw}`)

  if (failures > 0) {
    console.error(`\n${failures} check(s) failed`)
    process.exit(1)
  }
  console.log('\nAll wasm checks passed')
  process.exit(0)
}
