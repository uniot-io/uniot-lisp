/*
 * This is a part of the Uniot project. The following is the user apps interpreter.
 * Copyright (C) 2019-2020 Uniot <info.uniot@gmail.com>
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

#define ANSI_COLOR_RED "\x1b[31m"
#define ANSI_COLOR_GREEN "\x1b[32m"
#define ANSI_COLOR_RESET "\x1b[0m"

void *env_constructor[3];
void *root = NULL;
Obj **genv;

void printOut(const char *msg, int size)
{
  fprintf(stdout, ANSI_COLOR_GREEN "%s\n" ANSI_COLOR_RESET, msg);
}

void printErr(const char *msg, int size)
{
  fprintf(stderr, ANSI_COLOR_RED "%s\n" ANSI_COLOR_RESET, msg);
}

int main()
{
  lisp_set_printers(printOut, printErr);

  env_constructor[0] = root;
  env_constructor[1] = NULL;
  env_constructor[2] = ROOT_END;
  root = env_constructor;
  genv = (Obj **)(env_constructor + 1);

  lisp_create(40000);

  *genv = make_env(root, &Nil, &Nil);
  define_constants(root, genv);
  define_primitives(root, genv);

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

  char buf[200];
  char *str = NULL;
  while (NULL != (str = fgets(buf, sizeof(buf), stdin)))
  {
    str = strchr(str, '\n');
    if (str != NULL)
      *str = '\0';
    lisp_eval(root, genv, buf);
  };

  lisp_destroy();

  return 0;
}
