/*
 * This is a part of the Uniot project. The following is the user apps interpreter.
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

#include "libminilisp.h"
#include "line_edit.h"

#include <signal.h>
#include <unistd.h>

#define ANSI_COLOR_RED "\x1b[31m"
#define ANSI_COLOR_GREEN "\x1b[32m"
#define ANSI_COLOR_CYAN "\x1b[36m"
#define ANSI_COLOR_RESET "\x1b[0m"

void *env_constructor[3];
void *root = NULL;
Obj **genv;

// Three sinks, three colours, because they are three different things and telling them
// apart at a glance is the whole reason the library keeps them separate. Green is the
// value a form evaluated to, cyan is what the script asked to print, red is a failure.
// On a device these are separate channels; here they are one terminal, so colour is
// what stands in for that.
// The reset goes before the newline, not after it. After it, the colour is still in
// force at the moment the line ends, and anything that reaches the terminal before the
// reset does -- a prompt, another stream -- comes out wearing it.
void printOut(const char *msg, int size)
{
  fprintf(stdout, ANSI_COLOR_GREEN "%s" ANSI_COLOR_RESET "\n", msg);
}

void printLog(const char *msg, int size)
{
  fprintf(stdout, ANSI_COLOR_CYAN "%s" ANSI_COLOR_RESET "\n", msg);
}

void printErr(const char *msg, int size)
{
  fprintf(stderr, ANSI_COLOR_RED "%s" ANSI_COLOR_RESET "\n", msg);
}

// While a line is being edited the terminal is in raw mode with ISIG off, so ^C arrives
// as an ordinary character and abandons the line. During evaluation the terminal is back
// in cooked mode and ^C is a signal, which without this would end the session -- the
// worse outcome of the two, and one that only appeared once the editor started handing
// the terminal back between reads.
//
// The handler does nothing but set the flag. Raising from inside it is not allowed: it
// would longjmp() out of a signal handler into a stack that is still running. The flag
// is read by the yield hook instead, which the interpreter calls from its loops.
static volatile sig_atomic_t interrupted = 0;

static void on_sigint(int signum)
{
  (void)signum;
  interrupted = 1;
}

static void checkInterrupt(void)
{
  if (interrupted) {
    interrupted = 0;
    error("Interrupted");
  }
}

// Set when the variable exists and is not empty, so that `VAR= ./repl` reads as off
// and `VAR=1 ./repl` reads as on.
static bool env_flag(const char *name)
{
  const char *value = getenv(name);
  return value != NULL && value[0] != '\0';
}

// Test hooks, present only in the REPL. They let the suite assert what a collection
// actually does, which behavioural tests cannot observe.

// (mem) -> bytes currently held by live objects.
static Obj *prim_mem(void *root, Obj **env, Obj **list)
{
  return make_int(root, (int)lisp_mem_used());
}

// (gc) -> collect now, so a test can measure what was reclaimed.
static Obj *prim_gc(void *root, Obj **env, Obj **list)
{
  gc(root);
  return Nil;
}

int main()
{
  lisp_set_printers(printOut, printLog, printErr);
  lisp_set_cycle_yield(checkInterrupt);
  signal(SIGINT, on_sigint);

  env_constructor[0] = root;
  env_constructor[1] = NULL;
  env_constructor[2] = ROOT_END;
  root = env_constructor;
  genv = (Obj **)(env_constructor + 1);

  // Overridable so that GC behaviour can be exercised at realistic embedded heap
  // sizes without building a huge program.
  const char *heap_env = getenv("MINILISP_HEAP_SIZE");
  size_t heap_size = (heap_env != NULL && heap_env[0] != '\0') ? (size_t)atoi(heap_env) : 40000;
  // Generous by default: a workstation stack is large, and the limit exists to catch
  // runaway recursion rather than to emulate a device. In bytes of C stack.
  const char *stack_env = getenv("MINILISP_MAX_EVAL_STACK");
  size_t eval_stack_limit =
      (stack_env != NULL && stack_env[0] != '\0') ? (size_t)atol(stack_env) : 256 * 1024;

  lisp_create(heap_size, eval_stack_limit);
  if (!lisp_is_created())
  {
    fprintf(stderr, "failed to allocate a %u byte heap\n", (unsigned)heap_size);
    return 1;
  }

  *genv = make_env(root, &Nil, &Nil);
  define_constants(root, genv);
  define_primitives(root, genv);
  add_primitive(root, genv, "mem", prim_mem);
  add_primitive(root, genv, "gc", prim_gc);

  Obj *VERSION = make_int(root, 10204); // Represents the version 1.2.3
  add_constant(root, genv, "#version", &VERSION);

  // lisp_eval(root, genv, "(define a 5) (setq a 1) (print #itr) (print #t) (setq #itr 1)");
  // lisp_eval(root, genv, "(print #itr) (while (< #itr 10) (print #itr)) (print #itr)");
  // lisp_eval(root, genv, "(define code '(+ 1 2)) (eval '(+ 2 2)) (eval code) (print code) (+ 5 6)");
  // lisp_eval(root, genv, "(defun odd (n) (= 1 (% n 2))) (odd 1) (odd 2)");
  // lisp_eval(root, genv, "(list (list 1 2) (+ 2 3))");
  // lisp_eval(root, genv, "(/ 0 100)");
  // lisp_eval(root, genv, "(while (< #itr 15) (while (< #itr 10) (print #itr)))");

  // (defun a () (list (while (< #itr 10) (print #itr)) (+ 1 1)))
  // (defun a () (while (< #itr 10) (print #itr)) (+ 1 1))
  // (defun a (x) (print x) (print (+ x 1)) (list x x x))
  // ((lambda (l x) (while (< #itr x) (setq l (cdr l)) (print l))) (list 1 2 3 4 5) 3)

  // Enabled only once setup is complete: an allocation failure during setup would
  // unwind through error() before any setjmp() has been established, and setup itself
  // is not GC-safe (see `VERSION` below, an unrooted Obj* passed to add_constant).
  always_gc = env_flag("MINILISP_ALWAYS_GC");
#if MINILISP_GC_DEBUG
  debug_gc = env_flag("MINILISP_DEBUG_GC");
  fail_gc_alloc = env_flag("MINILISP_FAIL_GC_ALLOC");
  verify_gc = env_flag("MINILISP_VERIFY_GC");
#else
  if (env_flag("MINILISP_DEBUG_GC") || env_flag("MINILISP_FAIL_GC_ALLOC") ||
      env_flag("MINILISP_VERIFY_GC"))
    fprintf(stderr, "GC diagnostics ignored: rebuild with -D MINILISP_GC_DEBUG=1\n");
#endif

  // Read whole expressions rather than whole lines. The old loop took 200 characters at
  // a time and evaluated each one, so a definition spanning two lines could never parse
  // and a long line was silently truncated -- which is why the examples in this
  // repository could not be run by piping them in.
  //
  // Lines are accumulated until the parentheses balance, so a form may be as long and as
  // deeply nested as it likes, and typing one across several lines works interactively.
  char *form = NULL;
  size_t form_len = 0, form_cap = 0;
  int depth = 0;

  const bool interactive = line_edit_interactive();
  char history_path[512] = "";
  if (interactive)
  {
    const char *home = getenv("HOME");
    if (home)
      snprintf(history_path, sizeof(history_path), "%s/.unlisp_history", home);
    if (history_path[0])
      line_edit_load_history(history_path);
    printf("unlisp -- ^D to exit, ^C to abandon a line\n");
  }

  for (;;)
  {
    char *line = line_edit_read(depth > 0 ? "...     " : "unlisp> ");
    if (!line)
      break;
    if (interactive)
      line_edit_add_history(line);

    // The newline the editor stripped has to go back: it separates this line from the
    // next inside a form, and it terminates a trailing `;` comment.
    const size_t n = strlen(line);
    if (form_len + n + 2 > form_cap)
    {
      form_cap = (form_len + n + 2) * 2;
      char *grown = realloc(form, form_cap);
      if (!grown)
      {
        fprintf(stderr, "out of memory reading input\n");
        free(line);
        break;
      }
      form = grown;
    }
    memcpy(form + form_len, line, n);
    form_len += n;
    form[form_len++] = '\n';
    form[form_len] = '\0';

    // Track nesting so we know when the form is complete. A semicolon comments out the
    // rest of its line, so parentheses after one do not count.
    for (size_t i = 0; i < n; i++)
    {
      const char c = line[i];
      if (c == ';')
        break;
      else if (c == '(')
        depth++;
      else if (c == ')' && depth > 0)
        depth--;
    }

    // At a terminal, a blank line abandons a form left open by a typo -- otherwise the
    // prompt simply waits for a closing parenthesis that is never coming. Only when
    // interactive: a blank line inside a definition is ordinary in a file. ^C arrives
    // here as an empty line too, which is what makes it abandon the form.
    if (depth > 0 && interactive)
    {
      const char *p = line;
      while (*p == ' ' || *p == '\t' || *p == '\r')
        p++;
      if (*p == '\0')
      {
        fprintf(stderr, "abandoned %d unclosed form(s)\n", depth);
        depth = 0;
        form_len = 0;
        form[0] = '\0';
        free(line);
        continue;
      }
    }

    free(line);

    if (depth > 0)
      continue;  // still open, take another line

    interrupted = 0;
    lisp_eval(root, genv, form);
    form_len = 0;
    if (form)
      form[0] = '\0';
  }

  // Whatever is left over is unbalanced; evaluating it reports that rather than
  // discarding it silently.
  if (form_len > 0)
    lisp_eval(root, genv, form);

  if (interactive && history_path[0])
    line_edit_save_history(history_path);
  line_edit_free();
  free(form);

  lisp_destroy();

  return 0;
}
