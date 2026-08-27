;;; Macros: adding syntax the interpreter does not have.
;;;
;;; A macro receives its arguments unevaluated and returns a form, which is then
;;; evaluated in its place. That is the difference from a function, and it is what
;;; lets a macro decide whether to evaluate something at all -- which no function can
;;; do, because its arguments are already values by the time it runs.
;;;
;;; Wants 7000 bytes of heap and 3072 of eval stack. Expansion is recursive, and
;;; `cond` below expands itself once per clause, so macro-heavy code needs more stack
;;; than the 2048 an ESP8266 is given by default.
;;;
;;; `macroexpand` shows the form a macro produced without running it, and is the way
;;; to find out why one is not doing what you meant.

;;; Conditionals that a function could not be
;;; --------------------------------------------------------------------------------
;; `when` runs its body only if the test holds. As a function this would not work:
;; the body would be evaluated before `when` ever saw it.
(defmacro when (test . body)
  (list 'if test (cons 'progn body) ()))

(defmacro unless (test . body)
  (list 'if test () (cons 'progn body)))

(when #t 1 2 'yes)                      ; yes
(when () 'no)                           ; ()
(unless () 'yes)                        ; yes

;; What `when` actually built. The quote keeps the form from being evaluated so that
;; macroexpand has something to expand.
(macroexpand '(when #t 'a 'b))          ; (if #t (progn (quote a) (quote b)) ())

;;; Local bindings
;;; --------------------------------------------------------------------------------
;; There is no `let`, but a lambda applied on the spot is one: its parameters are the
;; names and its arguments are the values. The macro is that transformation written
;; down. It needs `map` over the bindings, so a small one is defined here.
(defun reverse (l acc)
  (if (eql l ()) acc (reverse (cdr l) (cons (car l) acc))))
(defun map (f l) (map1 f l ()))
(defun map1 (f l acc)
  (if (eql l ()) (reverse acc ()) (map1 f (cdr l) (cons (f (car l)) acc))))

(defun binding-name (b) (car b))
(defun binding-value (b) (car (cdr b)))

(defmacro let (bindings . body)
  (cons (cons 'lambda (cons (map binding-name bindings) body))
        (map binding-value bindings)))

(let ((a 3) (b 4)) (+ a b))             ; 7
(let ((x 10)) (let ((y 5)) (- x y)))    ; 5

(macroexpand '(let ((a 1)) a))          ; ((lambda (a) a) 1)

;;; Choosing between many
;;; --------------------------------------------------------------------------------
;; `cond` is a chain of ifs, and a macro can build a chain of any length -- which is
;; the other thing macros are for. It recurses on itself, expanding one clause at a
;; time until the clauses run out.
(defmacro cond (clause . rest)
  (if (eql rest ())
      (list 'if (car clause) (cons 'progn (cdr clause)) ())
      (list 'if (car clause) (cons 'progn (cdr clause)) (cons 'cond rest))))

(defun classify (n)
  (cond ((< n 0) 'negative)
        ((= n 0) 'zero)
        (#t      'positive)))

(classify -5)                           ; negative
(classify 0)                            ; zero
(classify 7)                            ; positive

;;; A caution
;;; --------------------------------------------------------------------------------
;; A macro's expansion is spliced in where it was written, so a name it introduces can
;; collide with one the caller is already using. `gensym` makes a symbol that nothing
;; else can name, which is how that is avoided.
(gensym)                                ; G__0, and a different one every time
(gensym)                                ; G__1
