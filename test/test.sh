#!/bin/bash

# Which interpreter to exercise. The suite is the same for every garbage collection
# strategy, so `make test-gc` points this at each build in turn.
REPL="${REPL:-build/repl}"

FAILURES=0

function fail() {
  echo -n -e '\e[1;31m[ERROR]\e[0m '
  echo "$1"
  FAILURES=$((FAILURES + 1))
}

function do_run() {
  error=$(echo "$3" | "$REPL" 2>&1 > /dev/null)
  if [ -n "$error" ]; then
    echo FAILED
    fail "$error"
  fi

  result=$(echo "$3" | "$REPL" 2> /dev/null | sed -r "s/\x1B\[([0-9]{1,2}(;[0-9]{1,2})?)?[mGK]//g" | tail -1)
  if [ "$result" != "$2" ]; then
    echo FAILED
    fail "$2 expected, but got $result"
  fi
}

function run() {
  echo -n "Testing $1 ... "
  local before=$FAILURES
  # Run the tests twice to test the garbage collector with different settings.
  MINILISP_ALWAYS_GC= do_run "$@"
  MINILISP_ALWAYS_GC=1 do_run "$@"
  if [ "$FAILURES" -eq "$before" ]; then
    echo ok
  fi
}

# The counter tests below run millions of iterations, which is the only way to reach a
# limit measured in the hundreds of thousands. That is quick on its own and ruinous with
# a collection and a full heap verification on every allocation -- one of them takes 27
# seconds in the slowest configuration, and the matrix runs the suite five times. They
# test the counters, not the collector, so they are run once, plainly, and skipped where
# the collector is the thing under test.
# Tests that pin the eval stack to a byte figure measure how much C stack a level of
# evaluation costs. A sanitizer build changes that -- redzones make every frame larger --
# so those tests cannot mean anything under it and are skipped rather than retuned to a
# number that describes nothing. Everything else still runs, which is the point.
STACK_BUDGET_OK=1
if [ -n "$MINILISP_SANITIZED" ]; then
  STACK_BUDGET_OK=
fi

# Runs a test that pins the eval stack budget. $1 is the budget; the rest is a `run`.
function run_stack() {
  if [ -z "$STACK_BUDGET_OK" ]; then return 0; fi
  local budget=$1; shift
  MINILISP_MAX_EVAL_STACK=$budget run "$@"
}

function run_error_stack() {
  if [ -z "$STACK_BUDGET_OK" ]; then return 0; fi
  run_error "$@"
}

SLOW_OK=1
if [ -n "$MINILISP_VERIFY_GC" ] || [ -n "$MINILISP_ALWAYS_GC" ]; then
  SLOW_OK=
fi

# Tests that need more heap than a device has. The interpreters file is a stress test,
# not a device script: it wants 14000 bytes at device object sizes, so the run that
# pins the heap to what an ESP8266 ships with would fail it for the right reason and
# the wrong purpose.
function run_big() {
  local need=$1; shift
  local have=${MINILISP_HEAP_SIZE:-40000}
  if [ "$have" -lt "$need" ]; then return 0; fi
  run_slow "$@"
}

function run_slow() {
  if [ -z "$SLOW_OK" ]; then return 0; fi
  MINILISP_ALWAYS_GC= run "$@"
}

function run_error_slow() {
  if [ -z "$SLOW_OK" ]; then return 0; fi
  run_error "$@"
}

# Runs code whose earlier forms raise on purpose, and checks the last value printed.
# `run` treats any output on stderr as a failure, which is what makes it useless for
# checking what survives an error.
#   $1 name, $2 expected last line, $3 code
function run_after_error() {
  echo -n "Testing $1 ... "
  local before=$FAILURES
  local result
  result=$(echo "$3" | "$REPL" 2>/dev/null | sed -r "s/\x1B\[([0-9]{1,2}(;[0-9]{1,2})?)?[mGK]//g" | tail -1)
  if [ "$result" != "$2" ]; then
    echo FAILED
    fail "$2 expected, but got $result"
  fi
  if [ "$FAILURES" -eq "$before" ]; then
    echo ok
  fi
}

# Runs code that is expected to raise, and checks the message.
#   $1 name, $2 expected substring, $3 code, $4 optional eval stack budget in bytes
function run_error() {
  echo -n "Testing $1 ... "
  local before=$FAILURES
  local out
  out=$(echo "$3" | MINILISP_MAX_EVAL_STACK="${4:-}" "$REPL" 2>&1 >/dev/null | sed -r "s/\x1B\[([0-9]{1,2}(;[0-9]{1,2})?)?[mGK]//g")
  if [[ "$out" != *"$2"* ]]; then
    echo FAILED
    fail "expected an error containing \"$2\", got \"$out\""
  fi
  if [ "$FAILURES" -eq "$before" ]; then
    echo ok
  fi
}

# Basic data types
run integer 1 1
run integer -1 -1
run symbol a "'a"
run quote a "(quote a)"
run quote 63 "'63"
run quote '(+ 1 2)' "'(+ 1 2)"

run + 3 '(+ 1 2)'
run + -2 '(+ 1 -3)'

run 'unary -' -3 '(- 3)'
run '-' -2 '(- 3 5)'
run '-' -9 '(- 3 5 7)'

run '<' \#t '(< 2 3)'
run '<' '()' '(< 3 3)'
run '<' '()' '(< 4 3)'

run '<=' \#t '(<= 2 3)'
run '<=' \#t '(<= 3 3)'
run '<=' '()' '(<= 4 3)'

run '>' \#t '(> 3 2)'
run '>' '()' '(> 3 3)'
run '>' '()' '(> 3 4)'

run '>=' \#t '(>= 3 2)'
run '>=' \#t '(>= 3 3)'
run '>=' '()' '(>= 3 5)'

run 'literal list' '(a b c)' "'(a b c)"
run 'literal list' '(a b . c)' "'(a b . c)"

# List manipulation
run cons "(a . b)" "(cons 'a 'b)"
run cons "(a b c)" "(cons 'a (cons 'b (cons 'c ())))"

run car a "(car '(a b c))"
run cdr "(b c)" "(cdr '(a b c))"

run setcar "(x . b)" "(define obj (cons 'a 'b)) (setcar obj 'x) obj"

# Comments
run comment 5 "
  ; 2
  5 ; 3"

# Global variables
run define 7 '(define x 7) x'
run define 10 '(define x 7) (+ x 3)'
run setq 11 '(define x 7) (setq x 11) x'
run setq 17 '(setq + 17) +'

# Conditionals
run if a "(if 1 'a)"
run if '()' "(if () 'a)"
run if a "(if 1 'a 'b)"
run if a "(if 0 'a 'b)"
run if a "(if 'x 'a 'b)"
run if b "(if () 'a 'b)"
run if c "(if () 'a 'b 'c)"

# Logical operations
# Only () is false, which is the rule `if` uses. 0 is a value like any other.
run not \#t '(not ())'
run not '()' '(not #t)'
run not '()' '(not 3)'
run not '()' '(not -5)'
run not '()' '(not 0)'
run not '()' "(not 'a)"

# `and` and `or` return the operand that decided the answer, not a boolean, and they stop
# as soon as it is decided. (and) is true and (or) is false, as they are everywhere else.
run and \#t '(and #t #t)'
run and 1 '(and #t 1)'
run and 4 '(and #t 1 4)'
run and 0 '(and #t 0)'
run and '()' '(and #t ())'
run and '()' '(and () #t)'
run and '()' '(and () ())'
run and '()' '(and #t 1 5 ())'
run and \#t '(and)'
run and 1 '(and 1)'

run or \#t '(or #t #t)'
run or \#t '(or #t 1)'
run or 1 '(or () 1)'
run or 0 '(or 0 0)'
run or \#t '(or #t ())'
run or \#t '(or () #t)'
run or '()' '(or () ())'
run or '()' '(or)'
run or 1 '(or 1)'

# Stopping early is what makes a guard safe to write. Each of these raises if the second
# operand is evaluated.
run and-short-circuits '()' '(and () (/ 1 0))'
run or-short-circuits \#t '(or #t (/ 1 0))'
run and-guards-a-list '()' '(and (consp ()) (car ()))'

# The last operand is a tail position, like the last form of a body.
run_stack 4096 and-tail-position 0 \
  '(defun lp (n) (and #t (if (eql n 0) 0 (lp (+ n -1))))) (lp 1000)'
run_stack 4096 or-tail-position 0 \
  '(defun lp (n) (or () (if (eql n 0) 0 (lp (+ n -1))))) (lp 1000)'

# Numeric comparisons
run = \#t '(= 3 3)'
run = '()' '(= 3 2)'

# `=` coerces a number to a boolean before comparing, which is what makes it useful for
# testing a flag and what makes it not an equality: it is deliberately not transitive,
# since (= 1 #t) and (= 2 #t) hold while (= 1 2) does not. `eql` is the equality. This
# matrix is the whole rule -- a number is true when it is not zero -- and it was only
# written down in a scratch file until now.
run =-nonzero-is-true \#t '(= 1 #t)'
run =-two-is-true \#t '(= 2 #t)'
run =-large-is-true \#t '(= 2000 #t)'
run =-negative-is-true \#t '(= -10 #t)'
run =-zero-is-not-true '()' '(= 0 #t)'
run =-zero-is-false \#t '(= 0 ())'
run =-one-is-not-false '()' '(= 1 ())'
run =-large-is-not-false '()' '(= 200 ())'
run =-false-equals-false \#t '(= () ())'
run =-true-equals-true \#t '(= #t #t)'
run =-false-is-not-true '()' '(= () #t)'
run_error "= rejects a list" "takes only numbers and booleans" '(= (list 3) (list 3))'

# eq
run eq \#t "(eql 'foo 'foo)"
run eq \#t "(eql + +)"
run eq '()' "(eql 'foo 'bar)"
run eq '()' "(eql + 'bar)"

# Other arithmetic operations
run abs 3 '(abs -3)'
run abs 3 '(abs 3)'
run abs 0 '(abs 0)'
run mul 0 '(* 0 2)'
run mul 0 '(* 3 0)'
run mul 0 '(* 3 9 0)'
run mul 1 '(* 1 1)'
run mul 5 '(* 1 5)'
run mul 16 '(* 2 8)'
run mul 60 '(* 2 -3 5 -2)'
run mul -1 '(* 1 -1)'
run mul -48 '(* 4 2 -6)'
run div 1 '(/ 5 5)'
run div 3 '(/ 6 2)'
run div -3 '(/ 6 -2)'

# gensym
run gensym G__0 '(gensym)'
run gensym '()' "(eql (gensym) 'G__0)"
run gensym '()' '(eql (gensym) (gensym))'
run gensym \#t '((lambda (x) (eql x x)) (gensym))'

# Functions
run lambda '<function>' '(lambda (x) x)'
run lambda \#t '((lambda () #t))'
run lambda 9 '((lambda (x) (+ x x x)) 3)'
run defun 12 '(defun double (x) (+ x x)) (double 6)'

run args 15 '(defun f (x y z) (+ x y z)) (f 3 5 7)'

run restargs '(3 5 7)' '(defun f (x . y) (cons x y)) (f 3 5 7)'
run restargs '(3)'    '(defun f (x . y) (cons x y)) (f 3)'

# Lexical closures
run closure 3 '(defun call (f) ((lambda (var) (f)) 5))
  ((lambda (var) (call (lambda () var))) 3)'

run counter 3 '
  (define counter ((lambda (val) (lambda () (setq val (+ val 1)) val)) 0))
  (counter)
  (counter)
  (counter)'

# While loop
run while 45 "
  (define i 0)
  (define sum 0)
  (while (< i 10) (setq sum (+ sum i)) (setq i (+ i 1)))
  sum"

# Macros
run macro 42 "
  (defun lst (x . y) (cons x y))
  (defmacro if-zero (x then) (lst 'if (lst '= x 0) then))
  (if-zero 0 42)"

run macro 7 '(defmacro seven () 7) ((lambda () (seven)))'

# macroexpand is a function, so it evaluates its argument: the form to expand is quoted.
# It used to take the form unevaluated, which made the standard spelling return the quote
# untouched instead of expanding anything.
run macroexpand '(if (= x 0) (print x))' "
  (defun lst (x . y) (cons x y))
  (defmacro if-zero (x then) (lst 'if (lst '= x 0) then))
  (macroexpand '(if-zero x (print x)))"


# Sum from 0 to 10
run recursion 55 '(defun f (x) (if (= x 0) 0 (+ (f (+ x -1)) x))) (f 10)'

# The collector must actually reclaim. Nothing else in this file would notice a
# collector that faithfully preserves every object and frees nothing: the answers stay
# correct and the heap is never pushed hard enough to exhaust. Measured headroom after
# collection is ~120 bytes; a collector that reclaimed nothing would sit ~9 KB above.
run gc-reclaims '#t' "
  (define i 0)
  (define junk ())
  (define base (mem))
  (while (< i 30) (setq junk (list 1 2 3 4 5)) (setq i (+ i 1)))
  (setq junk ())
  (gc)
  (< (mem) (+ base 1000))"

# Live data must come through repeated collections intact, which catches both premature
# collection and a corrupted copy.
run gc-survives '(1 2 3 4 5)' "
  (define keep (list 1 2 3 4 5))
  (gc)
  (gc)
  (gc)
  keep"

# A list whose elements are themselves lists. A collector that walks the heap depth-first
# has to hold each element aside while it follows the spine, so this is the shape that
# makes its own bookkeeping grow -- a flat list, or a chain of single-element lists, is
# walked without remembering anything at all. Sized past the mark-sweep work stack, which
# needs a larger heap than the rest of the suite runs in, so that the path that recovers
# from a full work stack is exercised at the default stack size.
MINILISP_HEAP_SIZE=20000 run gc-branching 149 "
  (define i 0)
  (define n ())
  (while (< i 150) (setq n (cons (list i) n)) (setq i (+ i 1)))
  (gc)
  (car (car n))"

# Churn of mixed shapes -- closures, their environments, lists -- rather than the single
# shape the other tests allocate, followed by real work that has to come out right.
run gc-churn 55 "
  (define i 0)
  (define acc ())
  (while (< i 40) (setq acc ((lambda (x) (list x (+ x 1) (list x))) i)) (setq i (+ i 1)))
  (defun f (x) (if (= x 0) 0 (+ (f (+ x -1)) x)))
  (f 10)"

# `eql` is identity for everything except numbers, which are compared by value. Numbers
# are boxed and not interned, so every literal is a distinct object and comparing
# addresses would make this false -- which is what `eq` used to do, and what silently
# broke generated comparisons against a sensor reading.
run eql-on-numbers '#t' '(eql 1 1)'
run eql-distinguishes-numbers '()' '(eql 1 2)'
run eql-on-symbols '#t' "(eql 'a 'a)"
run eql-is-identity-for-lists '()' '(eql (list 1) (list 1))'

# Sequencing without allocating. The alternative was (list a b), which conses a cell per
# statement and throws the result away.
run progn 3 '(progn 1 2 3)'
run progn-empty '()' '(progn)'
run progn-in-a-branch 42 '(if #t (progn 1 42) 0)'

# The last form of a progn is a tail position, like the last form of a body. The budget
# here is a fraction of what a thousand frames would need.
run_stack 4096 progn-tail-position 0 \
  '(defun lp (n) (progn 1 (if (eql n 0) 0 (lp (+ n -1))))) (lp 1000)'

# Type predicates. Without these nothing could ask what it was holding, and `=` raises on
# anything that is not a number or a boolean.
run numberp '#t' '(numberp 1)'
run numberp-negative '()' "(numberp 'a)"

# bool coerces under the numeric convention: 0 and () false, everything else true. It is
# total, so it never raises, and wrapping an already-boolean value is a no-op.
run bool-zero '()' '(bool 0)'
run bool-nonzero '#t' '(bool 1)'
run bool-negative '#t' '(bool -3)'
run bool-nil '()' '(bool ())'
run bool-true '#t' '(bool #t)'
run bool-symbol '#t' "(bool 'a)"
run bool-list '#t' "(bool '(1 2))"
run bool-idempotent '()' '(bool (bool 0))'
run bool-not-agrees '#t' '(not (bool 0))'
run bool-alias '()' '(define ? bool) (? 0)'
run_error "bool arity" "Malformed bool" '(bool 1 2)'

# A dotted tail ends the argument walk on a non-cell. Reading its car would take the
# union as a pointer, so the number would be evaluated as an address.
run_error "improper argument list" "Argument list is not a proper list" '(+ 1 . 2)'
run_error "improper argument list, large" "Argument list is not a proper list" '(+ 1 . 70000)'
run "dotted pair as data" '(1 . 2)' "'(1 . 2)"
run "car of a dotted pair" '1' "(car '(1 . 2))"
run "cons builds a dotted pair" '(1 . 2)' '(cons 1 2)'

run symbolp '#t' "(symbolp 'a)"
run consp '#t' '(consp (list 1))'
run consp-of-empty '()' '(consp ())'
run listp-of-empty '#t' '(listp ())'
run atom-of-empty '#t' '(atom ())'
run atom-of-cons '()' '(atom (list 1))'
run functionp '#t' '(functionp +)'

run length 3 '(length (list 1 2 3))'
run length-of-empty 0 '(length ())'
run setcdr '(1 . 9)' '(define p (cons 1 2)) (setcdr p 9)'

# A call in tail position must not grow the stack. The budget here is a fraction of what
# a thousand nested frames would need, so this fails the moment a tail position stops
# being one -- which is exactly what happened when `if` still evaluated its own branch.
run_stack 4096 tail-calls 0 \
  '(defun lp (n) (if (= n 0) 0 (lp (+ n -1)))) (lp 1000)'

# The last form of a multi-form else is a tail position too.
run_stack 4096 tail-calls-in-else 0 \
  '(defun lp (n) (if (= n 0) 0 (print n) (lp (+ n -1)))) (lp 300)'

# The last form of a function body, with earlier forms evaluated for effect.
run_stack 4096 tail-calls-in-body 0 \
  '(defun lp (n) (setq n (+ n -1)) (if (= n 0) 0 (lp n))) (lp 500)'

# Recursion that is not in tail position still has to be refused: the frames are real.
run_error_stack "non-tail recursion still bounded" "Recursion too deep" \
  '(defun d (n) (if (= n 0) 0 (+ 1 (d (+ n -1))))) (d 1000)' 4096

# A tail call that never returns is a loop, and is bounded like one. Before tail calls
# were eliminated this ran out of stack instead, which is not something to rely on.
run_error_slow "runaway tail call" "without returning" '(defun r (n) (r (+ n 1))) (r 0)'

# Nested loops, which were refused outright while a single global counter enforced the
# limit. #itr is the innermost loop's iteration and is put back on the way out.
run nested-while 24 \
  '(define n 0) (while (< #itr 2) (while (< #itr 3) (while (< #itr 4) (setq n (+ n 1))))) n'

# A loop that never ends, whatever it is nested in. The iteration count is summed over
# the whole nest, so an inner loop cannot hide behind an outer one's limit: neither of
# these two reaches 200 alone, and together they are 40000.
run_error_slow "runaway while" "without ending" '(while #t 1)'
run_error_slow "runaway nested while" "without ending" '(while #t (while (< #itr 10) 1))'
run_error_slow "nested loops multiply" "without ending" '(while (< #itr 200) (while (< #itr 200) 1))'

# Loops that merely follow one another do not multiply, and must not be charged for one
# another. Each of these is just under the limit; summing them would refuse the pair.
run_slow sequential-loops "()" '(progn (while (< #itr 19999) 1) (while (< #itr 19999) 1))'

# #itr belongs to the loop running now, and there is none once it ends. An error thrown
# out of a loop skips every bit of cleanup prim_while does, so the state is cleared where
# evaluation begins instead -- this is the case that catches it if that stops happening.
run itr-cleared-after-loop 0 '(while (< #itr 5) 1) #itr'
run_after_error itr-cleared-after-error 0 '(while (< #itr 5) (car 1))
#itr'

# The counters bound repetition, not work. Naive fib makes hundreds of thousands of
# calls, but no one chain of them repeats without end, so nothing here is bounded by
# either counter. A count of everything eval does -- which is what this replaced --
# refused programs of exactly this shape, eight queens among them.
run_slow heavy-recursion 10946 \
  '(defun fib (n) (if (< n 2) n (+ (fib (+ n -1)) (fib (+ n -2))))) (fib 21)'

# A tail-recursive loop is a loop, and is held to the same count as `while`. Under it
# is fine; the runaway test above is the same shape with no end.
run_slow long-tail-loop 40504500 \
  '(defun sum (n acc) (if (= n 0) acc (sum (+ n -1) (+ acc n)))) (sum 9000 0)'

# A parameter list may be a bare symbol, which takes every argument. Without it a
# function needs at least one required parameter before the rest, so "any number of
# arguments, none required" could not be written at all.
run params-symbol 3 '(defun f args (length args)) (f 1 2 3)'
run params-symbol-empty "()" '(defun f args args) (f)'
run params-symbol-lambda "(1 2)" '((lambda args args) 1 2)'
run params-dotted "(2 3)" '(defun f (a . rest) rest) (f 1 2 3)'
run params-dotted-empty "()" '(defun f (a . rest) rest) (f 1)'

# Calling with the wrong number of arguments is noticed either way round. Too many used
# to be discarded in silence while too few raised, which meant the same mistake was
# caught or not depending on which direction it went in.
run_error "too few arguments" "too few arguments" '(defun f (a b) a) (f 1)'
run_error "too many arguments" "too many arguments" '(defun f (a) a) (f 1 2)'
run_error "too many arguments, none expected" "too many arguments" '(defun f () 9) (f 1)'
run_error "too many arguments to a macro" "too many arguments" '(defmacro m (a) a) (m 1 2)'

# A dot with nothing before it has no cell to give a tail to. It used to write through
# Nil -- a shared constant, not a cell -- and read as ().
run_error "stray dot in a list" "Stray dot" "(quote (. rest))"
run params-dotted-pair "(a . b)" "(quote (a . b))"

# Parameter names must be distinct. The second binding used to hide the first in
# silence, which is predictable only if you know a later parameter is consed on later
# and so is found first.
run_error "duplicate parameter" "Duplicate parameter: a" '(defun f (a a) a)'
run_error "duplicate parameter, apart" "Duplicate parameter: a" '(defun f (a b a) a)'
run_error "duplicate rest parameter" "Duplicate parameter: a" '(defun f (a . a) a)'
run_error "duplicate parameter in a macro" "Duplicate parameter: x" '(defmacro m (x x) x)'
run distinct-parameters 3 '(defun f (a b) (+ a b)) (f 1 2)'

# One name for one condition. A name that is not bound reported "Undefined symbol: x"
# from eval and "Unbound variable x" from setq -- different words and different
# punctuation for the same thing.
run_error "undefined symbol" "Undefined symbol: nope" 'nope'
run_error "undefined symbol from setq" "Undefined symbol: nope" '(setq nope 1)'
run_error "undefined symbol as a head" "Undefined symbol: nope" '(nope 1)'

# Arithmetic that refuses to be quietly wrong. The overflow check that was here could
# not work -- it multiplied first and divided back, and signed overflow is undefined, so
# every build above -O0 deleted the test.
run_error "add overflows" "Integer overflow in +" '(+ 2000000000 2000000000)'
run_error "subtract overflows" "Integer overflow in -" '(- -2000000000 2000000000)'
run_error "multiply overflows" "Integer overflow in *" '(* 2000000000 2)'
# INT_MIN is reachable by arithmetic but cannot be written as a literal, and it is the
# one value whose negation, and whose division by -1, has no positive counterpart.
run int-min -2147483648 '(- (- 0 2147483647) 1)'
run_error "negate overflows" "Integer overflow in -" '(- (- (- 0 2147483647) 1))'
run_error "divide overflows" "Integer overflow in /" '(/ (- (- 0 2147483647) 1) -1)'
run modulo-of-int-min 0 '(% (- (- 0 2147483647) 1) -1)'
run_error "literal too large" "Integer literal is too large" '2147483648'

# Division in integers. In a float, whose mantissa is 24 bits, these were all wrong.
run divide-exactly 33333333 '(/ 100000000 3)'
run divide-exactly-2 123456789 '(/ 123456789 1 1)'
run divide-truncates-toward-zero -3 '(/ -7 2)'
run divide-agrees-with-modulo -1 '(% -7 2)'

# A cycle can be built with setcdr, and a walk of one has no end. The printer runs the
# value of every top-level form, so this is what a device hangs on.
run_error "length of a circular list" "length takes a list" \
  '(define x (list 1 2)) (setcdr x x) (length x)'
run_after_error circular-print-terminates 0 \
  '(define x (list 1 2)) (setcdr x x) x
0'

# gensym takes nothing, and said nothing when given something.
run_error "gensym arity" "Malformed gensym" '(gensym 1)'

# apply: calling a function with a list of arguments worked out at run time. The
# arguments are already values, so a primitive -- which evaluates what it is handed --
# has to be given them quoted, and the order has to survive that.
run apply-primitive 6 '(apply + (list 1 2 3))'
run apply-order 77 '(apply - (list 100 20 3))'
run apply-unevaluated 1 '(apply car (list (list 1 2)))'
run apply-symbols "(on off)" "(apply list (list 'on 'off))"
run apply-function 42 '(defun f (a b) (* a b)) (apply f (list 6 7))'
run apply-closure 15 '(define g ((lambda (n) (lambda (x) (+ n x))) 10)) (apply g (list 5))'
run apply-no-args 42 '(defun z () 42) (apply z ())'
run apply-rest-params "(1 (2 3))" '(defun v (a . r) (list a r)) (apply v (list 1 2 3))'
# The reason it exists: a rest parameter can collect arguments but nothing else can
# pass them on.
run apply-forwards "(got 1 2)" \
  "(defun raw (a b) (list 'got a b)) (defun traced args (apply raw args)) (traced 1 2)"

# The call apply makes is in tail position, like a call written out. Without that a
# loop written through it grows the stack where the same loop written directly does
# not, which is a difference nobody would predict from the outside.
run_stack 2048 apply-is-a-tail-call done \
  "(defun loop (n) (if (= n 0) 'done (apply loop (list (- n 1))))) (loop 500)"
run_stack 2048 apply-mutual-tail done \
  "(defun a (n) (if (= n 0) 'done (apply b (list (- n 1))))) (defun b (n) (apply a (list n))) (a 300)"
run_error "runaway apply" "without returning" '(defun r (n) (apply r (list (+ n 1)))) (r 0)'
# An alias is the same primitive and keeps the treatment; a rebinding does not.
run apply-alias 6 '(define ap apply) (ap + (list 1 2 3))'
run apply-rebound mine "(defun apply (f a) 'mine) (apply + (list 1))"
# Reached as an ordinary primitive rather than through eval's shortcut.
run apply-applied 6 '(apply apply (list + (list 1 2 3)))'

run_error "apply arity" "Malformed apply" '(apply +)'
run_error "apply needs a list" "expects a list of arguments" '(apply + 5)'
run_error "apply of a non-function" "head of a list must be a function" '(apply 5 (list 1))'
run_error "apply of a macro" "head of a list must be a function" '(defmacro m (a) a) (apply m (list 1))'
run_error "apply checks arity" "too many arguments" '(defun f (a) a) (apply f (list 1 2))'

# A macro object in head position, which a form built at run time can produce. It used
# to report an internal bug rather than the ordinary complaint.
run_error "macro object as a head" "head of a list must be a function" \
  '(defmacro m (a) a) (eval (cons m (list 1)))'
run macro-is-a-value "#t" '(defmacro m (a) a) (eql m m)'

# A function may have no body and answers () when called, as it does in the Lisp this
# imitates. Three separate places evaluate a body -- eval's own call path, apply_tail,
# and apply_func by way of progn -- and each had a guard for the empty case that could
# not be reached until now, progn's being a null pointer rather than ().
run empty-body-lambda "()" "((lambda ()))"
run empty-body-defun "()" "(defun f ()) (f)"
run empty-body-apply "()" "(apply (lambda ()) ())"
run empty-body-macro "()" "(defmacro m ()) (m)"
run empty-body-with-params "()" "(defun f (a b)) (f 1 2)"
run empty-body-rest-params "()" "(defun f args) (f 1 2)"
run_error "improper body tail" "Malformed lambda" "(lambda (x) . 5)"

# A parameter that is not a symbol, in either position.
run_error "non-symbol parameter" "must be a symbol" '(lambda (a 5) 1)'
run_error "non-symbol rest parameter" "must be a symbol" '(lambda (a . 5) 1)'
run_error "non-list parameters" "Malformed lambda" '(lambda 5 1)'

# examples/interpreters.lisp: the Z combinator, Church numerals, and this Lisp
# interpreting itself. Deliberately atypical -- closures capturing closures, recursion
# with no name to recurse through, environments built at run time, symbol identity
# leaned on by an inner evaluator. It allocates heavily, which makes it a collector test
# as much as an evaluator one.
run_big 14000 interpreters "(720 144 3 3 5 49 120 15)" "$(cat examples/interpreters.lisp)"

# Redefinition rebinds rather than raising, as it does everywhere else in this family.
run redefine-value 2 '(define a 1) (define a 2) a'
run redefine-function 2 '(defun f (x) 1) (defun f (x) 2) (f 0)'

# Recursion guard: deep recursion must raise rather than overrun the C stack.
# A budget far below what fifty nested calls need, so the guard raises rather than the
# stack running out. The recursion is deliberately not in tail position -- wrapped in an
# addition -- because a tail call costs no stack at all and would leave this passing for
# the wrong reason.
run_error_stack "eval stack guard" "Recursion too deep" \
  '(defun f (n) (if (= n 0) 0 (+ 1 (f (+ n -1))))) (f 50)' 4096

if [ "$FAILURES" -ne 0 ]; then
  echo -e "\n$FAILURES check(s) failed"
  exit 1
fi
echo -e "\nAll checks passed"
