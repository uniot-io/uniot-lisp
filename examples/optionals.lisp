;;; Optional arguments, which this Lisp does not have.
;;;
;;; There is no &optional and no &key. What there is is a rest parameter, and it is
;;; enough: an optional argument is one you look for in the rest instead of naming.
;;;
;;; Two things make this work, and neither is obvious from the list of primitives:
;;;   - `define` inside a body binds locally; the name is gone when the call returns.
;;;   - a rest list is a list, so it is false when empty and true when not, which is
;;;     the "was it supplied?" test.
;;;
;;; Runs on an ESP8266 as it stands: 7000 bytes of heap, 1024 of eval stack.
;;;
;;; Each form below is written for its value, which the host prints. A script running
;;; on a device would call `print` instead.

;;; One optional
;;; --------------------------------------------------------------------------------
;; `rest` holds everything after `name`. If it holds anything the caller supplied a
;; greeting, and if not, the default stands.
(defun greet (name . rest)
  (define greeting (if rest (car rest) 'hello))
  (list greeting name))

(greet 'world)                          ; (hello world)
(greet 'world 'goodbye)                 ; (goodbye world)

;;; Several optionals
;;; --------------------------------------------------------------------------------
;; Reaching into the rest by hand stops being readable at the second argument, so pull
;; the pattern out. `opt` walks n places into args and gives up with the default. It
;; calls itself in tail position, so the walk costs no stack however long the list.
(defun opt (args n default)
  (if (eql args ())
      default
      (if (= n 0)
          (car args)
          (opt (cdr args) (- n 1) default))))

(defun draw (x . o)
  (list x (opt o 0 0) (opt o 1 'black) (opt o 2 1)))

(draw 10)                               ; (10 0 black 1)
(draw 10 20)                            ; (10 20 black 1)
(draw 10 20 'red)                       ; (10 20 red 1)
(draw 10 20 'red 3)                     ; (10 20 red 3)

;;; Ask whether it was passed, not what it was
;;; --------------------------------------------------------------------------------
;; `opt` tests the list, not the value in it, and that distinction is the whole reason
;; it behaves. A caller who passes () explicitly meant to pass (), and gets it back --
;; the default is only for the argument that is not there at all.
(draw 10)                               ; (10 0 black 1)      y absent
(draw 10 ())                            ; (10 () black 1)     y present, and is ()

;; Written the other way round it cannot tell those apart, because every false value
;; looks like an absent one. This is the version to avoid.
(defun draw-badly (x . o)
  (list x (if (car o) (car o) 0)))

(draw-badly 10)                         ; (10 0)
(draw-badly 10 ())                       ; (10 0)   <- the () the caller asked for is gone
(draw-badly 10 20)                      ; (10 20)

;;; What is still checked
;;; --------------------------------------------------------------------------------
;; A rest parameter only absorbs the surplus. Arguments before it stay required, and
;; calling without them raises rather than quietly binding them to (), which is the
;; mistake an optional argument would otherwise hide:
;;
;;   (draw)  =>  Cannot apply function: too few arguments
;;
;; Optionals here are also positional and trailing, which is what &optional gives you
;; in any case. Keyword arguments are the thing this cannot imitate.
