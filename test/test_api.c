// What a host does through the C API, which the shell suite cannot reach: it drives the
// interpreter through the REPL. Here, a counter the host keeps as a constant and advances in
// place between evaluations, the way uniot-core advances a task's #t_pass. A script that
// saved the counter has to keep the value it saved.
#include <stdio.h>
#include <string.h>
#include "libminilisp.h"

static char last[256];
static void out(const char *s, int n) { snprintf(last, sizeof last, "%.*s", n, s); }

static int failures = 0;
static void check(bool ok, const char *what)
{
    printf("%s %s", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        printf(" (got %s)", last);
        failures++;
    }
    printf("\n");
}

static void *env_constructor[3];

int main(void)
{
    lisp_set_printers(out, out, out);
    void *root = env_constructor;
    env_constructor[0] = NULL;
    env_constructor[1] = NULL;
    env_constructor[2] = ROOT_END;
    Obj **genv = (Obj **)(env_constructor + 1);
    lisp_create(40000, 64 * 1024);
    *genv = make_env(root, &Nil, &Nil);
    define_constants(root, genv);
    define_primitives(root, genv);
    add_constant_int(root, genv, "#t_pass", 0);

    lisp_eval(root, genv, "(define saved ()) (define seen ())");
    for (int t = 9; t >= 0; t--) {
        get_variable(root, genv, "#t_pass")->cdr->value = t;
        if (t == 7)
            lisp_eval(root, genv, "(setq saved #t_pass)");
        lisp_eval(root, genv, "(setq seen (cons #t_pass seen))");
    }

    lisp_eval(root, genv, "saved");
    check(strcmp(last, "7") == 0, "a counter saved on one run keeps its value on later runs");
    lisp_eval(root, genv, "seen");
    check(strcmp(last, "(0 1 2 3 4 5 6 7 8 9)") == 0, "each run's value stays in a list");
    lisp_eval(root, genv, "#t_pass");
    check(strcmp(last, "0") == 0, "the counter itself reads the host's latest value");
    lisp_eval(root, genv, "(setq #t_pass 1)");
    check(strstr(last, "Cannot change constant") != NULL, "it is still a constant to scripts");

    lisp_destroy();
    return failures != 0;
}
