// This software is in the public domain.
#include "libminilisp.h"

int main(int argc, char **argv) {
    // Debug flags
    debug_gc = getEnvFlag("MINILISP_DEBUG_GC");
    always_gc = getEnvFlag("MINILISP_ALWAYS_GC");

    // Memory allocation
    memory = alloc_semispace();

    // Constants and primitives
    Symbols = Nil;
    void *root = NULL;
    DEFINE2(env, expr);
    *env = make_env(root, &Nil, &Nil);
    define_constants(root, env);
    define_primitives(root, env);

    // The main loop
    for (;;) {
        *expr = read_expr(root);
        if (!*expr)
            return 0;
        if (*expr == Cparen)
            error("Stray close parenthesis");
        if (*expr == Dot)
            error("Stray dot");
        print(eval(root, env, expr));
        printf("\n");
    }
}
