;;; This version uses the interpreter's own primitives wherever it has them.
;;; The definitions they replace are commented out rather than deleted, so the two
;;; can be compared: examples/life-portable.lisp keeps every one of them and only
;;; renames them out of the way. Both should print-board the same thing.
;;;
;;; Conway's game of life
;;;

;; (progn expr ...)
;; => ((lambda () expr ...))
;; Superseded by the interpreter's own `progn`, which is a special form: it sequences
;; without building a lambda and an environment frame per use, and its last expression is
;; a tail position. Kept here because it shows how the macro is written when the host
;; does not provide one.
;; (defmacro progn (expr . rest)
;;   (list (cons 'lambda (cons () (cons expr rest)))))

;; (defun list (x . y)
;;   (cons x y))

;; (defun not (x)
;;   (if x () t))

;; (let var val body ...)
;; => ((lambda (var) body ...) val)
(defmacro let (var val . body)
  (cons (cons 'lambda (cons (list var) body))
	(list val)))

;; (and e1 e2 ...)
;; => (if e1 (and e2 ...))
;; (and e1)
;; => e1
;; (defmacro and (expr . rest)
;;   (if rest
;;       (list 'if expr (cons 'and rest))
;;     expr))

;; (or e1 e2 ...)
;; => (let <tmp> e1
;;      (if <tmp> <tmp> (or e2 ...)))
;; (or e1)
;; => e1
;;
;; The reason to use the temporary variables is to avoid evaluating the
;; arguments more than once.
;; (defmacro or (expr . rest)
;;   (if rest
;;       (let var (gensym)
;;            (list 'let var expr
;;                  (list 'if var var (cons 'or rest))))
;;     expr))

;; (when expr body ...)
;; => (if expr (progn body ...))
(defmacro when (expr . body)
  (cons 'if (cons expr (list (cons 'progn body)))))

;; (unless expr body ...)
;; => (if expr () body ...)
(defmacro unless (expr . body)
  (cons 'if (cons expr (cons () body))))

;;;
;;; Numeric operators
;;;

;; (defun <= (e1 e2)
;;   (or (< e1 e2)
;;       (= e1 e2)))

;;;
;;; List operators
;;;

;;; Applies each element of lis to fn, and returns their return values as a list.
(defun map (lis fn)
  (when lis
    (cons (fn (car lis))
	  (map (cdr lis) fn))))

;; Returns nth element of lis.
(defun nth (lis n)
  (if (= n 0)
      (car lis)
    (nth (cdr lis) (- n 1))))

;; Returns the nth tail of lis.
(defun nth-tail (lis n)
  (if (= n 0)
      lis
    (nth-tail (cdr lis) (- n 1))))

;; Returns a list consists of m .. n-1 integers.
(defun %iota (m n)
  (unless (<= n m)
    (cons m (%iota (+ m 1) n))))

;; Returns a list consists of 0 ... n-1 integers.
(defun iota (n)
  (%iota 0 n))

;;;
;;; Main
;;;

(define width 10)
(define height 10)

;; Returns location (x, y)'s element.
(defun get (board x y)
  (nth (nth board y) x))

;; Returns true if location (x, y)'s value is "@".
(defun alive? (board x y)
  (and (<= 0 x)
       (< x height)
       (<= 0 y)
       (< y width)
       (eql (get board x y) '@)))

;; Print out the given board.
(defun print-board (board)
  (if (not board)
      '$
    (print (car board))
    (print-board (cdr board))))

(defun count (board x y)
  (let at (lambda (x y)
            (if (alive? board x y) 1 0))
       (+ (at (- x 1) (- y 1))
          (at (- x 1) y)
          (at (- x 1) (+ y 1))
          (at x (- y 1))
          (at x (+ y 1))
          (at (+ x 1) (- y 1))
          (at (+ x 1) y)
          (at (+ x 1) (+ y 1)))))

(defun next (board x y)
  (let c (count board x y)
       (if (alive? board x y)
           (or (= c 2) (= c 3))
         (= c 3))))

(defun generations () 4)

(defun run (board)
  ;; Upstream looped forever -- it was a screen demo you interrupted by hand.
  ;; Bounded here so the example terminates and can be compared against
  ;; examples/life-portable.lisp.
  (define gen 0)
  (while (< gen (generations))
    (setq gen (+ gen 1))
    (print-board board)
    (print '*)
    (let newboard (map (iota height)
                       (lambda (y)
                         (map (iota width)
                              (lambda (x)
                                (if (next board x y) '@ '_)))))
         (setq board newboard))))

(run '((_ _ _ _ _ _ _ _ _ _)
       (_ _ _ _ _ _ _ _ _ _)
       (_ _ _ _ _ _ _ _ _ _)
       (_ _ _ _ _ _ _ _ _ _)
       (_ _ _ _ _ _ _ _ _ _)
       (_ _ _ _ _ _ _ _ _ _)
       (_ _ _ _ _ _ _ _ _ _)
       (_ @ @ @ _ _ _ _ _ _)
       (_ _ _ @ _ _ _ _ _ _)
       (_ _ @ _ _ _ _ _ _ _)))
