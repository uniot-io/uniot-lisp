#!/bin/bash
#
# Runs the same workloads under each garbage collection strategy and puts the numbers
# side by side. The strategy is fixed when the library is compiled, so this needs two
# binaries; `make bench` builds them.
#
#   ./bench.sh [repeats]
#
# Each workload runs `repeats` times and the fastest run is kept, since the counters are
# identical every time and only the clock varies.

set -u
REPEATS="${1:-7}"

COPY=build/bench-copying
MARK=build/bench-mark-sweep

for b in "$COPY" "$MARK"; do
  if [ ! -x "$b" ]; then
    echo "$b is missing; run 'make bench' first" >&2
    exit 1
  fi
done

A=$(mktemp) && B=$(mktemp)
trap 'rm -f "$A" "$B"' EXIT

"$COPY" "$REPEATS" > "$A" || { echo "$COPY failed" >&2; exit 1; }
"$MARK" "$REPEATS" > "$B" || { echo "$MARK failed" >&2; exit 1; }

awk -F'\t' '
  FNR==NR { a[$1 "\t" $2] = $3; next }
          { b[$1 "\t" $2] = $3; if (!($1 in seen) && $1 != "-") { order[++n] = $1; seen[$1] = 1 } }

  function val(w, k, which) { return which == "a" ? a[w "\t" k] : b[w "\t" k] }

  # A percentage change, or a dash when there is nothing to compare against.
  function delta(w, k,   x, y) {
    x = val(w, k, "a") + 0; y = val(w, k, "b") + 0
    if (x == 0) return (y == 0) ? "same" : "n/a"
    return sprintf("%+.1f%%", (y - x) * 100.0 / x)
  }

  function row(w, label, k) {
    printf "  %-22s %14s %14s   %8s\n", label, val(w, k, "a"), val(w, k, "b"), delta(w, k)
  }

  function us(w, k, which,   v) { v = val(w, k, which) + 0; return sprintf("%.1f", v / 1000.0) }

  function row_us(w, label, k) {
    printf "  %-22s %14s %14s   %8s\n", label, us(w, k, "a"), us(w, k, "b"), delta(w, k)
  }

  END {
    printf "Heap sizes are for a 64-bit host, where an object is about twice the size it is\n"
    printf "on a 32-bit device. A bare interpreter occupies %s bytes before any workload runs.\n\n", a["-\tsetup_bytes"]

    for (i = 1; i <= n; i++) {
      w = order[i]
      printf "%s -- %s\n", w, b[w "\twhat"]
      printf "  heap %s bytes\n", b[w "\theap"]
      if (a[w "\tok"] != "yes" || b[w "\tok"] != "yes") {
        printf "  copying: %s\n  mark-sweep: %s\n\n", a[w "\tok"], b[w "\tok"]
        continue
      }
      printf "  %-22s %14s %14s   %8s\n", "", "copying", "mark-sweep", "change"
      row_us(w, "total time (us)",     "wall_ns")
      row_us(w, "in collection (us)",  "gc_ns")
      row_us(w, "longest pause (us)",  "gc_ns_max")
      row(w,    "collections",         "collections")
      row(w,    "allocations",         "allocations")
      row(w,    "bytes requested",     "alloc_requested")
      row(w,    "bytes supplied",      "alloc_supplied")
      row(w,    "live bytes handled",  "traced_bytes")
      row(w,    "heap bytes walked",   "walked_bytes")
      row(w,    "peak live bytes",     "live_peak")
      row(w,    "peak system bytes",   "system_peak")
      row(w,    "free blocks at end",  "free_blocks")
      row(w,    "largest free block",  "free_largest")
      row(w,    "deepest eval",        "eval_depth_max")
      if (b[w "\tmark_stack_peak"] + 0 > 0 || b[w "\tmark_rescues"] + 0 > 0)
        printf "  %-22s %14s %14s\n", "mark stack peak", "-", b[w "\tmark_stack_peak"]
      if (b[w "\tmark_rescues"] + 0 > 0)
        printf "  %-22s %14s %14s\n", "overflow rescues", "-", b[w "\tmark_rescues"]
      printf "\n"
    }
  }
' "$A" "$B"
