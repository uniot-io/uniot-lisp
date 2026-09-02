# Changelog

## 0.4.0

Two changes to what a failure tells the host. Small, but one of them adds to the public
API, which is why this is a minor rather than a patch.

### Added

- **`lisp_error_end()`**, the end of the span an error covers. The WebAssembly result
  carries it as `"end"` alongside `"idx"`, so a host can highlight a range rather than
  place a caret.

### Changed

- **`lisp_error_idx()` reports the form that failed, not the reader's position.** It
  returned the reader's cursor, which is the right answer while reading but has run past
  the whole form by the time evaluation fails — and often past the forms after it, so an
  error inside a function body reported an offset within the later call that triggered it.
  It now returns the start of the top-level form being evaluated. Read errors still report
  the cursor, where it was always correct. A host that stored the old value as a caret
  position will want to treat the pair as a range instead.

### Fixed

- **An improper argument list is refused rather than followed.** `eval_list` walked the
  cdr chain assuming every link was a cell, so a dotted tail such as `(+ 1 . 2)` ended the
  walk on an integer and read its union as a pointer — `car` and `value` share an offset —
  evaluating the number as an address. The pointer check in `eval` caught it, but only
  after the fact, and reported "Unexpected statement. Evaluation terminated", which says
  nothing about the problem. It now raises "Argument list is not a proper list". Dotted
  pairs as data are untouched: `'(1 . 2)`, `(car '(1 . 2))` and `(cons 1 2)` all behave as
  before.

## 0.3.0

The first release since 0.2.5, and a large one. Most of what changed was correction
rather than design: several primitives did something other than what their names say,
and arithmetic could be silently wrong. **Read the breaking list before upgrading** —
scripts written against 0.2.x will need changes.

### Breaking

**`eq` is now `eql`.** The old `eq` compared by address, and numbers are boxed and not
interned, so `(eq 1 1)` was false. `eql` is identity for everything and value for
numbers, which is what the name means everywhere else and what callers expect. Every
`eq` in existing code becomes `eql`.

**`and`, `or` and `not` treat `0` as true.** Only `()` is false now, which is what `if`
always did — previously the two disagreed, so `(if x …)` and `(if (not (not x)) …)`
could take different branches.

This one changes behaviour without raising, so it will not announce itself. A value that
came from a pin read, an event payload or any other primitive is a number, and the common
toggle written against the old rule stops toggling:

```lisp
(push_event 'led (not state))   ; state becomes 0, (not 0) is now (), so it sticks
(push_event 'led (= state 0))   ; works, on the first pass and every one after
```

Audit stored scripts for `not`, `and` and `or` applied to anything numeric.

**`and` and `or` stop as soon as the answer is known** and return the operand that
decided it rather than a boolean. `(and (consp x) (car x))` is safe to write now; before,
both operands were evaluated and it raised. They take any number of arguments, and their
last operand is a tail position.

**Arithmetic raises on overflow instead of wrapping.** `(* 2000000000 2)` used to give
`-294967296`. An integer literal too large to represent is refused when it is read.

**`/` is exact.** It computed in a float, whose mantissa cannot hold a 32-bit integer, so
`(/ 100000000 3)` gave `33333334`. It divides in integers and truncates towards zero,
agreeing with `%`.

**Calling with the wrong number of arguments raises in both directions.** Surplus
arguments used to be discarded silently while missing ones raised.

**Duplicate parameter names are refused.** `(defun f (a a) …)` used to bind the second
silently.

**`print` returns the value it printed**, so it can sit inside a larger form. It returned
`()` before, which made `(+ 1 (print x))` add to nothing. It writes only to the log sink
now, not to the value stream as well.

**`define` and `defun` rebind an existing name** instead of raising "Already defined".

**`macroexpand` evaluates its argument.** It is a function, so quote the form:
`(macroexpand '(m 5))`. Taking it unevaluated made the standard spelling return the quote
untouched.

**`car` and `cdr` of `()` return `()`** rather than raising.

**`lisp_create` takes a second argument**, the evaluation stack budget in bytes.
`max_eval_depth` — a count of nesting levels — is gone: nesting was only ever a proxy for
stack, and a poor one, since a level that prints costs several times what a bare call
costs and the figure had to be retuned per chip. `lisp_shoot_once` and `lisp_evaluate`
changed with it.

**`eval_depth` and `eval_depth_max` are gone.** Both were public. Eliminating tail calls
made nesting stop tracking call depth, so the number could no longer be turned into
anything a caller could act on. `stack_used` and `stack_limit` replace them in the WASM
report.

**`MAX_LOOP_ITERATIONS` is now `MINILISP_MAX_LOOP_ITERATIONS`**, and counts a whole loop
nest rather than each loop separately.

**`MEMORY_SIZE` is no longer public.** It is set by `lisp_create`; setting it alone left
the collectors bounds-checking against memory that was never allocated.

**The default collector is mark-sweep**, not copying.

**A heap larger than 65535 bytes is refused** rather than clamped, because one free block
must be able to span the heap and the size field counts to that.

### Added

- **Tail call elimination.** A call in tail position consumes no stack, so a loop written
  as tail recursion runs in constant space — the last form of a body, an `if` branch, the
  last form of a `progn`, the last operand of `and`/`or`, and the call `apply` makes.
- **A mark-sweep collector**, selected with `-D MINILISP_GC=MINILISP_GC_MARK_SWEEP` and
  now the default. Objects never move and a collection needs no memory beyond the heap,
  which removes the failure a small fragmented device heap is prone to. The copying
  collector remains, selectable with `MINILISP_GC_COPYING`.
- **`apply`**, calling a function with an argument list worked out at run time. The call
  it makes is in tail position.
- **`progn`**, sequencing without building a lambda per use.
- **Type predicates**: `numberp`, `symbolp`, `consp`, `listp`, `atom`, `functionp`.
  Nothing could previously ask what it was holding.
- **`bool`**, coercing under the numeric convention: `0` and `()` are false, everything
  else true. The language's own rule is unchanged — only `()` is false — but a device's
  inputs are numbers, and `not` on a pin read or an event payload asks the wrong question.
  Total and idempotent, so it never raises and can be applied without knowing a type.
- **`setcdr`**, the pair `setcar` never had, and **`length`**.
- **Parameter lists may be a bare symbol**, collecting every argument: `(defun f args …)`.
  Previously a function needed at least one required parameter before the rest, so "any
  number of arguments, none required" could not be written.
- **A function may have an empty body**, and answers `()`.
- `<`, `<=`, `>`, `>=` and `=` chain: `(< 1 2 3)` asks whether every adjacent pair holds.
- `(*)` is 1 and `(* x)` is x, as `(+)` and `(+ x)` already were; `(/ x)` is x.
- **A recursion guard in bytes of C stack**, applied to the evaluator, the reader and the
  printer. The reader and printer descend recursively without going through `eval`, so
  nothing bounded them before.
- **A bound on loops and on tail calls**, so a program that never ends is stopped rather
  than hanging the device.
- **The WebAssembly result carries a `log` array**, holding what a script printed. `out`
  is the value of each top-level form, which is not the same thing: a `print` nested
  inside an expression contributes to one and not the other.
- **In the WebAssembly build `print` is a host call**, so it also appears in `states`, in
  order, beside every other outward call. A printed line can be placed against the step it
  happened in, and reaches the host while the script is still running rather than only when
  it finishes. `print` still returns what it printed.
- **The WebAssembly module resolves its own `.wasm`.** It previously loaded relative to the
  page, so a page served from a nested path asked for the module beneath that path. It now
  loads from the site root; set `globalThis.UNLISP_WASM_URL` before loading to serve it from
  anywhere else. Node is unaffected.

### Fixed

- **A circular list no longer hangs the interpreter.** `(setcdr x x)` followed by
  printing walked for ever; since the host prints the value of every top-level form,
  building a cycle was by itself enough.
- **Garbage collection survives an out-of-memory condition** rather than aborting.
- **Buffer overflows and undefined behaviour in the error and print paths.**
- **`(-)`, `(defun)` and `(defmacro)` no longer crash.** Reading past an empty argument
  list gave a null pointer rather than `()`, and the guards fell through.
- **A dot with nothing before it is refused** rather than reading as `()` and writing
  through a shared constant.
- **`gensym` rejects arguments** it previously ignored.
- The overflow check that did exist could not work: it multiplied first and divided back,
  and signed overflow is undefined, so every build above `-O0` removed it.

### Performance

- The object header is four bytes rather than eight on a 32-bit target. An integer is 8
  bytes where it was 12, a cons cell 12 where it was 16.
- The head of a form is looked up once rather than twice.
- Formatting buffers are static rather than on the stack, and the three of them are sized
  apart instead of sharing one constant.
- On an ESP8266, the library's messages live in flash rather than RAM.

### Internals

- The two collectors sit behind a six-function strategy interface and live in their own
  files.
- The value type is `int32_t` rather than `int`, since the range a script sees is part of
  what the language promises now that arithmetic raises.
- The repository has a directory structure: `src/` is the interpreter, `tools/` the REPL
  and benchmarks, `test/` the suites, `wasm/` the WebAssembly target. Everything builds
  into `build/`.
- The test suite runs against both collectors, both word sizes, a sanitizer build, and a
  differential fuzzer that compares them against each other.
