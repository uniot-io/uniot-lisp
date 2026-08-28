#!/usr/bin/env python3
#
# The REPL's line editing, driven through a real pty because there is no other way to
# test it: the editor deliberately does nothing at all unless both ends are a terminal,
# so a pipe exercises the fallback path and proves nothing about the editor.
#
# Run with: make test-repl

import os, pty, re, select, sys, tempfile, time

_keep_escapes = False

REPL = sys.argv[1] if len(sys.argv) > 1 else "build/repl"
HOME = sys.argv[2] if len(sys.argv) > 2 else tempfile.mkdtemp()

def session(keys, settle=0.30):
    pid, fd = pty.fork()
    if pid == 0:
        os.environ["HOME"] = HOME
        os.environ["TERM"] = "xterm"
        os.execv(REPL, [REPL])
    time.sleep(settle)
    out = b""
    for k in keys:
        os.write(fd, k.encode())
        time.sleep(settle)
        while select.select([fd], [], [], 0.05)[0]:
            try:
                chunk = os.read(fd, 65536)
            except OSError:
                break
            if not chunk: break
            out += chunk
    try:
        while select.select([fd], [], [], 0.3)[0]:
            chunk = os.read(fd, 65536)
            if not chunk: break
            out += chunk
    except OSError:
        pass
    os.close(fd); os.waitpid(pid, 0)
    text = out.decode(errors="replace")
    if _keep_escapes:
        return text
    return re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", text)

UP, DOWN, LEFT, RIGHT = "\x1b[A", "\x1b[B", "\x1b[D", "\x1b[C"
HOME_K, END_K, DEL = "\x1b[H", "\x1b[F", "\x1b[3~"
CTRL = lambda c: chr(ord(c.upper()) - 64)

# The interesting output is always a whole line on its own -- a value, or something
# print wrote -- so compare against the lines rather than hunting substrings, which is
# what made the first version of this file assert things that were merely plausible.
# Two checks that have to see the bytes as they were written, not a tidied version.
# lines_of() below strips each line, which is right for comparing values and is exactly
# what let a display bug through: output creeping one column right per line survives
# .strip() untouched.

def session_raw(keys):
    """The stream with its escape sequences left in."""
    global _keep_escapes
    _keep_escapes = True
    try:
        return session(keys)
    finally:
        _keep_escapes = False


def check_no_bare_linefeed(name, keys):
    raw = session_raw(keys)
    # The creeping indent is not in the bytes -- there are no spaces to find. It is what
    # the terminal does with a line feed that has no carriage return: move down a row and
    # stay in the same column. OPOST supplies the carriage return, and raw mode turns
    # OPOST off, so the invariant to check is that every newline has its \r.
    bare = [i for i, ch in enumerate(raw) if ch == "\n" and (i == 0 or raw[i - 1] != "\r")]
    ok = not bare
    print(("  ok   " if ok else "  FAIL ") + name)
    if not ok:
        print("         %d newline(s) with no carriage return, e.g. %r"
              % (len(bare), raw[max(0, bare[0] - 12):bare[0] + 4]))
    return ok


def check_no_colour_bleed(name, keys):
    raw = session_raw(keys)
    # Walk the stream and keep track of whether a colour is in force. The prompt is
    # written straight to the descriptor while the colours go through stdio, so if a
    # reset is left sitting in the buffer the prompt comes out wearing the colour.
    active = None
    bleeding = False
    for piece in re.split(r"(\x1b\[[0-9;]*m)", raw):
        m = re.match(r"\x1b\[([0-9;]*)m", piece)
        if m:
            active = None if m.group(1) in ("", "0") else m.group(1)
        elif "unlisp>" in piece and active is not None:
            bleeding = True
    ok = not bleeding
    print(("  ok   " if ok else "  FAIL ") + name)
    if not ok:
        print("         the prompt is drawn while colour %s is in force" % active)
    return ok


def lines_of(text):
    return [l.strip() for l in text.replace("\r\n", "\n").split("\n") if l.strip()]

def check(name, keys, want_lines):
    got = lines_of(session(keys))
    ok = all(got.count(w) >= n for w, n in want_lines)
    print(("  ok   " if ok else "  FAIL ") + name)
    if not ok:
        print("         wanted %r in:\n%s" % (want_lines,
              "\n".join("           | " + l for l in got[-8:])))
    return ok

fails = 0
fails += not check("evaluates a typed line",      ["(+ 1 2)\r", CTRL('d')], [("3", 1)])
# Recalled, so the text appears a second time and is evaluated a second time.
fails += not check("up arrow recalls",            ["(+ 1 2)\r", UP, "\r", CTRL('d')], [("3", 2)])
fails += not check("up twice, down once",         ["(+ 1 1)\r", "(+ 2 2)\r", UP, UP, DOWN, "\r", CTRL('d')], [("4", 2)])
# The 0 lands before the 2, not after it, which is what proves the cursor moved.
fails += not check("left arrow + insert",         ["(+ 1 2)", LEFT, "0", "\r", CTRL('d')], [("21", 1)])
fails += not check("ctrl-a prepends",             ["+ 1 2)", CTRL('a'), "(", "\r", CTRL('d')], [("3", 1)])
fails += not check("ctrl-e appends",              ["(+ 1 2", CTRL('a'), CTRL('e'), ")", "\r", CTRL('d')], [("3", 1)])
fails += not check("backspace",                   ["(+ 1 29\x7f)\r", CTRL('d')], [("3", 1)])
# Home, then five characters right, puts the cursor on the 9 of 92; Delete takes it.
fails += not check("home and delete keys",        ["(+ 1 92)", HOME_K, RIGHT*5, DEL, END_K, "\r", CTRL('d')], [("3", 1)])
fails += not check("end key",                     ["(+ 1 2", HOME_K, END_K, ")", "\r", CTRL('d')], [("3", 1)])
fails += not check("ctrl-u clears to start",      ["garbage", CTRL('u'), "(+ 1 2)\r", CTRL('d')], [("3", 1)])
fails += not check("ctrl-k kills to end",         ["(+ 1 2)xxx", LEFT*3, CTRL('k'), "\r", CTRL('d')], [("3", 1)])
fails += not check("ctrl-w deletes a word",       ["(+ 1 2) junk", CTRL('w'), "\r", CTRL('d')], [("3", 1)])
# Swaps the two characters before the cursor: 21 becomes 12.
fails += not check("ctrl-t transposes",           ["(+ 21)", LEFT, CTRL('t'), "\r", CTRL('d')], [("12", 1)])
fails += not check("multi-line form",             ["(defun f (x)\r", "  (* x 3))\r", "(f 7)\r", CTRL('d')], [("21", 1)])
# The prompt is rewritten in place, so it never ends up on a line of its own to compare
# against -- this one has to look at the raw stream.
got = session(["(defun f (x)\r", "  (* x 3))\r", CTRL('d')])
ok = "..." in got and "unlisp>" in got
print(("  ok   " if ok else "  FAIL ") + "continuation prompt")
fails += not ok
fails += not check("ctrl-c abandons open form",   ["(+ 1", CTRL('c'), "(+ 4 5)\r", CTRL('d')], [("9", 1)])
# print writes its own line and the form still evaluates to 3: two lines, not one.
fails += not check("print is separate from value",["(+ 1 (print 2))\r", CTRL('d')], [("2", 1), ("3", 1)])

# Layout. Both of these were live bugs: raw mode was left on across evaluation, so the
# interpreter's newlines never returned the carriage, and the colour reset was still in
# the stdio buffer when the prompt was written straight to the descriptor underneath it.
fails += not check_no_bare_linefeed("output returns to the left margin",
                                      ["(+ (print 1) (print 2))\r", "(progn (print 1) (print 2))\r", CTRL('d')])
fails += not check_no_colour_bleed("prompt is not painted by the last colour",
                                   ["(+ 1 (print 2))\r", "(+ 1 2)\r", CTRL('d')])

# ^C means two different things depending on which mode the terminal is in, and both
# have to work. While a line is being edited it is a character (ISIG is off) and abandons
# the line; during evaluation it is a signal, and without a handler it would end the
# session -- which is a regression the editor introduced by handing the terminal back
# between reads, not something that was ever true before.
def check_interrupt(name):
    pid, fd = pty.fork()
    if pid == 0:
        os.environ["HOME"] = tempfile.mkdtemp()
        os.environ["TERM"] = "xterm"
        # A loop has to still be running when the interrupt lands, and it has to stay
        # under its own iteration limit or it raises for the wrong reason. Twenty
        # thousand turns is twelve milliseconds here, so the time comes from collecting
        # on every allocation rather than from doing more turns.
        os.environ["MINILISP_ALWAYS_GC"] = "1"
        os.execv(REPL, [REPL])
    time.sleep(0.4)
    os.write(fd, b"(define n 0)(while (< #itr 19999) (setq n (list 1 2 3 4 5 6 7 8 9 10)))\r")
    time.sleep(0.15)
    os.write(fd, b"\x03")
    time.sleep(0.6)
    os.write(fd, b"(+ 40 2)\r")   # the session has to still be there
    time.sleep(0.5)
    os.write(fd, b"\x04")
    time.sleep(0.4)
    out = b""
    try:
        while select.select([fd], [], [], 0.4)[0]:
            chunk = os.read(fd, 65536)
            if not chunk:
                break
            out += chunk
    except OSError:
        pass
    os.close(fd)
    status = os.waitpid(pid, 0)[1]
    got = lines_of(re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", out.decode(errors="replace")))
    ok = (any("Interrupted" in l for l in got) and "42" in got and os.WIFEXITED(status))
    print(("  ok   " if ok else "  FAIL ") + name)
    if not ok:
        print("         status %d, tail: %r" % (status, got[-6:]))
    return ok


fails += not check_interrupt("ctrl-c during evaluation raises, session survives")

# History persists across sessions.
session(["(+ 40 2)\r", CTRL('d')])
fails += not check("history survives restart",    [UP, "\r", CTRL('d')], [("42", 1)])

print("\n%s" % ("all interactive checks passed" if fails == 0 else "%d interactive check(s) failed" % fails))
sys.exit(1 if fails else 0)
