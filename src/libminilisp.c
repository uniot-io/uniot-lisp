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

#include <setjmp.h>
#include "libminilisp.h"
#include "memcheck.h"

// TODO: add comments ------------------------------------------------------
size_t MEMORY_SIZE = 4000; // default value

static bool cycle_in_progress = false;

static yield_def cycle_yield = NULL;
static print_def print_out = NULL;
static print_def print_err = NULL;

static jmp_buf error_jumper;

static const char *current_buffer = "";
static size_t current_index = 0;

static int buffer_getchar() {
    int r = (int)current_buffer[current_index++];
    if (r == '\0')
        r = EOF;
    return r;
}

static int buffer_ungetc(int c)
{
    if (current_index > 0)
        current_index--;
    return c;
}

static int printf_to_handler(char* dest, int pos, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int size = 0;

    if (dest) {
        size = pos + vsprintf(dest + pos, fmt, args);
    } else if (print_out) {
        char buf[LISP_MSG_BUF];
        size = vsprintf(buf, fmt, args);
        print_out(buf, size);
    }

    va_end(args);

    return size;
}

static void vsprintf_error_to_handler(const char *fmt, va_list ap)
{
    if (print_err) {
        char buf[LISP_MSG_BUF];
        int size = vsprintf(buf, fmt, ap);
        print_err(buf, size);
    }
}
// TODO: --------------------------------------------------------------------

void __attribute((noreturn)) error(const char *fmt, ...) {
    cycle_in_progress = false;

    va_list ap;
    va_start(ap, fmt);
    vsprintf_error_to_handler(fmt, ap);
    va_end(ap);
    longjmp(error_jumper, 1);
}

static Obj literals[] = {
    {TTRUE},
    {TNIL},
    {TDOT},
    {TCPAREN}};

// Constants
Obj *True = &literals[0];
Obj *Nil = &literals[1];
Obj *Dot = &literals[2];
Obj *Cparen = &literals[3];

// The list containing all symbols. Such data structure is traditionally called the "obarray", but I
// avoid using it as a variable name as this is not an array but a list.
static Obj *Symbols;

//======================================================================
// Memory management
//======================================================================

// The pointer pointing to the beginning of the current heap
static void *memory = NULL;

// The pointer pointing to the beginning of the old heap
static void *from_space = NULL;

// The number of bytes allocated from the heap
static size_t mem_nused = 0;

// Flags to debug GC
bool gc_running = false;
bool debug_gc = false;
bool always_gc = false;

// Currently we are using Cheney's copying GC algorithm, with which the available memory is split
// into two halves and all objects are moved from one half to another every time GC is invoked. That
// means the address of the object keeps changing. If you take the address of an object and keep it
// in a C variable, dereferencing it could cause SEGV because the address becomes invalid after GC
// runs.
//
// In order to deal with that, all access from C to Lisp objects will go through two levels of
// pointer dereferences. The C local variable is pointing to a pointer on the C stack, and the
// pointer is pointing to the Lisp object. GC is aware of the pointers in the stack and updates
// their contents with the objects' new addresses when GC happens.
//
// The following is a macro to reserve the area in the C stack for the pointers. The contents of
// this area are considered to be GC root.
//
// Be careful not to bypass the two levels of pointer indirections. If you create a direct pointer
// to an object, it'll cause a subtle bug. Such code would work in most cases but fails with SEGV if
// GC happens during the execution of the code. Any code that allocates memory may invoke GC.

// Round up the given value to a multiple of size. Size must be a power of 2. It adds size - 1
// first, then zero-ing the least significant bits to make the result a multiple of size. I know
// these bit operations may look a little bit tricky, but it's efficient and thus frequently used.
static inline size_t roundup(size_t var, size_t size) {
    return (var + size - 1) & ~(size - 1);
}

// Allocates memory block. This may start GC if we don't have enough memory.
static Obj *alloc(void *root, int type, size_t size) {
    // The object must be large enough to contain a pointer for the forwarding pointer. Make it
    // larger if it's smaller than that.
    size = roundup(size, sizeof(void *));

    // Add the size of the type tag and size fields.
    size += offsetof(Obj, value);

    // Round up the object size to the nearest alignment boundary, so that the next object will be
    // allocated at the proper alignment boundary. Currently we align the object at the same
    // boundary as the pointer.
    size = roundup(size, sizeof(void *));

    // If the debug flag is on, allocate a new memory space to force all the existing objects to
    // move to new addresses, to invalidate the old addresses. By doing this the GC behavior becomes
    // more predictable and repeatable. If there's a memory bug that the C variable has a direct
    // reference to a Lisp object, the pointer will become invalid by this GC call. Dereferencing
    // that will immediately cause SEGV.
    if (always_gc && !gc_running)
        gc(root);

    // Otherwise, run GC only when the available memory is not large enough.
    if (!always_gc && MEMORY_SIZE < mem_nused + size)
        gc(root);

    // Terminate the program if we couldn't satisfy the memory request. This can happen if the
    // requested size was too large or the from-space was filled with too many live objects.
    if (MEMORY_SIZE < mem_nused + size)
        error("Memory exhausted");

    // Allocate the object.
    Obj *obj = (Obj *)((char *)memory + mem_nused);
    obj->type = type;
    obj->size = size;
    obj->constant = false;
    mem_nused += size;
    return obj;
}

//======================================================================
// Garbage collector
//======================================================================
// Cheney's algorithm uses two pointers to keep track of GC status. At first both pointers point to
// the beginning of the to-space. As GC progresses, they are moved towards the end of the
// to-space. The objects before "scan1" are the objects that are fully copied. The objects between
// "scan1" and "scan2" have already been copied, but may contain pointers to the from-space. "scan2"
// points to the beginning of the free space.
static Obj *scan1;
static Obj *scan2;

// Moves one object from the from-space to the to-space. Returns the object's new address. If the
// object has already been moved, does nothing but just returns the new address.
static inline Obj *forward(Obj *obj) {
    // If the object's address is not in the from-space, the object is not managed by GC nor it
    // has already been moved to the to-space.
    ptrdiff_t offset = (uint8_t *)obj - (uint8_t *)from_space;
    if (offset < 0 || MEMORY_SIZE <= (size_t)offset)
        return obj;

    // The pointer is pointing to the from-space, but the object there was a tombstone. Follow the
    // forwarding pointer to find the new location of the object.
    if (obj->type == TMOVED)
        return (Obj *)obj->moved;

    // Otherwise, the object has not been moved yet. Move it.
    Obj *newloc = scan2;
    memcpy(newloc, obj, obj->size);
    scan2 = (Obj *)((uint8_t *)scan2 + obj->size);

    // Put a tombstone at the location where the object used to occupy, so that the following call
    // of forward() can find the object's new location.
    obj->type = TMOVED;
    obj->moved = newloc;
    return newloc;
}

static void *alloc_semispace() {
    return malloc(MEMORY_SIZE);
}

// Copies the root objects.
static void forward_root_objects(void *root) {
    Symbols = forward(Symbols);
    for (void **frame = (void **)root; frame; frame = *(void ***)frame)
        for (int i = 1; frame[i] != ROOT_END; i++)
            if (frame[i])
                frame[i] = forward((Obj *)frame[i]);
}

// Implements Cheney's copying garbage collection algorithm.
// http://en.wikipedia.org/wiki/Cheney%27s_algorithm
void gc(void *root) {
    assert(!gc_running);
    gc_running = true;

    // Allocate a new semi-space.
    from_space = memory;
    memory = alloc_semispace();

    // Initialize the two pointers for GC. Initially they point to the beginning of the to-space.
    scan1 = scan2 = (Obj *)memory;

    // Copy the GC root objects first. This moves the pointer scan2.
    forward_root_objects(root);

    // Copy the objects referenced by the GC root objects located between scan1 and scan2. Once it's
    // finished, all live objects (i.e. objects reachable from the root) will have been copied to
    // the to-space.
    while (scan1 < scan2) {
        switch (scan1->type) {
        case TINT:
        case TSYMBOL:
        case TPRIMITIVE:
            // Any of the above types does not contain a pointer to a GC-managed object.
            break;
        case TCELL:
            scan1->car = forward(scan1->car);
            scan1->cdr = forward(scan1->cdr);
            break;
        case TFUNCTION:
        case TMACRO:
            scan1->params = forward(scan1->params);
            scan1->body = forward(scan1->body);
            scan1->env = forward(scan1->env);
            break;
        case TENV:
            scan1->vars = forward(scan1->vars);
            scan1->up = forward(scan1->up);
            break;
        default:
            error("Bug: copy: unknown type %d", scan1->type);
        }
        scan1 = (Obj *)((uint8_t *)scan1 + scan1->size);
    }

    // Finish up GC.
    free(from_space);
    size_t old_nused = mem_nused;
    mem_nused = (size_t)((uint8_t *)scan1 - (uint8_t *)memory);
    if (debug_gc)
        printf_to_handler(NULL, 0, "GC: %zu bytes out of %zu bytes copied.\n", mem_nused, old_nused);
    gc_running = false;
}

//======================================================================
// Constructors
//======================================================================

Obj *make_int(void *root, int value) {
    Obj *r = alloc(root, TINT, sizeof(int));
    r->value = value;
    return r;
}

Obj *make_symbol(void *root, const char *name)
{
    Obj *sym = alloc(root, TSYMBOL, strlen(name) + 1);
    strcpy(sym->name, name);
    return sym;
}

static Obj *cons(void *root, Obj **car, Obj **cdr) {
    Obj *cell = alloc(root, TCELL, sizeof(Obj *) * 2);
    cell->car = *car;
    cell->cdr = *cdr;
    return cell;
}

static Obj *make_primitive(void *root, Primitive *fn) {
    Obj *r = alloc(root, TPRIMITIVE, sizeof(Primitive *));
    r->fn = fn;
    return r;
}

static Obj *make_function(void *root, Obj **env, int type, Obj **params, Obj **body) {
    assert(type == TFUNCTION || type == TMACRO);
    Obj *r = alloc(root, type, sizeof(Obj *) * 3);
    r->params = *params;
    r->body = *body;
    r->env = *env;
    return r;
}

struct Obj *make_env(void *root, Obj **vars, Obj **up) {
    Obj *r = alloc(root, TENV, sizeof(Obj *) * 2);
    r->vars = *vars;
    r->up = *up;
    return r;
}

// Returns ((x . y) . a)
static Obj *acons(void *root, Obj **x, Obj **y, Obj **a) {
    DEFINE1(cell);
    *cell = cons(root, x, y);
    return cons(root, cell, a);
}

//======================================================================
// Parser
//
// This is a hand-written recursive-descendent parser.
//======================================================================

static const char symbol_chars[] = "~!@#$%^&*-_=+:/?<>";

static int peek(void) {
    int c = buffer_getchar();
    buffer_ungetc(c);
    return c;
}

// Destructively reverses the given list.
static Obj *reverse(Obj *p) {
    Obj *ret = Nil;
    while (p != Nil) {
        Obj *head = p;
        p = p->cdr;
        head->cdr = ret;
        ret = head;
    }
    return ret;
}

// Skips the input until newline is found. Newline is one of \r, \r\n or \n.
static void skip_line(void) {
    for (;;) {
        int c = buffer_getchar();
        if (c == EOF || c == '\n')
            return;
        if (c == '\r') {
            if (peek() == '\n')
                buffer_getchar();
            return;
        }
    }
}

// Reads a list. Note that '(' has already been read.
static Obj *read_list(void *root) {
    DEFINE3(obj, head, last);
    *head = Nil;
    for (;;) {
        *obj = read_expr(root);
        if (!*obj)
            error("Unclosed parenthesis");
        if (*obj == Cparen)
            return reverse(*head);
        if (*obj == Dot) {
            *last = read_expr(root);
            if (read_expr(root) != Cparen)
                error("Closed parenthesis expected after dot");
            Obj *ret = reverse(*head);
            (*head)->cdr = *last;
            return ret;
        }
        *head = cons(root, obj, head);
    }
}

// May create a new symbol. If there's a symbol with the same name, it will not create a new symbol
// but return the existing one.
static Obj *intern(void *root, const char *name) {
    for (Obj *p = Symbols; p != Nil; p = p->cdr)
        if (strcmp(name, p->car->name) == 0)
            return p->car;
    DEFINE1(sym);
    *sym = make_symbol(root, name);
    Symbols = cons(root, sym, &Symbols);
    return *sym;
}

// Reader marcro ' (single quote). It reads an expression and returns (quote <expr>).
static Obj *read_quote(void *root) {
    DEFINE2(sym, tmp);
    *sym = intern(root, "quote");
    *tmp = read_expr(root);
    *tmp = cons(root, tmp, &Nil);
    *tmp = cons(root, sym, tmp);
    return *tmp;
}

static int read_number(int val) {
    while (isdigit(peek()))
        val = val * 10 + (buffer_getchar() - '0');
    return val;
}

static Obj *read_symbol(void *root, char c) {
    char buf[SYMBOL_MAX_LEN + 1];
    buf[0] = c;
    int len = 1;
    while (isalnum(peek()) || strchr(symbol_chars, peek())) {
        if (SYMBOL_MAX_LEN <= len)
            error("Symbol name too long");
        buf[len++] = buffer_getchar();
    }
    buf[len] = '\0';
    return intern(root, buf);
}

Obj *read_expr(void *root) {
    for (;;) {
        int c = buffer_getchar();
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t')
            continue;
        if (c == EOF)
            return NULL;
        if (c == ';') {
            skip_line();
            continue;
        }
        if (c == '(')
            return read_list(root);
        if (c == ')')
            return Cparen;
        if (c == '.')
            return Dot;
        if (c == '\'')
            return read_quote(root);
        if (isdigit(c))
            return make_int(root, read_number(c - '0'));
        if (c == '-' && isdigit(peek()))
            return make_int(root, -read_number(0));
        if (isalpha(c) || strchr(symbol_chars, c))
            return read_symbol(root, c);
        error("Don't know how to handle %c", c);
    }
}

// Prints the given object.
int print_to_buf(char *buf, int pos, Obj *obj) {
    switch (obj->type) {
    case TCELL:
        pos = printf_to_handler(buf, pos, "(");
        for (;;) {
            pos = print_to_buf(buf, pos, obj->car);
            if (obj->cdr == Nil)
                break;
            if (obj->cdr->type != TCELL) {
                pos = printf_to_handler(buf, pos, " . ");
                pos = print_to_buf(buf, pos, obj->cdr);
                break;
            }
            pos = printf_to_handler(buf, pos, " ");
            obj = obj->cdr;
        }
        pos = printf_to_handler(buf, pos, ")");
        return pos;

#define CASE(type, ...)                                     \
    case type:                                              \
        return printf_to_handler(buf, pos, __VA_ARGS__);
    CASE(TINT, "%d", obj->value);
    CASE(TSYMBOL, "%s", obj->name);
    CASE(TPRIMITIVE, "<primitive>");
    CASE(TFUNCTION, "<function>");
    CASE(TMACRO, "<macro>");
    CASE(TMOVED, "<moved>");
    CASE(TTRUE, "#t");
    CASE(TNIL, "()");
#undef CASE
    default:
        error("Bug: print: Unknown tag type: %d", obj->type);
    }
}

void print(Obj *obj) {
    print_to_buf(NULL, 0, obj);
}

// Returns the length of the given list. -1 if it's not a proper list.
int length(Obj *list) {
    int len = 0;
    for (; list->type == TCELL; list = list->cdr)
        len++;
    return list == Nil ? len : -1;
}

//======================================================================
// Evaluator
//======================================================================

static void add_variable(void *root, Obj **env, Obj **sym, Obj **val) {
    DEFINE2(vars, tmp);
    *vars = (*env)->vars;
    *tmp = acons(root, sym, val, vars);
    (*env)->vars = *tmp;
}

// Returns a newly created environment frame.
static Obj *push_env(void *root, Obj **env, Obj **vars, Obj **vals) {
    DEFINE3(map, sym, val);
    *map = Nil;
    for (; (*vars)->type == TCELL; *vars = (*vars)->cdr, *vals = (*vals)->cdr) {
        if ((*vals)->type != TCELL)
            error("Cannot apply function: number of argument does not match");
        *sym = (*vars)->car;
        *val = (*vals)->car;
        *map = acons(root, sym, val, map);
    }
    if (*vars != Nil)
        *map = acons(root, vars, vals, map);
    return make_env(root, map, env);
}

// Evaluates the list elements from head and returns the last return value.
static Obj *progn(void *root, Obj **env, Obj **list) {
    DEFINE2(lp, r);
    for (*lp = *list; *lp != Nil; *lp = (*lp)->cdr) {
        *r = (*lp)->car;
        *r = eval(root, env, r);
    }
    return *r;
}

// Evaluates all the list elements and returns their return values as a new list.
Obj *eval_list(void *root, Obj **env, Obj **list) {
    DEFINE4(head, lp, expr, result);
    *head = Nil;
    for (lp = list; *lp != Nil; *lp = (*lp)->cdr) {
        *expr = (*lp)->car;
        *result = eval(root, env, expr);
        *head = cons(root, result, head);
    }
    return reverse(*head);
}

static bool is_list(Obj *obj) {
    return obj == Nil || obj->type == TCELL;
}

static Obj *apply_func(void *root, Obj **env, Obj **fn, Obj **args) {
    DEFINE3(params, newenv, body);
    *params = (*fn)->params;
    *newenv = (*fn)->env;
    *newenv = push_env(root, newenv, params, args);
    *body = (*fn)->body;
    return progn(root, newenv, body);
}

// Apply fn with args.
static Obj *apply(void *root, Obj **env, Obj **fn, Obj **args) {
    if (!is_list(*args))
        error("argument must be a list");
    if ((*fn)->type == TPRIMITIVE)
        return (*fn)->fn(root, env, args);
    if ((*fn)->type == TFUNCTION) {
        DEFINE1(eargs);
        *eargs = eval_list(root, env, args);
        return apply_func(root, env, fn, eargs);
    }
    error("not supported");
}

// Searches for a variable by symbol. Returns null if not found.
static Obj *find(Obj **env, Obj *sym) {
    for (Obj *p = *env; p != Nil; p = p->up) {
        for (Obj *cell = p->vars; cell != Nil; cell = cell->cdr) {
            Obj *bind = cell->car;
            if (sym == bind->car)
                return bind;
        }
    }
    return NULL;
}

// Expands the given macro application form.
static Obj *macroexpand(void *root, Obj **env, Obj **obj) {
    if ((*obj)->type != TCELL || (*obj)->car->type != TSYMBOL)
        return *obj;
    DEFINE3(bind, macro, args);
    *bind = find(env, (*obj)->car);
    if (!*bind || (*bind)->cdr->type != TMACRO)
        return *obj;
    *macro = (*bind)->cdr;
    *args = (*obj)->cdr;
    return apply_func(root, env, macro, args);
}

// Evaluates the S expression.
Obj *eval(void *root, Obj **env, Obj **obj) {
    if (!is_valid_ptr(*obj))
        error("Unexpected statement. Evaluation terminated");

    switch ((*obj)->type) {
    case TINT:
    case TPRIMITIVE:
    case TFUNCTION:
    case TTRUE:
    case TNIL:
        // Self-evaluating objects
        return *obj;
    case TSYMBOL: {
        // Variable
        Obj *bind = find(env, *obj);
        if (!bind)
            error("Undefined symbol: %s", (*obj)->name);
        return bind->cdr;
    }
    case TCELL: {
        // Function application form
        DEFINE3(fn, expanded, args);
        *expanded = macroexpand(root, env, obj);
        if (*expanded != *obj)
            return eval(root, env, expanded);
        *fn = (*obj)->car;
        *fn = eval(root, env, fn);
        *args = (*obj)->cdr;
        if ((*fn)->type != TPRIMITIVE && (*fn)->type != TFUNCTION)
            error("The head of a list must be a function");
        return apply(root, env, fn, args);
    }
    default:
        error("Unexpected statement. Evaluation terminated. Bug: eval: Unknown tag type: %d", (*obj)->type);
    }
}

//======================================================================
// Primitive functions and special forms
//======================================================================

// 'expr
static Obj *prim_quote(void *root, Obj **env, Obj **list) {
    if (length(*list) != 1)
        error("Malformed quote");
    return (*list)->car;
}

// (cons expr expr)
static Obj *prim_cons(void *root, Obj **env, Obj **list) {
    if (length(*list) != 2)
        error("Malformed cons");
    Obj *cell = eval_list(root, env, list);
    cell->cdr = cell->cdr->car;
    return cell;
}

// (car <cell>)
static Obj *prim_car(void *root, Obj **env, Obj **list) {
    Obj *args = eval_list(root, env, list);
    if (length(args) < 1 || args->car->type != TCELL || args->cdr != Nil)
        error("Malformed car");
    return args->car->car;
}

// (cdr <cell>)
static Obj *prim_cdr(void *root, Obj **env, Obj **list) {
    Obj *args = eval_list(root, env, list);
    if (length(args) < 1 || args->car->type != TCELL || args->cdr != Nil)
        error("Malformed cdr");
    return args->car->cdr;
}

// (setq <symbol> expr)
static Obj *prim_setq(void *root, Obj **env, Obj **list) {
    if (length(*list) != 2 || (*list)->car->type != TSYMBOL)
        error("Malformed setq");
    DEFINE2(bind, value);
    *bind = find(env, (*list)->car);
    if (!*bind)
        error("Unbound variable %s", (*list)->car->name);
    if ((*list)->car->constant)
        error("Cannot change constant %s", (*list)->car->name);
    *value = (*list)->cdr->car;
    *value = eval(root, env, value);
    (*bind)->cdr = *value;
    return *value;
}

// (setcar <cell> expr)
static Obj *prim_setcar(void *root, Obj **env, Obj **list) {
    DEFINE1(args);
    *args = eval_list(root, env, list);
    if (length(*args) != 2 || (*args)->car->type != TCELL)
        error("Malformed setcar");
    (*args)->car->car = (*args)->cdr->car;
    return (*args)->car;
}

// (while cond expr ...)
static Obj *prim_while(void *root, Obj **env, Obj **list) {
    if (cycle_in_progress)
        error("Nested loops are prohibited");
    if (length(*list) < 2)
        error("Malformed while");
    cycle_in_progress = true;
    DEFINE3(cond, exprs, itr);
    *cond = (*list)->car;
    *itr = get_variable(root, env, "#itr");
    (*itr)->cdr->value = 0;
    while (eval(root, env, cond) != Nil) {
        *exprs = (*list)->cdr;
        eval_list(root, env, exprs);
        (*itr)->cdr->value++;
        // TODO: disallow endless loops

        if (cycle_yield)
            cycle_yield();
    }
    cycle_in_progress = false;
    return Nil;
}

// (gensym)
static Obj *prim_gensym(void *root, Obj **env, Obj **list) {
  static int count = 0;
  char buf[10];
  snprintf(buf, sizeof(buf), "G__%d", count++);
  return make_symbol(root, buf);
}

// (+ <integer> ...)
static Obj *prim_plus(void *root, Obj **env, Obj **list) {
    int sum = 0;
    for (Obj *args = eval_list(root, env, list); args != Nil; args = args->cdr) {
        if (args->car->type != TINT)
            error("+ takes only numbers");
        sum += args->car->value;
    }
    return make_int(root, sum);
}

// (- <integer> ...)
static Obj *prim_minus(void *root, Obj **env, Obj **list) {
    Obj *args = eval_list(root, env, list);
    for (Obj *p = args; p != Nil; p = p->cdr)
        if (p->car->type != TINT)
            error("- takes only numbers");
    if (args->cdr == Nil)
        return make_int(root, -args->car->value);
    int r = args->car->value;
    for (Obj *p = args->cdr; p != Nil; p = p->cdr)
        r -= p->car->value;
    return make_int(root, r);
}

// (% <integer> <integer>)
static Obj *prim_modulo(void *root, Obj **env, Obj **list) {
    Obj *args = eval_list(root, env, list);
    if (length(args) != 2)
        error("Malformed MODULO");
    Obj *x = args->car;
    Obj *y = args->cdr->car;
    if (x->type != TINT || y->type != TINT)
        error("MODULO takes only numbers");

    if (y->value == 0)
        error("Division by zero");

    return make_int(root, x->value % y->value);
}

// (/ <integer> <integer> ...)
static Obj *prim_div(void *root, Obj **env, Obj **list) {
    Obj *args = eval_list(root, env, list);

    if (length(args) < 2)
        error("Malformed /");

    for (Obj *p = args; p != Nil; p = p->cdr)
        if (p->car->type != TINT)
            error("/ takes only numbers");

    for (Obj *p = args->cdr; p != Nil; p = p->cdr)
        if (p->car->value == 0)
            error("Division by zero");

    if (args->car->value == 0)
        return make_int(root, 0);

    float r = args->car->value;
    for (Obj *p = args->cdr; p != Nil; p = p->cdr)
        r /= (float)p->car->value;

    return make_int(root, r);
}

static bool mul_with_overflow_check(int a, int b, int* res) {
    const int r = a * b;
    if (a != 0 && r / a != b)
        return false;

    *res = r;
    return true;
}

// (* <integer> <integer> ...)
static Obj *prim_mul(void *root, Obj **env, Obj **list) {
    Obj *args = eval_list(root, env, list);

    if (length(args) < 2)
        error("Malformed *");

    for (Obj *p = args; p != Nil; p = p->cdr)
        if (p->car->type != TINT)
            error("* takes only numbers");

    if (args->car->value == 0)
        return make_int(root, 0);

    int r = args->car->value;
    for (Obj *p = args->cdr; p != Nil; p = p->cdr)
    {
        const bool is_overflow = ! mul_with_overflow_check(r, p->car->value, &r);
        if (is_overflow)
            error("Multiplication overflow");
    }

    return make_int(root, r);
}

// (< <integer> <integer>)
static Obj *prim_lt(void *root, Obj **env, Obj **list) {
    Obj *args = eval_list(root, env, list);
    if (length(args) != 2)
        error("Malformed <");

    Obj *x = args->car;
    Obj *y = args->cdr->car;
    if (x->type != TINT || y->type != TINT)
        error("< takes only numbers");

    return x->value < y->value ? True : Nil;
}

// (<= <integer> <integer>)
static Obj *prim_lte(void *root, Obj **env, Obj **list) {
    Obj *args = eval_list(root, env, list);
    if (length(args) != 2)
        error("Malformed <=");

    Obj *x = args->car;
    Obj *y = args->cdr->car;
    if (x->type != TINT || y->type != TINT)
        error("<= takes only numbers");

    return x->value <= y->value ? True : Nil;
}

// (> <integer> <integer>)
static Obj *prim_gt(void *root, Obj **env, Obj **list) {
    Obj *args = eval_list(root, env, list);
    if (length(args) != 2)
        error("Malformed >");

    Obj *x = args->car;
    Obj *y = args->cdr->car;
    if (x->type != TINT || y->type != TINT)
        error("> takes only numbers");

    return x->value > y->value ? True : Nil;
}

// (>= <integer> <integer>)
static Obj *prim_gte(void *root, Obj **env, Obj **list) {
    Obj *args = eval_list(root, env, list);
    if (length(args) != 2)
        error("Malformed >=");

    Obj *x = args->car;
    Obj *y = args->cdr->car;
    if (x->type != TINT || y->type != TINT)
        error(">= takes only numbers");

    return x->value >= y->value ? True : Nil;
}

static Obj *handle_function(void *root, Obj **env, Obj **list, int type) {
    if ((*list)->type != TCELL || !is_list((*list)->car) || (*list)->cdr->type != TCELL)
        error("Malformed lambda");
    Obj *p = (*list)->car;
    for (; p->type == TCELL; p = p->cdr)
        if (p->car->type != TSYMBOL)
            error("Parameter must be a symbol");
    if (p != Nil && p->type != TSYMBOL)
        error("Parameter must be a symbol");
    DEFINE2(params, body);
    *params = (*list)->car;
    *body = (*list)->cdr;
    return make_function(root, env, type, params, body);
}

// (lambda (<symbol> ...) expr ...)
static Obj *prim_lambda(void *root, Obj **env, Obj **list) {
    return handle_function(root, env, list, TFUNCTION);
}

static Obj *handle_defun(void *root, Obj **env, Obj **list, int type) {
    if ((*list)->car->type != TSYMBOL || (*list)->cdr->type != TCELL)
        error("Malformed defun");
    DEFINE4(fn, sym, rest, bind);
    *sym = (*list)->car;
    *rest = (*list)->cdr;
    *bind = find(env, *sym);
    if (*bind)
        error("Already defined: %s", (*sym)->name);
    *fn = handle_function(root, env, rest, type);
    add_variable(root, env, sym, fn);
    return *fn;
}

// (defun <symbol> (<symbol> ...) expr ...)
static Obj *prim_defun(void *root, Obj **env, Obj **list) {
    return handle_defun(root, env, list, TFUNCTION);
}

// (define <symbol> expr)
static Obj *prim_define(void *root, Obj **env, Obj **list) {
    if (length(*list) != 2 || (*list)->car->type != TSYMBOL)
        error("Malformed define");
    DEFINE3(sym, value, bind);
    *sym = (*list)->car;
    *value = (*list)->cdr->car;
    *bind = find(env, *sym);
    if (*bind)
        error("Already defined: %s", (*sym)->name);
    *value = eval(root, env, value);
    add_variable(root, env, sym, value);
    return *value;
}

// (defmacro <symbol> (<symbol> ...) expr ...)
static Obj *prim_defmacro(void *root, Obj **env, Obj **list) {
    return handle_defun(root, env, list, TMACRO);
}

// (macroexpand expr)
static Obj *prim_macroexpand(void *root, Obj **env, Obj **list) {
    if (length(*list) != 1)
        error("Malformed macroexpand");
    DEFINE1(body);
    *body = (*list)->car;
    return macroexpand(root, env, body);
}

// (print expr)
static Obj *prim_print(void *root, Obj **env, Obj **list) {
    DEFINE1(tmp);
    if (length(*list) != 1)
        *tmp = eval_list(root, env, list);
    else
        *tmp = eval(root, env, &(*list)->car);

    char buf[SYMBOL_MAX_LEN];
    print_to_buf(buf, 0, *tmp);
    printf_to_handler(NULL, 0, buf);
    return Nil;
}

// (eval 'expr)
static Obj *prim_eval(void *root, Obj **env, Obj **list) {
    if (length(*list) != 1)
        error("Malformed eval");
    DEFINE2(quote, expr);
    *quote = (*list)->car;
    *expr = eval(root, env, quote);
    return eval(root, env, expr);
}

// (list expr ... expr)
static Obj *prim_list(void *root, Obj **env, Obj **list) {
    return eval_list(root, env, list);
}

// (if expr expr expr ...)
static Obj *prim_if(void *root, Obj **env, Obj **list) {
    if (length(*list) < 2)
        error("Malformed if");
    DEFINE3(cond, then, els);
    *cond = (*list)->car;
    *cond = eval(root, env, cond);
    if (*cond != Nil) {
        *then = (*list)->cdr->car;
        return eval(root, env, then);
    }
    *els = (*list)->cdr->cdr;
    return *els == Nil ? Nil : progn(root, env, els);
}

// (not expr)
static Obj *prim_not(void *root, Obj **env, Obj **list) {
    if (length(*list) != 1)
        error("Malformed not");

    Obj *arg = eval_list(root, env, list)->car;
    if (arg->type == TTRUE)
        return Nil;
    if (arg->type == TNIL)
        return True;
    if (arg->type != TINT)
        error("not takes only boolean and int values");

    const bool val = (bool)arg->value;
    return val ? Nil : True;
}

// (abs <integer>)
static Obj *prim_abs(void *root, Obj **env, Obj **list) {
    if (length(*list) != 1)
        error("Malformed abs");

    Obj *arg = eval_list(root, env, list)->car;
    if (arg->type != TINT)
        error("abs takes only numbers");

    const int ret = arg->value < 0 ? -arg->value : arg->value;
    return make_int(root, ret);
}

// (and expr expr ..)
static Obj *prim_and(void *root, Obj **env, Obj **list) {
    if (length(*list) < 2)
        error("Malformed and");

    for (Obj *args = eval_list(root, env, list); args != Nil; args = args->cdr) {
        if (args->car->type == TNIL)
            return Nil;
        if (args->car->type == TTRUE)
            continue;
        if (args->car->type != TINT)
            error("and takes only boolean and int values");
        if (! (bool)args->car->value)
            return Nil;
    }

    return True;
}

// (or expr expr ..)
static Obj *prim_or(void *root, Obj **env, Obj **list) {
    if (length(*list) < 2)
        error("Malformed or");

    bool current_res = false;
    for (Obj *args = eval_list(root, env, list); args != Nil; args = args->cdr) {
        if (args->car->type == TNIL)
            current_res = current_res || false;
        else if (args->car->type == TTRUE)
            current_res = current_res || true;
        else if (args->car->type != TINT)
            error("or takes only boolean and int values");

        current_res = current_res || (bool)args->car->value;
    }

    return current_res ? True : Nil;
}

// (= <integer> <integer>)
static Obj *prim_num_eq(void *root, Obj **env, Obj **list) {
    if (length(*list) != 2)
        error("Malformed =");
    Obj *values = eval_list(root, env, list);
    Obj *x = values->car;
    Obj *y = values->cdr->car;
    if (x->type != TINT || y->type != TINT)
        error("= takes only numbers");
    return x->value == y->value ? True : Nil;
}

// (eq expr expr)
static Obj *prim_eq(void *root, Obj **env, Obj **list) {
    if (length(*list) != 2)
        error("Malformed eq");
    Obj *values = eval_list(root, env, list);
    return values->car == values->cdr->car ? True : Nil;
}

void add_primitive(void *root, Obj **env, const char *name, Primitive *fn) {
    DEFINE2(sym, prim);
    *sym = intern(root, name);
    *prim = make_primitive(root, fn);
    add_variable(root, env, sym, prim);
}

void add_constant(void *root, Obj **env, const char *name, Obj **val) {
    DEFINE1(sym);
    *sym = intern(root, name);
    (*sym)->constant = true;
    add_variable(root, env, sym, val);
}

void add_constant_int(void *root, Obj **env, const char *name, int value) {
    DEFINE1(val);
    *val = make_int(root, value);
    add_constant(root, env, name, val);
}

Obj *get_variable(void *root, Obj **env, const char *name) {
    DEFINE2(sym, bind);
    *sym = intern(root, name);
    *bind = find(env, *sym);
    if (!*bind)
        error("Unbound variable %s", name);

    return *bind;
}

void define_constants(void *root, Obj **env) {
    add_constant(root, env, "#t", &True);
    add_constant_int(root, env, "#itr", 0);
    add_constant_int(root, env, "#version", LISP_VERSION);
}

void define_primitives(void *root, Obj **env) {
    add_primitive(root, env, "quote", prim_quote);
    add_primitive(root, env, "cons", prim_cons);
    add_primitive(root, env, "car", prim_car);
    add_primitive(root, env, "cdr", prim_cdr);
    add_primitive(root, env, "setq", prim_setq);
    add_primitive(root, env, "setcar", prim_setcar);
    add_primitive(root, env, "while", prim_while);
    add_primitive(root, env, "gensym", prim_gensym);
    add_primitive(root, env, "+", prim_plus);
    add_primitive(root, env, "-", prim_minus);
    add_primitive(root, env, "*", prim_mul);
    add_primitive(root, env, "/", prim_div);
    add_primitive(root, env, "%", prim_modulo);
    add_primitive(root, env, "<", prim_lt);
    add_primitive(root, env, "<=", prim_lte);
    add_primitive(root, env, ">", prim_gt);
    add_primitive(root, env, ">=", prim_gte);
    add_primitive(root, env, "define", prim_define);
    add_primitive(root, env, "defun", prim_defun);
    add_primitive(root, env, "defmacro", prim_defmacro);
    add_primitive(root, env, "macroexpand", prim_macroexpand);
    add_primitive(root, env, "lambda", prim_lambda);
    add_primitive(root, env, "if", prim_if);
    add_primitive(root, env, "=", prim_num_eq);
    add_primitive(root, env, "eq", prim_eq);
    add_primitive(root, env, "abs", prim_abs);
    add_primitive(root, env, "print", prim_print);
    // Implemented to reduce code.
    // Most of these functions can be implemented using previously declared functions.
    add_primitive(root, env, "eval", prim_eval);
    add_primitive(root, env, "list", prim_list);
    add_primitive(root, env, "not", prim_not);
    add_primitive(root, env, "and", prim_and);
    add_primitive(root, env, "or", prim_or);
}

//======================================================================
// Entry point
//======================================================================

void lisp_create(size_t size)
{
    if (memory == NULL)
    {
        set_origin_ptr(literals);
        MEMORY_SIZE = size;
        memory = alloc_semispace();
        Symbols = Nil;
    }
}

void lisp_destroy(void)
{
    if (memory != NULL)
    {
        free(memory);
        memory = NULL;
        from_space = NULL;
        gc_running = false;
        mem_nused = 0;
        current_index = 0;
    }
}

bool lisp_is_created()
{
    return memory != NULL;
}

bool lisp_eval(void *root, Obj **env, const char *code)
{
    current_buffer = code;
    current_index = 0;

    DEFINE1(expr);
    while (true)
    {
        if (setjmp(error_jumper) == 0)
        {
            *expr = read_expr(root);
            if (!*expr)
                return true;
            if (*expr == Cparen)
                error("Stray close parenthesis");
            if (*expr == Dot)
                error("Stray dot");

            char buf[SYMBOL_MAX_LEN];
            print_to_buf(buf, 0, eval(root, env, expr));
            printf_to_handler(NULL, 0, buf);
        }
        else
            return false;
    }
}

bool safe_eval(void *root, Obj **env, Obj **expr)
{
    if (setjmp(error_jumper) == 0)
    {
        char buf[SYMBOL_MAX_LEN];
        print_to_buf(buf, 0, eval(root, env, expr));
        printf_to_handler(NULL, 0, buf);
        return true;
    }
    return false;
}

void lisp_set_cycle_yield(yield_def yield)
{
    cycle_yield = yield;
}

void lisp_set_printers(print_def out, print_def err)
{
    print_out = out;
    print_err = err;
}

size_t lisp_mem_used(void) {
    return mem_nused;
}

int lisp_error_idx(void)
{
    return current_index;
}

// Used to simplify the work with the emulator. This has no other practical use!
Obj *handle_pruner(void *root, Obj **env, Obj **list, const char *handler_name, bool include_name)
{
    if ((*list)->car->type != TSYMBOL || (*list)->cdr->type != TCELL || !is_list((*list)->cdr->car))
        error("Malformed pruner");
    DEFINE4(fn, sym, rest, bind);
    *sym = (*list)->car;
    *rest = (*list)->cdr;
    *bind = find(env, *sym);
    if (*bind)
        error("Already defined: %s", (*sym)->name);

    Obj *p = (*rest)->car;
    for (; p->type == TCELL; p = p->cdr)
        if (p->car->type != TSYMBOL)
            error("Parameter must be a symbol");
    if (p != Nil && p->type != TSYMBOL)
        error("Parameter must be a symbol");

    {
        DEFINE3(handler, params, body);
        *handler = intern(root, handler_name);
        *params = (*rest)->car;
        *body = Nil;
        *body = cons(root, handler, body);
        if (include_name)
        {
            DEFINE2(tmp, quote);
            *tmp = *sym;
            *quote = intern(root, "quote");
            *tmp = cons(root, tmp, &Nil);
            *tmp = cons(root, quote, tmp);
            *body = cons(root, tmp, body);
        }
        Obj *s = (*rest)->car;
        for (; s->type == TCELL; s = s->cdr)
            *body = cons(root, &s->car, body);
        *body = reverse(*body);
        *body = cons(root, body, &Nil);

        *fn = make_function(root, env, TFUNCTION, params, body);
    }
    add_variable(root, env, sym, fn);
    return *fn;
}
