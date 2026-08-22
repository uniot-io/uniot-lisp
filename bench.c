/*
 * This is a part of the Uniot project. The following compares garbage collection
 * strategies on identical work.
 * Copyright (C) 2019-2020 Uniot <contact@uniot.io>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

// The strategy is chosen when the library is compiled, so one binary can only measure
// one of them. bench.sh builds this twice and puts the two sets of numbers side by side.

#include "libminilisp.h"

#include <time.h>

#if !MINILISP_GC_STATS
#error "the benchmark needs -D MINILISP_GC_STATS=1"
#endif

#if MINILISP_GC == MINILISP_GC_COPYING
#define STRATEGY "copying"
#elif MINILISP_GC == MINILISP_GC_MARK_SWEEP
#define STRATEGY "mark-sweep"
#else
#error "unknown MINILISP_GC strategy"
#endif

static uint64_t now_ns(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// The workloads produce values only so that the interpreter has something to do with
// them; printing those values is not what is being measured, so it all goes nowhere.
static char error_text[160];

static void sink_out(const char *msg, int size)
{
  (void)msg;
  (void)size;
}

static void sink_err(const char *msg, int size)
{
  (void)size;
  if (error_text[0] == '\0')
    snprintf(error_text, sizeof(error_text), "%s", msg);
}

typedef struct
{
  const char *name;
  size_t heap;   // bytes. Sized for a 64-bit host, where every object is about twice the
                 // size it is on a 32-bit device, so roughly half of each figure is what
                 // the equivalent device heap would be
  const char *what;
  const char *code;
} Workload;

// Each ends in a small value so that printing the result of the last form costs the same
// trivial amount under either strategy.
static const Workload WORKLOADS[] = {
    {"churn", 14000,
     "garbage only: every allocation dies before the next collection",
     "(define i 0)"
     "(define junk ())"
     "(while (< i 400) (setq junk (list 1 2 3 (list 4 5) 6)) (setq i (+ i 1)))"
     "(setq junk ()) 0"},

    {"retain", 14000,
     "the opposite: almost everything allocated stays reachable",
     "(define n ())"
     "(define i 0)"
     "(while (< i 45) (setq n (cons (list i i) n)) (setq i (+ i 1)))"
     "0"},

    {"working-set", 14000,
     "a table that persists while events churn around it, as a rule engine does",
     "(define tbl ())"
     "(define i 0)"
     "(while (< i 25) (setq tbl (cons (list i (* i 2)) tbl)) (setq i (+ i 1)))"
     "(define tmp ())"
     "(setq i 0)"
     "(while (< i 300) (setq tmp (list i (list i i) i)) (setq i (+ i 1)))"
     "(setq tmp ()) 0"},

    {"closures", 14000,
     "functions and the environments they capture, rather than plain cells",
     "(define i 0)"
     "(define f ())"
     "(while (< i 200) (setq f ((lambda (x) (lambda (y) (+ x y))) i)) (setq i (+ i 1)))"
     "(setq f ()) 0"},

    {"recursion", 14000,
     "nested calls: environments chained as deep as the recursion goes",
     "(defun d (n) (if (= n 0) 0 (+ 1 (d (+ n -1)))))"
     "(define i 0)"
     "(while (< i 20) (d 40) (setq i (+ i 1)))"
     "0"},

    {"fib", 14000,
     "a call-heavy computation with a live set that stays small",
     "(defun fib (n) (if (< n 2) n (+ (fib (+ n -1)) (fib (+ n -2)))))"
     "(fib 15) 0"},

    {"mixed-sizes", 14000,
     "alternating short and long allocations, which is what breaks a heap into pieces",
     "(define a ())"
     "(define b ())"
     "(define i 0)"
     "(while (< i 150)"
     "  (setq a (list i))"
     "  (setq b (list i i i i i i i i i i i i))"
     "  (setq a ())"
     "  (setq i (+ i 1)))"
     "(setq b ()) 0"},

    {"tight-heap", 6000,
     "the same churn in a heap barely larger than the live set, so collections are constant",
     "(define keep ())"
     "(define i 0)"
     "(while (< i 12) (setq keep (cons i keep)) (setq i (+ i 1)))"
     "(define tmp ())"
     "(setq i 0)"
     "(while (< i 200) (setq tmp (list i i i)) (setq i (+ i 1)))"
     "(setq tmp ()) 0"},
};

#define WORKLOAD_COUNT (sizeof(WORKLOADS) / sizeof(WORKLOADS[0]))

// One run from a clean heap. Returns the wall time; the counters are left in lisp_stats.
static uint64_t run_once(const Workload *w, bool *ok)
{
  void *env_constructor[3];
  env_constructor[0] = NULL;
  env_constructor[1] = NULL;
  env_constructor[2] = ROOT_END;
  void *root = env_constructor;
  Obj **genv = (Obj **)(env_constructor + 1);

  error_text[0] = '\0';

  lisp_create(w->heap, 512);
  if (!lisp_is_created())
  {
    snprintf(error_text, sizeof(error_text), "could not allocate a %u byte heap",
             (unsigned)w->heap);
    *ok = false;
    return 0;
  }

  *genv = make_env(root, &Nil, &Nil);
  define_constants(root, genv);
  define_primitives(root, genv);

  // Setting up the environment allocates, and none of that is the workload. Start the
  // count from here.
  lisp_stats_reset();

  const uint64_t started = now_ns();
  *ok = lisp_eval(root, genv, w->code);
  const uint64_t elapsed = now_ns() - started;

  lisp_destroy();
  return elapsed;
}

// The bytes a bare interpreter occupies once its primitives and constants are defined.
// Nothing can run in less than this, and the allocations that build it happen before
// lisp_eval() installs an error handler, so a heap below it would abort the process
// rather than raise. Measured rather than guessed, because it depends on the width of a
// pointer and on how many primitives are defined.
static size_t measure_setup_cost(void)
{
  void *env_constructor[3];
  env_constructor[0] = NULL;
  env_constructor[1] = NULL;
  env_constructor[2] = ROOT_END;
  void *root = env_constructor;
  Obj **genv = (Obj **)(env_constructor + 1);

  lisp_create(512 * 1024, 512);
  if (!lisp_is_created())
    return (size_t)-1;

  *genv = make_env(root, &Nil, &Nil);
  define_constants(root, genv);
  define_primitives(root, genv);

  const size_t used = lisp_mem_used();
  lisp_destroy();
  return used;
}

int main(int argc, char **argv)
{
  int repeats = 7;
  if (argc > 1)
    repeats = atoi(argv[1]);
  if (repeats < 1)
    repeats = 1;

  lisp_set_printers(sink_out, NULL, sink_err);
  lisp_stats_clock = now_ns;

  const size_t setup_cost = measure_setup_cost();
  printf("-\tsetup_bytes\t%u\n", (unsigned)setup_cost);
  printf("-\tstrategy\t%s\n", STRATEGY);

  for (size_t i = 0; i < WORKLOAD_COUNT; i++)
  {
    const Workload *w = &WORKLOADS[i];

    // Leave room for the workload itself, not only for the interpreter.
    if (w->heap < setup_cost + 1024)
    {
      printf("%s\tok\theap of %u is too small; a bare interpreter needs %u\n",
             w->name, (unsigned)w->heap, (unsigned)setup_cost);
      continue;
    }

    // The workloads are deterministic, so the counters come out identical every time and
    // only the clock varies. Take the fastest run: it is the one least disturbed by
    // whatever else the machine was doing.
    uint64_t best_wall = 0;
    uint64_t best_gc = 0;
    LispStats best = {0};
    bool ok = true;

    for (int r = 0; r < repeats; r++)
    {
      bool run_ok = true;
      const uint64_t wall = run_once(w, &run_ok);
      if (!run_ok)
        ok = false;
      if (r == 0 || wall < best_wall)
      {
        best_wall = wall;
        best_gc = lisp_stats.gc_time;
        best = lisp_stats;
      }
    }

#define OUT(key, fmt, ...) printf("%s\t%s\t" fmt "\n", w->name, key, __VA_ARGS__)
    OUT("strategy", "%s", STRATEGY);
    OUT("what", "%s", w->what);
    OUT("heap", "%u", (unsigned)w->heap);
    OUT("ok", "%s", ok ? "yes" : error_text);
    OUT("wall_ns", "%llu", (unsigned long long)best_wall);
    OUT("gc_ns", "%llu", (unsigned long long)best_gc);
    OUT("gc_ns_max", "%llu", (unsigned long long)best.gc_time_max);
    OUT("collections", "%u", (unsigned)best.collections);
    OUT("allocations", "%u", (unsigned)best.allocations);
    OUT("alloc_requested", "%u", (unsigned)best.alloc_requested);
    OUT("alloc_supplied", "%u", (unsigned)best.alloc_supplied);
    OUT("traced_bytes", "%u", (unsigned)best.traced_bytes);
    OUT("walked_bytes", "%u", (unsigned)best.walked_bytes);
    OUT("live_peak", "%u", (unsigned)best.live_peak);
    OUT("system_peak", "%u", (unsigned)best.system_peak);
    OUT("free_blocks", "%u", (unsigned)best.free_blocks);
    OUT("free_largest", "%u", (unsigned)best.free_largest);
    OUT("mark_stack_peak", "%u", (unsigned)best.mark_stack_peak);
    OUT("mark_rescues", "%u", (unsigned)best.mark_rescues);
    OUT("eval_depth_max", "%u", (unsigned)eval_depth_max);
#undef OUT
  }

  return 0;
}
