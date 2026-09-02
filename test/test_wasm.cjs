#!/usr/bin/env node
/*
 * Smoke test for the WebAssembly build.
 *
 * Drives build/unlisp-local.js the same way index.html does, so the JavaScript
 * entry points are exercised and not just compiled. Run it with `make test-wasm`,
 * which builds the module first, or with `node test/test_wasm.cjs` once build/ exists.
 *
 * The .cjs extension keeps this loading as CommonJS whatever a package.json
 * elsewhere in the tree may say.
 */

const fs = require('fs')
const path = require('path')

// This file lives in test/, and the build products are in build/ at the repo root.
const MODULE_PATH = path.join(__dirname, '..', 'build', 'unlisp-local.js')
if (!fs.existsSync(MODULE_PATH)) {
  console.error(`${MODULE_PATH} not found. Run 'make wasm' first.`)
  process.exit(1)
}

const RED = '\x1b[1;31m'
const RESET = '\x1b[0m'
const HEAP = 8000
const TASK_LIMIT = 200
// The recursion guard is a stack budget in bytes, not a nesting count. A WASM build's
// frames are not an ESP8266's, so `stack_used` here does not predict what a script will
// spend on a device -- it says what this build spent, and the guard is what enforces the
// limit on each target.
const EVAL_STACK = 8 * 1024

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
    const size = await api.lisp_evaluate(HEAP, EVAL_STACK, library_ptr, input_ptr, output_ptr, TASK_LIMIT)
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

  // `print` writes to the log sink, which is a separate stream from the value of each
  // top-level form. Asserting on `out` alone is not enough: print returns what it printed,
  // so a top-level (print x) lands in `out` regardless of whether the log sink is wired at
  // all. These nest it so the two streams must differ.
  r = await evaluate('(+ 1 (print 5))')
  check('print goes to the log sink', r.json && r.json.log && r.json.log[0] === '5',
    `expected log ["5"], got ${r.raw}`)
  check('print does not disturb the value', r.json && r.json.out[0] === '6',
    `expected out ["6"], got ${r.raw}`)

  r = await evaluate('(while (< #itr 3) (print #itr))')
  check('log keeps every write in order',
    r.json && r.json.log && r.json.log.join(',') === '0,1,2',
    `expected log ["0","1","2"], got ${r.raw}`)

  // An error reports the span of the top-level form that failed, not the reader's cursor.
  // The cursor is only right while reading: by the time evaluation fails it has consumed
  // the whole form, and often the forms after it, so it used to point past the problem.
  r = await evaluate('(+ 1 2)\n(car 5)')
  check('an error spans the form that failed',
    r.json && r.json.err && r.json.err.idx === 8 && r.json.err.end === 15,
    `expected idx 8 and end 15 covering "(car 5)", got ${r.raw}`)

  r = await evaluate('(defun f (x) (+ x missing))\n(f 1)')
  check('the span follows the call, not the definition',
    r.json && r.json.err && r.json.err.idx === 28,
    `expected idx 28 at "(f 1)", got ${r.raw}`)

  // A syntax error has no form yet, so the reader's cursor is the right answer there.
  r = await evaluate('(car 5')
  check('a syntax error reports the reader position',
    r.json && r.json.err && r.json.err.idx === r.json.err.end,
    `expected idx and end to match for a read error, got ${r.raw}`)

  // print is a host call in this build, so it lands in the state timeline in the order it
  // happened, beside the primitives -- rather than in a parallel array that has to be
  // correlated afterwards, and rather than being invisible until the run finishes.
  r = await evaluate("(defjs dread (pin))(print 'a)(dread 1)(print 'b)")
  check('print appears in states, in order',
    r.json && r.json.states.map(s => s.ask).join(' | ') === "print a | dread 1 | print b",
    `expected print interleaved with dread, got ${r.raw}`)
  check('print still returns what it printed',
    (await evaluate('(+ 1 (print 5))')).json.out[0] === '6',
    'expected (+ 1 (print 5)) to be 6')

  // A result longer than SYMBOL_MAX_LEN used to overflow a fixed stack buffer.
  // It must now come back truncated and marked, with the module still usable.
  // Comfortably past LISP_PRINT_MAX_LEN rather than just over it, so that the check is
  // about the truncation working at all and not about where the limit happens to sit.
  r = await evaluate(
    '(define l (list 1 2 3 4 5 6 7 8 9 10))' +
    '(define m (list l l l l l l l l l l))' +
    '(define n (list m m m m m m m m m m))' +
    '(print n)')
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

  // The result reports what a script actually cost, so a host can warn before the script
  // is pushed to hardware. Stack is the only figure now: nesting stopped tracking
  // call depth once tail calls were eliminated, so it no longer meant anything a caller
  // could act on.
  r = await evaluate('(+ 1 2)')
  check('meta reports eval cost',
    r.json && r.json.meta && r.json.meta.eval &&
    typeof r.json.meta.eval.stack_used === 'number' &&
    r.json.meta.eval.stack_used > 0 &&
    r.json.meta.eval.stack_limit === EVAL_STACK,
    `expected meta.eval with positive stack_used and stack_limit ${EVAL_STACK}, got ${r.raw}`)

  // Recursion past the budget is refused rather than allowed to run the stack out. The
  // call is wrapped in an addition so that it is not in tail position: a tail call costs
  // no stack and would never reach the guard.
  r = await evaluate('(defun f (n) (if (= n 0) 0 (+ 1 (f (+ n -1))))) (f 200)')
  check('recursion guard',
    r.json && r.json.err && typeof r.json.err.msg === 'string' &&
    r.json.err.msg.includes('Recursion too deep'),
    `expected a recursion error, got ${r.raw}`)

  // And the guard is a budget, not a fixed depth: a shallower run of the same shape
  // stays inside it.
  r = await evaluate('(defun f (n) (if (= n 0) 0 (+ 1 (f (+ n -1))))) (f 3)')
  check('recursion within budget',
    r.json && !r.json.err && r.json.out[r.json.out.length - 1] === '3',
    `expected (f 3) to return 3, got ${r.raw}`)

  // The same depth in tail position costs no stack at all, so it runs where the
  // non-tail version above was refused.
  r = await evaluate('(defun lp (n) (if (= n 0) 0 (lp (+ n -1)))) (lp 200)')
  check('tail calls cost no stack',
    r.json && !r.json.err && r.json.out[r.json.out.length - 1] === '0',
    `expected (lp 200) to succeed, got ${r.raw}`)

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
