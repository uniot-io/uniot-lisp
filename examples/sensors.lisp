;;; Filters for a noisy input, which is most of what a device rule does.
;;;
;;; A rule runs again every time something changes, so it needs to remember what it
;;; saw last. There are two ways to keep that here, and this file uses the second:
;;;
;;;   - a global, set with `setq`. Simple, and there is one of them.
;;;   - a closure. A function returned from another function keeps the environment it
;;;     was made in, and `setq` inside it writes to that -- so each one made has state
;;;     of its own, and nothing else can reach it.
;;;
;;; Wants 10000 bytes of heap and 3072 of eval stack -- more than the 7000/2048 a
;;; device ships with, because every filter here is defined at once. One or two of them
;;; is what a real rule uses, and that fits.
;;;
;;; Readings are fed from a list here so the output can be checked. On a device they
;;; would arrive one at a time from an event.
;;;
;;; Arithmetic is integer only: (/ 7 2) is 3.

(defun reverse (l acc)
  (if (eql l ()) acc (reverse (cdr l) (cons (car l) acc))))
(defun map (f l) (map1 f l ()))
(defun map1 (f l acc)
  (if (eql l ()) (reverse acc ()) (map1 f (cdr l) (cons (f (car l)) acc))))

;;; State that survives the call
;;; --------------------------------------------------------------------------------
;; `n` belongs to the call of `make-counter` that produced this lambda, and stays
;; alive as long as the lambda does. Two counters do not share it.
(defun make-counter ()
  ((lambda (n) (lambda () (setq n (+ n 1)) n)) 0))

(define tick (make-counter))
(define tock (make-counter))
(list (tick) (tick) (tick) (tock))      ; (1 2 3 1)

;;; Smoothing
;;; --------------------------------------------------------------------------------
;; A running average that keeps no history: each reading moves the average a fraction
;; of the way towards itself. Larger `weight` means slower and smoother. This is what
;; to reach for when a sensor jitters and you do not want to store a window.
(defun make-smoother (weight)
  ((lambda (avg first)
     (lambda (x)
       (if first
           (progn (setq first ()) (setq avg x))
           (setq avg (+ avg (/ (- x avg) weight))))
       avg))
   0 #t))

(define smooth (make-smoother 4))
(map smooth (list 100 100 180 100 100 100 100 100))
;; => (100 100 120 115 112 109 107 106)   the spike is absorbed, not followed

;;; A threshold that does not chatter
;;; --------------------------------------------------------------------------------
;; One threshold on a noisy signal switches back and forth every time the reading
;; wobbles across it. Two thresholds fix that: it takes `high` to turn on and `low` to
;; turn off, and between them nothing changes. A Schmitt trigger, in hardware terms.
(defun make-trigger (low high)
  ((lambda (state)
     (lambda (x)
       (if (>= x high) (setq state 'on) ())
       (if (<= x low) (setq state 'off) ())
       state))
   'off))

(define hot (make-trigger 20 25))
(map hot (list 18 24 26 24 22 21 20 19 26))
;; => (off off on on on on off off on)    24 does not flip it either way

;; With a single threshold at 25 the same readings rattle:
(defun make-plain (limit)
  (lambda (x) (if (>= x limit) 'on 'off)))
(define plain (make-plain 25))
(map plain (list 24 25 24 25 24 25))    ; (off on off on off on)

;;; Believing a change only when it repeats
;;; --------------------------------------------------------------------------------
;; A button bounces, and a radio drops a reading. `make-debounce` reports a new value
;; only after seeing it `n` times running, so a single stray reading is ignored.
(defun make-debounce (n)
  ((lambda (stable candidate count)
     (lambda (x)
       (if (eql x candidate)
           (setq count (+ count 1))
           (progn (setq candidate x) (setq count 1)))
       (if (>= count n) (setq stable x) ())
       stable))
   'off 'off 0))

(define steady (make-debounce 3))
(map steady (list 'off 'on 'off 'on 'on 'on 'off 'on 'on 'on))
;; => (off off off off off on on on on on)   the lone 'on at position 1 is dropped

;;; Acting only when something changed
;;; --------------------------------------------------------------------------------
;; Publishing every reading wastes a radio. This reports the value when it differs
;; from the last one and () otherwise, so a caller can skip the work.
(defun make-edge ()
  ((lambda (last first)
     (lambda (x)
       (if (if first #t (eql x last))
           (progn (setq first ()) (setq last x) ())
           (progn (setq last x) x))))
   () #t))

(define changed (make-edge))
(map changed (list 5 5 5 7 7 9 9 9))    ; (() () () 7 () 9 () ())

;;; Putting them together
;;; --------------------------------------------------------------------------------
;; The point of building each as a function of one reading is that they compose: a
;; noisy temperature, smoothed, then triggered, then reported only when it changes.
(define t-smooth (make-smoother 4))
(define t-trigger (make-trigger 20 25))
(define t-edge (make-edge))

(defun handle (x) (t-edge (t-trigger (t-smooth x))))

(map handle (list 10 30 30 30 30 30 30 10 10 10 10 10 10 10))
;; => (() () () () () () on () off () () () () ())
;;    two events out of fourteen readings, which is what the radio has to send.
;;    The smoother is why they arrive late: it takes a few readings to climb past 25
;;    and a few more to fall back under 20, which is the cost of not chattering.
