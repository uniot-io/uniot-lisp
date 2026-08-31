# uniot-lisp

A small Lisp interpreter in C, written to run on microcontrollers. It is part of the
[Uniot](https://github.com/uniot-io) project and is used as a module by
[uniot-core](https://github.com/uniot-io/uniot-core), but it has no dependency on either
and builds on its own.

Derived from [minilisp](https://github.com/rui314/minilisp) by Rui Ueyama. The evaluator
and the copying collector come from there; tail call elimination, a mark-sweep collector,
the WebAssembly target and most of the primitives below were added here.

It supports integers, symbols, cons cells, lexically scoped variables, closures,
macros, tail call elimination and a choice of garbage collector. There are no strings
and no floating point.

## Building

    make            # build the REPL
    make test       # run the test suite
    make wasm       # build the WebAssembly module (needs the Emscripten SDK)

`make emsdk` installs the SDK; `make env` prints the line to source before `make wasm`.

Products go to `build/`. The interpreter itself is `src/`; everything else is tooling.

## Embedding

The interpreter allocates one heap at startup and never calls `malloc` again. A host
creates it, defines the primitives, and evaluates:

```c
lisp_create(heap_bytes, eval_stack_bytes);
*env = make_env(root, &Nil, &Nil);
define_constants(root, env);
define_primitives(root, env);
add_primitive(root, env, "dread", my_digital_read);   // your own
lisp_eval(root, env, "(dread 4)");
```

Both limits are arguments rather than compile-time constants, because a device and a
browser want different ones. `eval_stack_bytes` bounds evaluation depth in bytes of C
stack; pass 0 to lift it.

The collector is chosen at compile time with `-D MINILISP_GC=MINILISP_GC_COPYING` or
`MINILISP_GC_MARK_SWEEP`, and defaults to mark-sweep. Mark-sweep never moves an object
and needs no memory beyond the heap; copying is faster to allocate into but asks the
system for a second heap during every collection, which a small fragmented device heap
may not be able to give.

## Language

The interpreter reads one expression at a time, evaluates it, and prints the result.

    (+ 1 2)   ; prints 3

### Literals

* **Integers** are 32-bit and signed. Arithmetic that would overflow raises rather than
  wrapping, and a literal too large to represent is refused when it is read.
* **`()`** is the only false value, and also the empty list.
* **`#t`** is the true literal. Any value other than `()` is true, including `0`.
* **Symbols** are interned, so two symbols with the same name are the same object.
  There is no string type; symbols often stand in for one.
* **Lists** are cons cells, either proper (`(a b c)`) or dotted (`(a . b)`).

### Lists

`cons` builds a cell; `car` and `cdr` take it apart; `setcar` and `setcdr` mutate one.
`car` and `cdr` of `()` are `()`.

    (cons 'a 'b)     ; -> (a . b)
    (car '(a b))     ; -> a
    (cdr '(a b))     ; -> (b)
    (length '(1 2))  ; -> 2

`list` returns its arguments as a list, and `apply` calls a function with a list of
arguments worked out at run time.

    (list 1 2 3)             ; -> (1 2 3)
    (apply + (list 1 2 3))   ; -> 6

### Arithmetic

`+ - * / %` take any number of arguments. `/` and `%` are integer operations that
truncate towards zero, so `(/ 7 2)` is `3`. `abs` gives magnitude. Division by zero and
overflow both raise.

    (+ 1 2 3)   ; -> 6
    (- 3)       ; -> -3
    (- 5 2 7)   ; -> -4
    (/ 7 2)     ; -> 3
    (*)         ; -> 1

### Comparison

`< <= > >=` compare numbers and chain, so `(< 1 2 3)` asks whether every adjacent pair
holds.

`eql` is equality: identity for everything, value for numbers. Two lists with the same
contents are not `eql`, because they are different objects.

    (eql 'a 'a)            ; -> #t
    (eql 1 1)              ; -> #t
    (eql (list 1) (list 1)); -> ()

`=` compares numbers, and also compares a number against a boolean by treating any
non-zero number as true. That is what makes it convenient for testing a flag, and also
what makes it not an equality — it is deliberately not transitive. Use `eql` when you
mean equality.

    (= 11 11)   ; -> #t
    (= 1 #t)    ; -> #t
    (= 0 ())    ; -> #t

### Predicates

`numberp`, `symbolp`, `consp`, `listp`, `atom` and `functionp` ask what a value is.
`not` inverts truth. `listp` and `atom` are both true of `()`.

    (numberp 1)     ; -> #t
    (consp '(a))    ; -> #t
    (atom 'a)       ; -> #t
    (not ())        ; -> #t

### Conditionals and sequencing

`(if cond then else)` is the conditional. `(progn expr ...)` evaluates each expression
and returns the last. `and` and `or` stop as soon as the answer is known and return the
value that decided it, so `(and (consp x) (car x))` is safe to write.

    (progn 1 2 3)   ; -> 3
    (and)           ; -> #t
    (or)            ; -> ()

### Loops and recursion

`(while cond expr ...)` is the loop. Inside one, `#itr` is the current iteration,
counting from zero. Loops may nest; `#itr` belongs to the innermost.

    (define c 0)
    (while (< #itr 3) (setq c (+ c 1)))
    c   ; -> 3

**A call in tail position does not consume stack**, so a loop written as tail recursion
runs in constant space. This is the idiomatic way to walk a list:

    (defun sum (l acc) (if (eql l ()) acc (sum (cdr l) (+ acc (car l)))))
    (sum '(1 2 3) 0)   ; -> 6

Recursion that is *not* in tail position does consume stack, and is bounded by the
`eval_stack_bytes` given to `lisp_create`. A loop that never ends is stopped rather than
running forever, whether it is a `while` or a tail call.

### Definitions

`define` binds a variable, `defun` a function, and both may be used again on the same
name to rebind it. `lambda` makes a function value. `setq` assigns to a variable that
already exists.

    (define a (+ 1 2))
    (defun double (x) (+ x x))
    (define double (lambda (x) (+ x x)))   ; the same thing

A parameter list may end in a symbol, which collects the remaining arguments, or be a
symbol, which collects all of them.

    (defun f (a . rest) rest)   (f 1 2 3)   ; -> (2 3)
    (defun g args args)         (g 1 2)     ; -> (1 2)

Calling with the wrong number of arguments raises, in either direction. Parameter names
must be distinct.

Variables are lexically scoped and outlive the call that created them:

    (define counter
      ((lambda (count)
         (lambda () (setq count (+ count 1)) count))
       0))
    (counter)   ; -> 1
    (counter)   ; -> 2

### Macros

`(defmacro name (args ...) body ...)` defines a macro, which receives its arguments
unevaluated and returns a form to be evaluated in its place.

    (defmacro unless (test . body) (list 'if test () (cons 'progn body)))
    (unless (= 1 1) 'no)   ; -> ()
    (unless (= 1 2) 'yes)  ; -> yes

`macroexpand` shows what a macro produced. It is a function, so quote the form:

    (macroexpand '(unless (= 1 1) 'no))   ; -> (if (= 1 1) () (progn (quote no)))

`gensym` returns a symbol nothing else can name, for macros that introduce identifiers.

### Output

`print` writes a value and returns it, so it can be placed inside a larger expression.

    (print '(hello world))   ; writes (hello world)
    (+ 1 (print 2))          ; writes 2, evaluates to 3

### Comments

`;` begins a comment that runs to the end of the line.

## Writing library functions in Lisp

The interpreter deliberately stops at what cannot be written in the language itself.
Everything below is a few lines of Lisp, and belongs in your own code rather than in the
interpreter — a script pays for what it defines and nothing else.

One rule governs all of it: **recurse in tail position.** The accumulator versions below
run in constant stack; the same functions written to recurse on the way back out are
bounded by the evaluation stack and will stop on a long list.

```lisp
(defun reverse (l acc)
  (if (eql l ()) acc (reverse (cdr l) (cons (car l) acc))))

(defun append (a b) (app1 (reverse a ()) b))
(defun app1 (a b) (if (eql a ()) b (app1 (cdr a) (cons (car a) b))))

(defun member (x l)
  (if (eql l ()) () (if (eql x (car l)) l (member x (cdr l)))))

(defun assoc (k l)
  (if (eql l ()) () (if (eql k (car (car l))) (car l) (assoc k (cdr l)))))

(defun nth (n l) (if (= n 0) (car l) (nth (- n 1) (cdr l))))

(defun reduce (f l acc)
  (if (eql l ()) acc (reduce f (cdr l) (f acc (car l)))))

(defun map (f l) (map1 f l ()))
(defun map1 (f l acc)
  (if (eql l ()) (reverse acc ()) (map1 f (cdr l) (cons (f (car l)) acc))))

(defun filter (f l) (filter1 f l ()))
(defun filter1 (f l acc)
  (if (eql l ())
      (reverse acc ())
      (filter1 f (cdr l) (if (f (car l)) (cons (car l) acc) acc))))

(defun min (a b) (if (< a b) a b))
(defun max (a b) (if (> a b) a b))
(defun null (x) (eql x ()))
(defun zerop (x) (= x 0))
```

Control structures are macros:

```lisp
(defmacro when (test . body) (list 'if test (cons 'progn body) ()))

;; cond expands one clause at a time, so it takes any number of them
(defmacro cond (clause . rest)
  (if (eql rest ())
      (list 'if (car clause) (cons 'progn (cdr clause)) ())
      (list 'if (car clause) (cons 'progn (cdr clause)) (cons 'cond rest))))
```

`let` is a lambda applied where it stands, which is also how to introduce a local
variable without a macro:

```lisp
((lambda (x y) (+ x y)) 1 2)   ; -> 3
```

Optional arguments are a rest parameter you look into. Test whether the argument is
*present*, not whether it is true, or a caller who passes `()` on purpose cannot:

```lisp
(defun opt (args n default)
  (if (eql args ()) default
      (if (= n 0) (car args) (opt (cdr args) (- n 1) default))))

(defun draw (x . o) (list x (opt o 0 0) (opt o 1 'black)))
(draw 10)          ; -> (10 0 black)
(draw 10 20 'red)  ; -> (10 20 red)
```

More of these, with commentary, are in [`examples/`](examples/).

## Roadmap

Candidates for a future release. Nothing here is committed to, and anything that can
reasonably be written in Lisp will stay in Lisp.

**Bitwise operations** — `logand`, `logior`, `logxor`, `lognot`, `ash`. These cannot be
expressed in the language at all, which makes them the clearest gap for a dialect meant
to drive hardware: pin masks, register flags, packing readings into a word.

**`equal`** — structural comparison, alongside `eql`. It can be written in Lisp, but the
version in C compares nested structure more deeply before it runs out of stack.

**`random`** — needs a source of entropy the language cannot reach on its own.

Both `equal` and `random` are likely to be offered as primitives a host registers rather
than as part of the core, so that a device paying no attention to them pays nothing for
them.

Not planned: strings, floating point, arbitrary-precision integers, and namespaces.

## License

GPL v3. See [LICENSE](LICENSE). The original minilisp is by Rui Ueyama and is used here
under the same terms.
