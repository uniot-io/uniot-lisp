;;; A list library, written in the language itself.
;;;
;;; None of this is built in. It is here because it shows what the primitives are
;;; enough for, and because on a device you have to choose what to pay for: every
;;; definition below costs heap for as long as it is defined, between about 180 and
;;; 420 bytes of it. A device running a 7000 byte heap cannot afford all of them at
;;; once, so take the four you use and leave the rest.
;;;
;;; Wants 10000 bytes of heap and 2048 of eval stack, so all of it at once does not
;;; fit the 7000 a device ships with -- which is the point made above. Take what you use.
;;;
;;; Every function here recurses in tail position, which after tail call elimination
;;; costs no stack at all -- the length of the list does not matter. Written the
;;; natural way instead, recursing on the way back out, an ESP8266 manages about
;;; eleven levels before the recursion guard stops it. That is the whole reason for
;;; the accumulator that most of these carry.

;;; Building and taking apart
;;; --------------------------------------------------------------------------------
;; The accumulator arrives reversed, which is why `reverse` comes first: several of
;; the others end by putting one the right way round.
(defun reverse (l acc)
  (if (eql l ()) acc (reverse (cdr l) (cons (car l) acc))))

(reverse (list 1 2 3) ())               ; (3 2 1)

;; Two lists joined. Walking `a` backwards onto `b` is the tail-recursive way; the
;; obvious (cons (car a) (append (cdr a) b)) is not, and dies on a device.
(defun append (a b)
  (app1 (reverse a ()) b))
(defun app1 (a b)
  (if (eql a ()) b (app1 (cdr a) (cons (car a) b))))

(append (list 1 2) (list 3 4))          ; (1 2 3 4)

;; The integers below n, which is what most of the loops here want.
(defun iota (n)
  (iota1 n ()))
(defun iota1 (n acc)
  (if (= n 0) acc (iota1 (- n 1) (cons (- n 1) acc))))

(iota 5)                                ; (0 1 2 3 4)

;;; Walking
;;; --------------------------------------------------------------------------------
;; `map` and `filter` both collect backwards and turn the result round at the end.
(defun map (f l)   (map1 f l ()))
(defun map1 (f l acc)
  (if (eql l ()) (reverse acc ()) (map1 f (cdr l) (cons (f (car l)) acc))))

(defun filter (f l) (filter1 f l ()))
(defun filter1 (f l acc)
  (if (eql l ())
      (reverse acc ())
      (filter1 f (cdr l) (if (f (car l)) (cons (car l) acc) acc))))

(defun double (x) (* x 2))
(defun positive (x) (> x 0))

(map double (list 1 2 3))               ; (2 4 6)
(filter positive (list 1 -2 3 -4))      ; (1 3)

;; `reduce` folds the list into one value. It carries the answer so far as its
;; accumulator, so it needs no reversing and no allocation at all.
(defun reduce (f l acc)
  (if (eql l ()) acc (reduce f (cdr l) (f acc (car l)))))

(defun add (a b) (+ a b))
(defun bigger (a b) (if (> a b) a b))

(reduce add (iota 101) 0)               ; 5050
(reduce bigger (list 3 9 4) 0)          ; 9

;;; Calling a function whose arguments are a list
;;; --------------------------------------------------------------------------------
;; `apply` is not built in, and this is it. The arguments are already values, so each
;; is wrapped in a quote to stop `eval` evaluating it a second time -- without that,
;; (apply car (list (list 1 2))) would try to call 1.
(defun quoted (x) (list 'quote x))
(defun apply (f args) (eval (cons f (map quoted args))))

(apply + (list 1 2 3))                  ; 6
(apply car (list (list 1 2)))           ; 1
(apply cons (list 'a 'b))               ; (a . b)

;; What it is actually for. A rest parameter lets a function accept any number of
;; arguments, but nothing else lets it pass them on, so a wrapper cannot exist without
;; this. Summing a list is `reduce`; forwarding is `apply`.
(defun raw (a b) (list 'got a b))
(defun traced args (apply raw args))

(traced 1 2)                            ; (got 1 2)

;; Two things to know before using it on a device. It builds a form and evaluates it,
;; which allocates: about 1900 bytes for three arguments where calling directly costs
;; 216. And the call it makes is not in tail position, so a loop written through
;; `apply` consumes stack where the same loop written directly does not.

;;; Looking things up
;;; --------------------------------------------------------------------------------
;; `member` answers with the tail it found, not with #t, so a caller can carry on
;; from there. That is the convention in this family and it costs nothing.
(defun member (x l)
  (if (eql l ()) () (if (eql x (car l)) l (member x (cdr l)))))

(member 3 (list 1 2 3 4))               ; (3 4)
(member 9 (list 1 2 3 4))               ; ()

;; A list of pairs is the nearest thing to a table here. `assoc` finds one by its key.
(defun assoc (k l)
  (if (eql l ())
      ()
      (if (eql k (car (car l))) (car l) (assoc k (cdr l)))))

(defun config () (list (list 'pin 13) (list 'mode 'out)))

(assoc 'mode (config))                  ; (mode out)
(car (cdr (assoc 'pin (config))))       ; 13

;; Position, counting from zero. Note that (car ()) is () here rather than an error,
;; so running off the end gives () instead of stopping.
(defun nth (n l)
  (if (= n 0) (car l) (nth (- n 1) (cdr l))))

(nth 2 (list 10 20 30))                 ; 30
(nth 9 (list 10 20 30))                 ; ()
