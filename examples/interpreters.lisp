;;; Programs written to strain the engine rather than to be useful.
;;;
;;; Everything here is deliberately atypical: recursion with no name to recurse
;;; through, integers built out of nothing but lambdas, and this Lisp interpreting
;;; itself. None of it is how anyone would write a device rule. That is the point --
;;; between them these exercise closures capturing closures, environments built at run
;;; time, symbol identity, deep allocation, and self-application, in combinations an
;;; ordinary script never reaches.
;;;
;;; The file ends in one list of results, so a test can assert the whole thing at once:
;;;
;;;   (720 144 3 3 5 49 120 15)
;;;
;;; Run it under MINILISP_ALWAYS_GC=1 MINILISP_VERIFY_GC=1 and it becomes a collector
;;; test as well -- every one of those allocations moves under the copying collector.
;;;
;;; Wants 14000 bytes of heap at device object sizes, so it does not fit the 7000 an
;;; ESP8266 ships with. That is not a defect in it: nothing here is a device rule, and
;;; the point is to allocate hard enough to move the collector.

;;; Recursion with no name
;;; --------------------------------------------------------------------------------
;; The Z combinator: the applicative-order Y. A function is handed to itself, and the
;; recursive call is a closure over a closure over its own argument. Nothing here is
;; bound to a name the recursion can reach.
(defun Z (f)
  ((lambda (x) (f (lambda (v) ((x x) v))))
   (lambda (x) (f (lambda (v) ((x x) v))))))

(define fact (Z (lambda (self) (lambda (n) (if (= n 0) 1 (* n (self (- n 1))))))))
(define fib  (Z (lambda (self) (lambda (n) (if (< n 2) n (+ (self (- n 1)) (self (- n 2))))))))

;;; Integers made of lambdas
;;; --------------------------------------------------------------------------------
;; A Church numeral is a function that applies another function n times. There is no
;; number anywhere until `to-int` puts one there, so this tests the evaluator's handling
;; of closures and nothing else.
(define zero (lambda (f) (lambda (x) x)))
(defun succ (n) (lambda (f) (lambda (x) (f ((n f) x)))))
(defun plus (m n) (lambda (f) (lambda (x) ((m f) ((n f) x)))))
(defun to-int (n) ((n (lambda (x) (+ x 1))) 0))

;;; This Lisp, interpreting itself
;;; --------------------------------------------------------------------------------
;; A small evaluator over the same data the host reads. It builds its own environments
;; as association lists, represents closures as tagged lists, and dispatches on symbols
;; by identity -- so it leans on `eql` comparing interned symbols by pointer, which is
;; exactly the property that breaks if interning ever goes wrong.
(defun cadr (l) (car (cdr l)))
(defun caddr (l) (car (cdr (cdr l))))
(defun cadddr (l) (car (cdr (cdr (cdr l)))))

(defun m-lookup (k env)
  (if (eql env ())
      'unbound
      (if (eql k (car (car env))) (cdr (car env)) (m-lookup k (cdr env)))))

(defun m-bind (names vals env)
  (if (eql names ())
      env
      (m-bind (cdr names) (cdr vals) (cons (cons (car names) (car vals)) env))))

(defun m-eval-list (l env)
  (if (eql l ()) () (cons (m-eval (car l) env) (m-eval-list (cdr l) env))))

(defun m-prim (op a b)
  (if (eql op '+) (+ a b)
    (if (eql op '-) (- a b)
      (if (eql op '*) (* a b)
        (if (eql op '<) (< a b) 'no-such-primitive)))))

;; A closure is (closure <lambda-form> <env>), so a cons means a closure and anything
;; else is one of the primitives above.
(defun m-apply (f args)
  (if (consp f)
      (m-eval (caddr (cadr f)) (m-bind (cadr (cadr f)) args (caddr f)))
      (m-prim f (car args) (cadr args))))

(defun m-eval (e env)
  (if (numberp e) e
    (if (symbolp e) (m-lookup e env)
      (if (eql (car e) 'quote) (cadr e)
        (if (eql (car e) 'if)
            (if (m-eval (cadr e) env) (m-eval (caddr e) env) (m-eval (cadddr e) env))
          (if (eql (car e) 'lambda)
              (list 'closure e env)
            (m-apply (m-eval (car e) env) (m-eval-list (cdr e) env))))))))

(define genv (list (cons '+ '+) (cons '- '-) (cons '* '*) (cons '< '<)))

;;; Everything, in one value
;;; --------------------------------------------------------------------------------
(list
  (fact 6)                                     ; 720   anonymous recursion
  (fib 12)                                     ; 144   anonymous tree recursion
  (to-int (succ (succ (succ zero))))           ; 3     lambdas as integers
  (to-int (plus (succ zero) (succ (succ zero)))) ; 3   lambdas as arithmetic
  (m-eval '(+ 2 3) genv)                       ; 5     two interpreters deep
  (m-eval '((lambda (x) (* x x)) 7) genv)      ; 49    a closure made inside one
  (m-eval '((lambda (f n) (f f n))             ; 120   recursion inside one, by
            (lambda (self k)                   ;       self-application, with an
              (if (< k 2) 1 (* k (self self (- k 1))))) ;  environment it built itself
            5)
          genv)
  (m-eval '(((lambda (a) (lambda (b) (+ a b))) 10) 5) genv)) ; 15  a closure that escapes
