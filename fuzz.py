#!/usr/bin/env python3
#
# Differential fuzzing for the interpreter.
#
# There is no oracle for "what should this program print", and writing one would mean
# writing a second interpreter. What there is instead is four builds of this one that
# must agree: two collectors, two word sizes, and a sanitizer build. A generated program
# on which any two of them disagree has found a bug in whichever differs, and needs no
# expected answer to be written down.
#
# The generator produces programs that terminate, because a program that runs forever
# tells us nothing and the ones that stop are where the interesting disagreements are.
# It leans on the constructs most likely to expose a collector or evaluator fault:
# allocation in the middle of building a structure, closures capturing an environment
# that later moves, macros expanding into tail position, and deep-but-bounded recursion.
#
#   ./fuzz.py [iterations] [--seed N]

import random
import subprocess
import sys
import re

BUILDS = {
    "mark-sweep": "./repl-mark-sweep",
    "copying": "./repl-copying",
}
SAN = "./repl-san"
WORD32 = "./repl32"

HEAP = "60000"          # generous, so that memory exhaustion is not what differs
STACK = "262144"
TIMEOUT = 10

ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


# ---------------------------------------------------------------- generation

ATOMS = ["0", "1", "2", "3", "7", "-1", "-4", "100", "#t", "()", "'a", "'b", "'sym"]

def atom(r):
    return r.choice(ATOMS)

def num(r):
    return str(r.choice([0, 1, 2, 3, 5, -1, -3, 10, 99]))

def expr(r, d, vars):
    """One expression. `d` is the remaining depth; `vars` are names in scope."""
    if d <= 0:
        return r.choice(vars) if vars and r.random() < 0.4 else atom(r)

    k = r.randint(0, 15)
    e = lambda: expr(r, d - 1, vars)

    if k == 0:                                        # arithmetic, incl. the checked paths
        op = r.choice(["+", "-", "*"])
        return f"({op} {num(r)} {num(r)})"
    if k == 1:
        return f"({r.choice(['/', '%'])} {num(r)} {r.choice(['1','2','3','7'])})"
    if k == 2:                                        # comparison and equality
        return f"({r.choice(['<', '<=', '>', '>=', '=', 'eql'])} {num(r)} {num(r)})"
    if k == 3:                                        # list building, the allocation path
        return f"(list {e()} {e()})"
    if k == 4:
        return f"(cons {e()} {e()})"
    if k == 5:                                        # accessors, incl. () cases
        return f"({r.choice(['car', 'cdr'])} (list {e()} {e()}))"
    if k == 6:                                        # predicates
        return f"({r.choice(['numberp','symbolp','consp','listp','atom','functionp','not'])} {e()})"
    if k == 7:                                        # branching
        return f"(if {e()} {e()} {e()})"
    if k == 8:                                        # short-circuit
        return f"({r.choice(['and', 'or'])} {e()} {e()})"
    if k == 9:
        return f"(progn {e()} {e()})"
    if k == 10:                                       # a closure, applied at once
        v = f"v{r.randint(0, 9)}"
        return f"((lambda ({v}) {expr(r, d - 1, vars + [v])}) {e()})"
    if k == 11:                                       # a closure that captures and escapes
        v = f"c{r.randint(0, 9)}"
        return f"(((lambda ({v}) (lambda () {expr(r, d - 1, vars + [v])})) {e()}))"
    if k == 12:                                       # rest parameters
        return f"((lambda args (length args)) {e()} {e()})"
    if k == 13:                                       # apply, the tail path
        return f"(apply {r.choice(['+', 'list', 'cons'])} (list {num(r)} {num(r)}))"
    if k == 14:                                       # mutation of a fresh structure
        return f"(progn (define m{r.randint(0,9)} (list {e()} {e()})) " \
               f"({r.choice(['setcar', 'setcdr'])} m{r.randint(0,9)} {e()}) m{r.randint(0,9)})"
    return f"(length (list {e()} {e()} {e()}))"


def program(r):
    """A whole program: some definitions, then forms that use them."""
    out = []
    n = r.randint(1, 3)

    for i in range(n):
        kind = r.randint(0, 3)
        f = f"f{i}"
        if kind == 0:                                  # bounded recursion, tail position
            out.append(f"(defun {f} (n acc) (if (= n 0) acc ({f} (- n 1) (+ acc n))))")
            out.append(f"({f} {r.randint(0, 40)} 0)")
        elif kind == 1:                                # non-tail recursion, bounded low
            out.append(f"(defun {f} (n) (if (= n 0) 0 (+ 1 ({f} (- n 1)))))")
            out.append(f"({f} {r.randint(0, 12)})")
        elif kind == 2:                                # a macro expanding into tail position
            out.append(f"(defmacro {f} (a b) (list 'if a b ()))")
            out.append(f"({f} {expr(r, 2, [])} {expr(r, 2, [])})")
        else:                                          # a plain function over the generated exprs
            out.append(f"(defun {f} (x y) {expr(r, 3, ['x', 'y'])})")
            out.append(f"({f} {expr(r, 2, [])} {expr(r, 2, [])})")

    for _ in range(r.randint(1, 4)):
        out.append(expr(r, r.randint(2, 4), []))

    # A while loop, bounded, touching the loop counter and the shared budget.
    if r.random() < 0.4:
        out.append(f"(define w 0) (while (< #itr {r.randint(1, 30)}) (setq w (+ w 1))) w")

    return "\n".join(out) + "\n"


# ---------------------------------------------------------------- running

def run(binary, src, extra_env=None):
    env = {"MINILISP_HEAP_SIZE": HEAP, "MINILISP_MAX_EVAL_STACK": STACK, "PATH": "/usr/bin:/bin"}
    if extra_env:
        env.update(extra_env)
    try:
        p = subprocess.run([binary], input=src, capture_output=True, text=True,
                           timeout=TIMEOUT, env=env)
    except subprocess.TimeoutExpired:
        return None, "TIMEOUT", ""
    out = ANSI.sub("", p.stdout).strip()
    err = ANSI.sub("", p.stderr).strip()
    return p.returncode, out, err


def main():
    iterations = int(sys.argv[1]) if len(sys.argv) > 1 else 200
    seed = 0
    if "--seed" in sys.argv:
        seed = int(sys.argv[sys.argv.index("--seed") + 1])

    findings = []
    for i in range(iterations):
        r = random.Random(seed + i)
        src = program(r)

        results = {}
        for name, binary in BUILDS.items():
            rc, out, err = run(binary, src)
            results[name] = (rc, out, err)

        # A timeout is not by itself a disagreement -- one collector can be slower --
        # but it is worth knowing about.
        if any(out == "TIMEOUT" for _, out, _ in results.values()):
            findings.append(("timeout", i, src, results))
            continue

        # Memory exhaustion is the one outcome the two collectors may legitimately
        # differ on, since their capacity differs. Everything else must match.
        vals = {n: (o, e) for n, (_, o, e) in results.items()}
        if any("Memory exhausted" in o + e for o, e in vals.values()):
            continue

        if len(set(vals.values())) > 1:
            findings.append(("collector disagreement", i, src, results))
            continue

        # The sanitizer build must agree and must report nothing.
        rc, out, err = run(SAN, src)
        if "runtime error" in err or "AddressSanitizer" in err:
            findings.append(("sanitizer", i, src, {"san": (rc, out, err)}))
            continue

        # 32-bit: the same values, though memory behaviour differs.
        rc32, out32, err32 = run(WORD32, src)
        if "Memory exhausted" not in out32 + err32 and out32 != "TIMEOUT":
            ref_out, ref_err = vals["mark-sweep"]
            if (out32, err32) != (ref_out, ref_err):
                findings.append(("32-bit disagreement", i, src,
                                 {"64-bit": (0, ref_out, ref_err), "32-bit": (rc32, out32, err32)}))

        if (i + 1) % 25 == 0:
            print(f"  {i+1}/{iterations} programs, {len(findings)} finding(s)", flush=True)

    print(f"\n  {iterations} programs, {len(findings)} finding(s)")
    for kind, i, src, results in findings[:8]:
        print(f"\n  --- {kind} (seed {seed + i}) ---")
        for line in src.strip().split("\n")[:10]:
            print(f"    | {line}")

        # Show the line where they part company, not the first line of output --
        # the first line is almost always a <function> echo and says nothing.
        names = list(results.keys())
        streams = {n: ((o or "") + ("\n" + e if e else "")).split("\n")
                   for n, (_, o, e) in results.items()}
        depth = max(len(v) for v in streams.values())
        shown = False
        for ln in range(depth):
            row = {n: (streams[n][ln] if ln < len(streams[n]) else "<no output>") for n in names}
            if len(set(row.values())) > 1:
                print(f"    diverges at output line {ln + 1}:")
                for n in names:
                    print(f"      {n:<22} {row[n][:88]}")
                shown = True
                break
        if not shown:
            for n in names:
                rc, out, err = results[n]
                print(f"      {n:<22} rc={rc} {(out or err).split(chr(10))[0][:88]}")
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
