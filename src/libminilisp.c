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

// The interpreter's own invariant checks. A failed assert() on an ESP calls abort(),
// which panics and reboots the device before anything can be reported, so these are
// present only in a diagnostic build. A local macro rather than NDEBUG keeps that
// decision from silencing asserts in every other library in the link.
#if MINILISP_GC_DEBUG
#define LISP_ASSERT(cond) assert(cond)
#else
#define LISP_ASSERT(cond) ((void)0)
#endif

// TODO: add comments ------------------------------------------------------
size_t MEMORY_SIZE = 4000; // default value

// Iterations of the loop nest being run, summed over every loop in it however deeply
// nested. Nesting is what needs bounding, because an inner loop runs afresh on every
// pass of the outer one and the two multiply. Loops that merely follow one another only
// add, so each outermost loop starts with the count whole.
static long loops_done = 0;
static int loop_depth = 0;

static void loop_iterated(void) {
    if (++loops_done > MINILISP_MAX_LOOP_ITERATIONS)
        error("Loop ran for %d iterations without ending; possible endless loop",
              MINILISP_MAX_LOOP_ITERATIONS);
}

static yield_def cycle_yield = NULL;
static print_def print_out = NULL;
static print_def print_log = NULL;
static print_def print_err = NULL;

static jmp_buf error_jumper;

// True while a handler installed by lisp_eval() or safe_eval() is live. error() must
// not longjmp() when this is false: the jmp_buf would be uninitialised, which is
// undefined behaviour. It showed up as a segfault whenever setup exhausted a heap too
// small to hold the primitives table.
static bool error_jumper_valid = false;

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

// Appends formatted output at `pos` in `dest`, which has capacity `size`, and returns
// the new offset. Output that does not fit is truncated instead of overflowing: the
// unbounded vsprintf() this replaces smashed the caller's stack whenever a value
// printed longer than the buffer, which any list of more than a few elements does.
// Formatting a message needs somewhere to put it, and on this target the stack is the
// one resource that cannot be spared: a device has four kilobytes of it against forty of
// heap, and the deepest point of a recursive evaluation is exactly where a message gets
// formatted, because that is where the recursion guard raises. So these live in static
// storage instead.
//
// Two of them, not one: a value is formatted into the first and then handed to a handler,
// and a message formatted for a handler must not overwrite the value being reported. They
// are never in use at the same moment beyond that pairing.
//
// This assumes a print handler does not itself evaluate Lisp, and that it consumes what
// it is given before returning rather than keeping the pointer. Both already hold: the
// handlers copy the text out and none of them re-enter the interpreter.
static char s_value_buf[LISP_PRINT_MAX_LEN];
static char s_message_buf[LISP_MESSAGE_MAX_LEN];

// Hands an already formatted string to a handler. The obvious spelling of this,
// printf_to_handler(NULL, 0, 0, "%s", buf), copies the string through a second buffer for
// no reason, and that second buffer used to be 200 bytes of stack in the places where
// stack is scarcest.
static void write_out(const char *msg, int len)
{
    if (print_out)
        print_out(msg, len);
}

static void write_log(const char *msg, int len)
{
    if (print_log)
        print_log(msg, len);
}

static int printf_to_handler(char *dest, size_t size, int pos, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int written = 0;

    if (dest) {
        size_t remaining = (pos >= 0 && (size_t)pos < size) ? size - (size_t)pos : 0;
        bool truncated = false;
        int n = 0;
        if (remaining > 0) {
            n = LISP_VSNPRINTF(dest + pos, remaining, fmt, args);
            if (n < 0)
                n = 0;
            truncated = ((size_t)n >= remaining);
        } else {
            truncated = (size > 0);
        }
        written = pos + n;
        // Clamp so that the next call still writes inside the buffer.
        if (size > 0 && (size_t)written > size - 1)
            written = (int)size - 1;
        // Mark the cut so a shortened value cannot be mistaken for a complete one.
        // Applied on every truncating call, which is harmless because it is idempotent.
        if (truncated && size >= 4) {
            dest[size - 4] = '.';
            dest[size - 3] = '.';
            dest[size - 2] = '.';
            dest[size - 1] = '\0';
            written = (int)size - 1;
        }
    } else if (print_out) {
        int n = LISP_VSNPRINTF(s_message_buf, sizeof(s_message_buf), fmt, args);
        if (n < 0)
            n = 0;
        if ((size_t)n > sizeof(s_message_buf) - 1)
            n = (int)sizeof(s_message_buf) - 1;
        written = n;
        print_out(s_message_buf, written);
    }

    va_end(args);

    return written;
}

static void vsprintf_error_to_handler(const char *fmt, va_list ap)
{
    if (print_err) {
        // Reachable from user code, e.g. error("Undefined symbol: %s", name) with a
        // name approaching SYMBOL_MAX_LEN.
        int size = LISP_VSNPRINTF(s_message_buf, sizeof(s_message_buf), fmt, ap);
        if (size < 0)
            size = 0;
        if ((size_t)size > sizeof(s_message_buf) - 1)
            size = (int)sizeof(s_message_buf) - 1;
        print_err(s_message_buf, size);
    }
}
// TODO: --------------------------------------------------------------------

void __attribute((noreturn)) lisp_error(const char *fmt, ...) {
    // Nothing to do for the loop counters. The tail-call count is a local and goes with
    // the frame; the loop count and depth are cleared where the next evaluation begins,
    // which is reached by exactly this jump. Clearing them here as well would be a
    // second place to keep right, and the wrong one -- a nested evaluation that catches
    // its own error must not wipe the state of the loop that called it.

    // A longjmp out of gc() would otherwise leave gc_running set forever: the next
    // collection would trip assert(!gc_running), or, in a build with NDEBUG, would
    // silently re-enter a half-finished collection.
    gc_running = false;

    va_list ap;
    va_start(ap, fmt);
    vsprintf_error_to_handler(fmt, ap);
    va_end(ap);

    if (!error_jumper_valid)
        abort();  // nothing can catch this; the message above is all we can report

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

// Flags to debug GC
bool gc_running = false;
bool always_gc = false;

#if MINILISP_GC_DEBUG
// Logs a line for each collection.
bool debug_gc = false;

// Test hook. When set, alloc_semispace() reports failure without calling malloc,
// so the out-of-memory recovery path can be exercised deterministically.
bool fail_gc_alloc = false;

// When set, the heap is walked after every collection and any broken invariant stops
// the program. Combine with always_gc to check the whole heap after every allocation.
bool verify_gc = false;
#endif

// True once a heap has been created and not yet destroyed.
static bool s_heap_created = false;

#if MINILISP_GC_STATS
clock_def lisp_stats_clock = NULL;
LispStats lisp_stats;

void lisp_stats_reset(void) {
    const size_t held = lisp_stats.system_now;
    memset(&lisp_stats, 0, sizeof(lisp_stats));
    lisp_stats.system_now = held;
    lisp_stats.system_peak = held;
}

// Every byte the collector takes from or gives back to the system passes through these,
// so that the peak is the real high-water mark rather than a figure worked out from the
// heap size afterwards.
static void stats_system_add(size_t bytes) {
    lisp_stats.system_now += bytes;
    if (lisp_stats.system_now > lisp_stats.system_peak)
        lisp_stats.system_peak = lisp_stats.system_now;
}

static void stats_system_sub(size_t bytes) {
    lisp_stats.system_now -= bytes;
}

static uint64_t stats_now(void) {
    return lisp_stats_clock != NULL ? lisp_stats_clock() : 0;
}

static void stats_add_gc_time(uint64_t started) {
    if (lisp_stats_clock == NULL)
        return;
    const uint64_t elapsed = lisp_stats_clock() - started;
    lisp_stats.gc_time += elapsed;
    if (elapsed > lisp_stats.gc_time_max)
        lisp_stats.gc_time_max = elapsed;
}

#define STATS_SYSTEM_ADD(n) stats_system_add(n)
#define STATS_SYSTEM_SUB(n) stats_system_sub(n)
#else
#define STATS_SYSTEM_ADD(n) ((void)0)
#define STATS_SYSTEM_SUB(n) ((void)0)
#endif

//======================================================================
// Garbage collector: strategy interface
//======================================================================
//
// Everything outside the strategy section reaches the collector through these. To add
// a strategy, implement all six below the selector and leave the rest of the file
// untouched.
//
//   gc_heap_create()      allocate the heap sized by MEMORY_SIZE; false on failure
//   gc_heap_destroy()     release it and reset the strategy's own state
//   gc_alloc_raw(root, n) at least n bytes of object storage, collecting first if
//                         required, raising "Memory exhausted" if it cannot be
//                         satisfied. Sets the block's `size` field, because a collector
//                         may hand back a larger block than was asked for and `size` is
//                         what every walk of the heap steps by.
//   gc(root)              force a collection (declared in libminilisp.h)
//   gc_mem_used()         bytes currently held by live objects
//   gc_owns(p)            true if p addresses storage this collector manages
//   gc_verify(root)       walk the heap and stop on a broken invariant, present only
//                         when MINILISP_GC_DEBUG is set
//
static bool gc_heap_create(void);
static void gc_heap_destroy(void);
static Obj *gc_alloc_raw(void *root, size_t size);
static size_t gc_mem_used(void);
static bool gc_owns(const void *p);
#if MINILISP_GC_DEBUG
static void gc_verify(void *root);
#endif

// Access from C to Lisp objects goes through two levels of
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

    Obj *obj = gc_alloc_raw(root, size);
    obj->type = type;
    obj->constant = false;

#if MINILISP_GC_STATS
    lisp_stats.allocations++;
    lisp_stats.alloc_requested += size;
    lisp_stats.alloc_supplied += (size_t)obj->size;
    {
        const size_t live = gc_mem_used();
        if (live > lisp_stats.live_peak)
            lisp_stats.live_peak = live;
    }
#endif

    return obj;
}

//----------------------------------------------------------------------
// Heap verification: the parts every strategy needs
//----------------------------------------------------------------------
#if MINILISP_GC_DEBUG

// A heap that fails verification cannot be used, so this reports and stops rather than
// raising a catchable error.
static void gc_verify_fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsprintf_error_to_handler(fmt, ap);
    va_end(ap);
    abort();
}

// True for the tags a live object may carry. TFREE is deliberately excluded: a block
// marked free is not an object, and every caller wants to hear about it separately.
static bool gc_verify_type_known(unsigned char type) {
    return type >= TINT && type <= TCPAREN;
}
#endif  // MINILISP_GC_DEBUG

//======================================================================
// Garbage collector: the strategy itself
//======================================================================
//
// One of these implements the six functions described above. Which one is fixed when
// the library is compiled, so nothing outside pays for the choice at run time, and the
// selection is here rather than spread through the file.
#if MINILISP_GC == MINILISP_GC_COPYING
#include "gc_copying.inc"
#elif MINILISP_GC == MINILISP_GC_MARK_SWEEP
#include "gc_marksweep.inc"
#else
#error "Unknown MINILISP_GC strategy"
#endif

//======================================================================
// Constructors
//======================================================================

Obj *make_int(void *root, int32_t value) {
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
    LISP_ASSERT(type == TFUNCTION || type == TMACRO);
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

// The most stack one evaluation has consumed, in bytes.
size_t eval_stack_max = 0;

// Stack budget for one evaluation, taken from lisp_create(). Zero means no limit.
static size_t s_max_eval_stack = 0;

// Where the stack stood when the current evaluation began, set by lisp_eval() and
// safe_eval(). NULL when no evaluation is in progress, in which case nothing is
// measured and nothing is enforced.
static const char *s_stack_base = NULL;

// Stack consumed since the evaluation began. Which way the stack grows is not fixed by
// the C standard, so the distance is taken whichever way round it lies.
static inline size_t eval_stack_used(const char *here) {
    if (s_stack_base == NULL)
        return 0;
    return (s_stack_base > here) ? (size_t)(s_stack_base - here)
                                 : (size_t)(here - s_stack_base);
}

// Marks the start of an evaluation. Returns the previous base so a caller that finds
// itself nested inside another evaluation can put it back, rather than leaving the
// outer one measuring from a frame that has since returned.
static const char *eval_stack_begin(const char *here) {
    const char *previous = s_stack_base;
    s_stack_base = here;
    return previous;
}

// Raises if the frame this is called from has taken the evaluation past its stack
// budget.
//
// The reader and the printer both descend recursively through nested structure without
// going through eval(), so the guard in eval() never sees them. On an ESP8266 a level of
// reading costs 80 bytes against roughly 3200 available, which is about forty nested
// parentheses before the stack is gone -- during parsing, before there is an evaluation
// to guard. The budget is already known here, and lisp_eval() sets the base before it
// reads, so both can be held to the same limit as everything else.
static void check_stack_budget(const char *here, const char *what) {
    if (s_stack_base == NULL || s_max_eval_stack == 0)
        return;
    const size_t used = eval_stack_used(here);
    if (used > eval_stack_max)
        eval_stack_max = used;
    if (used > s_max_eval_stack)
        error("%s too deeply nested: used %u bytes of stack, limit is %u",
              what, (unsigned)used, (unsigned)s_max_eval_stack);
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
            // Nothing before the dot means there is no cell to give a tail to, and the
            // line below would write through Nil -- a shared constant, and not a cell.
            // It read as () before, which is not what anyone typing (. x) meant.
            if (*head == Nil)
                error("Stray dot");
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

// Reads the digits of an integer literal. Checked for the same reason the arithmetic
// is: this used to wrap, so a constant too large for the type became a different number
// with nothing said -- 99999999999999999999 read as 1661992959.
//
// The negative case is read as a positive magnitude and negated by the caller, so the
// limit here is INT_MAX and INT_MIN cannot be written as a literal. Accepting one more
// digit only where a minus sign preceded it is more machinery than the case deserves.
static int32_t read_number(int32_t val) {
    while (isdigit(peek())) {
        const int32_t digit = buffer_getchar() - '0';
        if (val > (LISP_INT_MAX - digit) / 10)
            error("Integer literal is too large");
        val = val * 10 + digit;
    }
    return val;
}

static Obj *read_symbol(void *root, char c) {
    // Static for the same reason as the formatting buffers: reading a nested expression
    // recurses, so this would otherwise be 200 bytes of stack per level of nesting in the
    // source being read. Nothing re-enters it -- it consumes characters and interns the
    // result, and interning never reads.
    static char buf[SYMBOL_MAX_LEN + 1];
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
    char stack_marker;
    check_stack_budget(&stack_marker, "Input");
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
int print_to_buf(char *buf, size_t size, int pos, Obj *obj) {
    char stack_marker;
    check_stack_budget(&stack_marker, "Value");

    switch (obj->type) {
    case TCELL:
        pos = printf_to_handler(buf, size, pos, "(");
        for (;;) {
            pos = print_to_buf(buf, size, pos, obj->car);
            if (obj->cdr == Nil)
                break;
            if (obj->cdr->type != TCELL) {
                pos = printf_to_handler(buf, size, pos, " . ");
                pos = print_to_buf(buf, size, pos, obj->cdr);
                break;
            }
            // Nothing more will fit, so there is nothing to gain by walking further --
            // and a list can be made to point at itself, with (setcdr x x), after which
            // this walk has no end. The output was truncated either way; stopping here
            // is what keeps a cycle from hanging the device. The recursion through `car`
            // above is bounded separately, by the stack budget.
            if (buf && size > 0 && (size_t)pos >= size - 1)
                break;
            pos = printf_to_handler(buf, size, pos, " ");
            obj = obj->cdr;
        }
        pos = printf_to_handler(buf, size, pos, ")");
        return pos;

#define CASE(type, ...)                                     \
    case type:                                              \
        return printf_to_handler(buf, size, pos, __VA_ARGS__);
    CASE(TINT, "%" LISP_INT_FMT, obj->value);
    CASE(TSYMBOL, "%s", obj->name);
    CASE(TPRIMITIVE, "<primitive>");
    CASE(TFUNCTION, "<function>");
    CASE(TMACRO, "<macro>");
    CASE(TMOVED, "<moved>");
    CASE(TFREE, "<free>");
    CASE(TTRUE, "#t");
    CASE(TNIL, "()");
    // Neither of these should reach a value a script can hold -- an environment is not
    // exposed, and the two reader sentinels are refused before evaluation -- but a
    // printer that raises is worse than one that prints something odd. This is the
    // function a user calls to find out what went wrong.
    CASE(TENV, "<env>");
    CASE(TDOT, ".");
    CASE(TCPAREN, ")");
#undef CASE
    default:
        // Every tag is named above, so this is a tag that does not exist: memory read as
        // an object, which is the collector's failure and not something to print past.
        error("Bug: print: Unknown tag type: %d", obj->type);
    }
}

void print(Obj *obj) {
    print_to_buf(NULL, 0, 0, obj);
}

// Returns the length of the given list. -1 if it's not a proper list.
//
// A list can be made to point at itself with (setcdr x x), and a walk of one has no end.
// No acyclic list can hold more cells than the heap does, so a count past that says the
// list is circular -- reported as improper, which is what a caller already handles.
int length(Obj *list) {
    const size_t limit = MEMORY_SIZE / sizeof(Obj);
    size_t len = 0;
    for (; list->type == TCELL; list = list->cdr)
        if (++len > limit)
            return -1;
    return list == Nil ? (int)len : -1;
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
            error("Cannot apply function: too few arguments");
        *sym = (*vars)->car;
        *val = (*vals)->car;
        *map = acons(root, sym, val, map);
    }

    // Whatever is left of the parameters is the symbol that takes the rest, if there is
    // one. Without it there is nothing left to put the surplus in, and passing it anyway
    // used to be accepted in silence -- while passing too few raised. Two ways of calling
    // a function wrongly should not differ in whether they are noticed.
    if (*vars != Nil)
        *map = acons(root, vars, vals, map);
    else if (*vals != Nil)
        error("Cannot apply function: too many arguments");

    return make_env(root, map, env);
}

// Evaluates the list elements from head and returns the last return value.
static Obj *progn(void *root, Obj **env, Obj **list) {
    // An empty sequence is (), not whatever the loop left behind. ADD_ROOT fills its
    // slots with NULL, so without this the loop running zero times returns a null
    // pointer into the evaluator -- which is why a function was not allowed an empty
    // body until this was here to make one safe.
    if (*list == Nil)
        return Nil;
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

// Hands a primitive a list of arguments that are already values. A primitive is given
// its arguments unevaluated and calls eval_list on them itself, which is right when it
// stands in a form a user wrote and wrong here, so each one is wrapped in a quote first.
// Two cells per argument, and the only way short of a second entry point into every
// primitive.
//
// `rest` walks the list from a root rather than from a local Obj *. cons() below can
// collect, and under the copying collector every object moves when it does, so a bare
// pointer held across it would be left pointing at where the cell used to be.
static Obj *apply_primitive(void *root, Obj **env, Obj **fn, Obj **args) {
    DEFINE4(rest, quoted, cell, qsym);
    // Interned once, not once per argument: it walks the symbol table, and the symbol it
    // returns is on the heap and moves like everything else.
    *qsym = intern(root, "quote");
    *quoted = Nil;
    *rest = *args;
    while (*rest != Nil) {
        *cell = (*rest)->car;
        *cell = cons(root, cell, &Nil);
        *cell = cons(root, qsym, cell);
        *quoted = cons(root, cell, quoted);
        *rest = (*rest)->cdr;
    }
    // Built back to front and reversed, which is destructive and allocates nothing,
    // rather than kept in order with a tail pointer -- one less root to hold.
    *quoted = reverse(*quoted);
    return (*fn)->fn(root, env, quoted);
}

static Obj *apply_func(void *root, Obj **env, Obj **fn, Obj **args) {
    DEFINE3(params, newenv, body);
    *params = (*fn)->params;
    *newenv = (*fn)->env;
    *newenv = push_env(root, newenv, params, args);
    *body = (*fn)->body;
    return progn(root, newenv, body);
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

// True if `p` addresses an object the collector manages, or one of the four
// statically allocated literals. Used to reject stale or corrupt pointers before
// they are dereferenced.
//
// This replaces an address-ordering heuristic that compared the pointer against the
// current malloc break. That test rejected perfectly valid objects as soon as a
// collection placed the heap above the break, and it called malloc()/free() on every
// single eval. Both bounds here are exact and cost two comparisons.
static inline bool is_valid_ptr(const void *p) {
    if (p == NULL)
        return false;

    const Obj *obj = (const Obj *)p;
    if (obj >= &literals[0] && obj < &literals[sizeof(literals) / sizeof(literals[0])])
        return true;

    return gc_owns(p);
}

// Evaluates every form of a body except the last, and returns the last one unevaluated,
// so that the caller can put it in tail position rather than evaluating it in a frame of
// its own. `list` must not be empty, and is advanced to its last cell.
//
// This is the shape a function body has and the shape a multi-form `else` has, and it was
// written out twice before. The reference implementation this borrows from keeps it as
// `progn` for the same reason.
static Obj *eval_but_last(void *root, Obj **env, Obj **list) {
    DEFINE1(form);
    for (; (*list)->cdr != Nil; *list = (*list)->cdr) {
        *form = (*list)->car;
        eval(root, env, form);
    }
    return (*list)->car;
}

// Chooses an `if` branch and returns it unevaluated, for the caller to evaluate in tail
// position. The environment is unchanged: a branch is evaluated where the `if` stood.
static Obj *if_branch(void *root, Obj **env, Obj **list) {
    if (length(*list) < 2)
        error("Malformed if");
    DEFINE2(cond, els);
    *cond = (*list)->car;
    *cond = eval(root, env, cond);
    if (*cond != Nil)
        return (*list)->cdr->car;
    *els = (*list)->cdr->cdr;
    if (*els == Nil)
        return Nil;
    return eval_but_last(root, env, els);
}

// Evaluates a single argument. Defined below, next to the predicates that share it.
static Obj *eval_one(void *root, Obj **env, Obj **list, const char *name);

// Registered as `if`, `progn`, `and` and `or`. eval() recognises these and takes their
// tail form over itself so that it lands in tail position; see the comment there.
static Obj *prim_if(void *root, Obj **env, Obj **list);
static Obj *prim_progn(void *root, Obj **env, Obj **list);
static Obj *prim_and(void *root, Obj **env, Obj **list);
static Obj *prim_or(void *root, Obj **env, Obj **list);

// Also recognised, for the same reason: the call it makes lands in tail position.
static Obj *prim_apply(void *root, Obj **env, Obj **list);

// Evaluates the operands of `and` or `or` until the answer is decided. Returns the
// operand left over for the caller to evaluate in tail position, or NULL when the answer
// was settled early, in which case it is left in *settled.
//
// Stopping early is the whole point: (and (consp x) (car x)) has to be safe to write, and
// it is not if both operands are evaluated. Only () is false here, as it is for `if`.
// Evaluates (apply f args) as far as the call itself. Returns the body left over for
// the caller to evaluate in *newenv, or NULL when there was no tail to hand over -- a
// primitive computes its own value -- in which case it is left in *settled.
//
// The call has to land in the caller's loop rather than be made here, or a loop written
// through `apply` would grow the stack where the same loop written out does not.
static Obj *apply_tail(void *root, Obj **env, Obj **list, Obj **newenv, Obj **settled) {
    if (length(*list) != 2)
        error("Malformed apply");
    DEFINE4(fn, args, params, body);
    *fn = (*list)->car;
    *args = (*list)->cdr->car;
    *fn = eval(root, env, fn);
    *args = eval(root, env, args);
    if (!is_list(*args))
        error("apply expects a list of arguments");

    if ((*fn)->type == TPRIMITIVE) {
        *settled = apply_primitive(root, env, fn, args);
        return NULL;
    }
    if ((*fn)->type != TFUNCTION)
        error("The head of a list must be a function");

    // The same as an ordinary call, with eval_list left out: these are values already.
    *params = (*fn)->params;
    *newenv = (*fn)->env;
    *newenv = push_env(root, newenv, params, args);
    *body = (*fn)->body;
    if (*body == Nil) {
        *settled = Nil;
        return NULL;
    }
    return eval_but_last(root, newenv, body);
}

static Obj *logic_tail(void *root, Obj **env, Obj **args, bool is_and, Obj **settled) {
    if (*args == Nil) {
        *settled = is_and ? True : Nil;
        return NULL;
    }
    DEFINE1(operand);
    while ((*args)->cdr != Nil) {
        *operand = (*args)->car;
        *operand = eval(root, env, operand);
        if (is_and ? (*operand == Nil) : (*operand != Nil)) {
            *settled = *operand;
            return NULL;
        }
        *args = (*args)->cdr;
    }
    return (*args)->car;
}

// Evaluates the S expression.
//
// Tail calls do not recurse. Where the value of a form is the value of another form --
// the last expression of a function body, the branch an `if` chose -- the loop below
// rewrites what it is evaluating and goes round again. A tail-recursive function
// therefore costs one frame however far it goes. Everything else still recurses.
Obj *eval(void *root, Obj **env, Obj **obj) {
    char stack_marker;
    const size_t used = eval_stack_used(&stack_marker);
    if (used > eval_stack_max)
        eval_stack_max = used;

    if (s_max_eval_stack > 0 && used > s_max_eval_stack) {
        // Raising needs stack of its own, which is why the budget must leave room for it.
        error("Recursion too deep: evaluation used %u bytes of stack, limit is %u",
              (unsigned)used, (unsigned)s_max_eval_stack);
    }

    if (!is_valid_ptr(*obj))
        error("Unexpected statement. Evaluation terminated");

    // Anything that is not a list is its own value or a variable lookup, and needs
    // neither roots nor a loop. This is most of what eval sees -- every argument, every
    // condition, every literal -- so it is worth taking before the root frame below is
    // built, which would otherwise be paid for on every one of them.
    if ((*obj)->type != TCELL) {
        Obj *value;
        switch ((*obj)->type) {
        case TINT:
        case TPRIMITIVE:
        case TFUNCTION:
        // A macro is a value like any other here. It is reachable through a form built
        // at run time -- (eval (cons m args)) -- where the head is the macro object
        // rather than a symbol, and expansion above only recognises the symbol. Without
        // this it fell to the default below and reported an internal bug, where what is
        // wanted is the ordinary complaint that the head of a list must be a function.
        case TMACRO:
        case TTRUE:
        case TNIL:
            value = *obj;
            break;
        case TSYMBOL: {
            Obj *bind = find(env, *obj);
            if (!bind)
                error("Undefined symbol: %s", (*obj)->name);
            value = bind->cdr;
            break;
        }
        default:
            error("Unexpected statement. Evaluation terminated. Bug: eval: Unknown tag type: %d",
                  (*obj)->type);
        }
        return value;
    }

    // Every root this function needs, declared once and before the loop. ADD_ROOT links
    // the frame it declares into the chain and reassigns `root`, so declaring any of
    // these inside the loop would link a frame to itself and the collector would walk the
    // chain forever.
    DEFINE7(cur, cenv, fn, args, params, newenv, body);
    *cur = *obj;
    *cenv = *env;

    // Tail calls made without leaving this frame. See MINILISP_MAX_TAIL_CALLS.
    long tail_calls = 0;

    // Each turn of the loop either produces the value, and returns it, or replaces what
    // is being evaluated with the form whose value this one is, and breaks out of the
    // switch to go round again. Those are the only two ways out.
    for (;;) {
        if (!is_valid_ptr(*cur))
            error("Unexpected statement. Evaluation terminated");

        switch ((*cur)->type) {
        case TINT:
        case TPRIMITIVE:
        case TFUNCTION:
        case TTRUE:
        case TNIL:
            // Self-evaluating objects
            return *cur;

        case TSYMBOL: {
            // Variable
            Obj *bind = find(cenv, *cur);
            if (!bind)
                error("Undefined symbol: %s", (*cur)->name);
            return bind->cdr;
        }

        case TCELL:
            // Function application form.
            *fn = (*cur)->car;
            if ((*fn)->type == TSYMBOL) {
                // One lookup answers both questions -- is the head a macro, and if not
                // what function is it. Asking macroexpand() first and then evaluating the
                // head walked the environment twice for the same symbol, which was about
                // a third of all the lookups this interpreter did.
                //
                // `bind` is not rooted because nothing allocates between finding it and
                // reading its value into the rooted slot below.
                Obj *bind = find(cenv, *fn);
                if (!bind)
                    error("Undefined symbol: %s", (*fn)->name);
                *fn = bind->cdr;
                if ((*fn)->type == TMACRO) {
                    *args = (*cur)->cdr;
                    // The expansion stands where the form stood, in the same tail
                    // position.
                    *cur = apply_func(root, cenv, fn, args);
                    break;
                }
            } else {
                // A head that is not a symbol has to be evaluated, as in ((lambda ...) x).
                *fn = eval(root, cenv, fn);
            }
            *args = (*cur)->cdr;
            if ((*fn)->type != TPRIMITIVE && (*fn)->type != TFUNCTION)
                error("The head of a list must be a function");
            if (!is_list(*args))
                error("argument must be a list");

            if ((*fn)->type == TPRIMITIVE) {
                // `if` is the one primitive whose value is the value of another form, so
                // its branch is taken over here rather than evaluated inside the
                // primitive, which would put a frame under a tail call.
                //
                // Recognised by the C function it carries rather than by the object
                // holding it: a function pointer is a fixed address, while the object
                // moves under a copying collector and would need a root of its own. It is
                // also the right test -- an alias of `if` is the same primitive and still
                // gets tail treatment, while a genuine rebinding correctly does not.
                if ((*fn)->fn == prim_if) {
                    *cur = if_branch(root, cenv, args);
                    break;
                }
                if ((*fn)->fn == prim_progn) {
                    if (*args == Nil)
                        return Nil;
                    *cur = eval_but_last(root, cenv, args);
                    break;
                }
                if ((*fn)->fn == prim_apply) {
                    Obj *settled = NULL;
                    Obj *tail = apply_tail(root, cenv, args, newenv, &settled);
                    if (!tail)
                        return settled;
                    *cur = tail;
                    *cenv = *newenv;
                    break;
                }
                if ((*fn)->fn == prim_and || (*fn)->fn == prim_or) {
                    Obj *settled = NULL;
                    Obj *tail = logic_tail(root, cenv, args, (*fn)->fn == prim_and, &settled);
                    if (!tail)
                        return settled;
                    *cur = tail;
                    break;
                }
                return (*fn)->fn(root, cenv, args);
            }

            // A function: bind the arguments, then go on with its body.
            *args = eval_list(root, cenv, args);
            *params = (*fn)->params;
            *newenv = (*fn)->env;
            *newenv = push_env(root, newenv, params, args);
            *body = (*fn)->body;
            if (*body == Nil)
                return Nil;
            *cur = eval_but_last(root, newenv, body);
            *cenv = *newenv;
            break;

        default:
            error("Unexpected statement. Evaluation terminated. Bug: eval: Unknown tag type: %d", (*cur)->type);
        }

        // Going round again is a loop, and an unbounded one would spin here rather than
        // run out of stack, which is how it used to end. The count lives in this frame,
        // so it measures one chain of tail calls that never returns -- not how much work
        // the program has done, which is not the interpreter's business to judge.
        if (++tail_calls > MINILISP_MAX_TAIL_CALLS)
            error("Tail call repeated %d times without returning; possible endless recursion",
                  MINILISP_MAX_TAIL_CALLS);

        // Fed on every pass, as `while` does. Throttling this to every 64th pass was
        // measured and recovered 0.2%, which is noise -- yielding is not what a tail call
        // costs. It is not free of consequence either: a pass here evaluates every
        // non-tail form of a function body, which is arbitrary work, so skipping 63 of
        // them could leave the watchdog unfed for as long as 63 bodies take.
        if (cycle_yield)
            cycle_yield();
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
    if (length(args) != 1)
        error("Malformed car");
    Obj *x = args->car;
    // (car ()) is () rather than an error, which is what walking a list relies on.
    if (x == Nil)
        return Nil;
    if (x->type != TCELL)
        error("car takes a list");
    return x->car;
}

// (cdr <cell>)
static Obj *prim_cdr(void *root, Obj **env, Obj **list) {
    Obj *args = eval_list(root, env, list);
    if (length(args) != 1)
        error("Malformed cdr");
    Obj *x = args->car;
    // (cdr ()) is () rather than an error, which is what walking a list relies on.
    if (x == Nil)
        return Nil;
    if (x->type != TCELL)
        error("cdr takes a list");
    return x->cdr;
}

// (setq <symbol> expr)
static Obj *prim_setq(void *root, Obj **env, Obj **list) {
    if (length(*list) != 2 || (*list)->car->type != TSYMBOL)
        error("Malformed setq");
    DEFINE2(bind, value);
    *bind = find(env, (*list)->car);
    if (!*bind)
        error("Undefined symbol: %s", (*list)->car->name);
    if ((*list)->car->constant)
        error("Cannot change constant %s", (*list)->car->name);
    *value = (*list)->cdr->car;
    *value = eval(root, env, value);
    (*bind)->cdr = *value;
    return *value;
}

// A quoted literal is the very structure the reader built, and these change it in
// place. (defun f () '(1 2 3)) followed by (setcar (f) 99) leaves f returning (99 2 3)
// for good -- the function's own source has been edited. That is how this family
// behaves and it is not a bug, but nothing about the spelling warns you, so: copy
// before mutating anything you did not cons yourself.
//
// (setcar <cell> expr)
static Obj *prim_setcar(void *root, Obj **env, Obj **list) {
    DEFINE1(args);
    *args = eval_list(root, env, list);
    if (length(*args) != 2 || (*args)->car->type != TCELL)
        error("Malformed setcar");
    (*args)->car->car = (*args)->cdr->car;
    return (*args)->car;
}

static Obj *prim_setcdr(void *root, Obj **env, Obj **list) {
    DEFINE1(args);
    *args = eval_list(root, env, list);
    if (length(*args) != 2 || (*args)->car->type != TCELL)
        error("Malformed setcdr");
    (*args)->car->cdr = (*args)->cdr->car;
    return (*args)->car;
}

// (while cond expr ...)
static Obj *prim_while(void *root, Obj **env, Obj **list) {
    if (length(*list) < 2)
        error("Malformed while");
    DEFINE3(cond, exprs, itr);
    *cond = (*list)->car;
    *itr = get_variable(root, env, "#itr");

    // Loops nest, so the counter is saved and put back: #itr is the innermost loop's
    // iteration. Bind it to a variable first if an inner loop needs the outer one's.
    // Nesting used to be refused outright because a single global counter would be
    // clobbered, and it was also what enforced the iteration limit.
    const int outer_itr = (*itr)->cdr->value;
    (*itr)->cdr->value = 0;

    // Only a nest shares a budget. Charging one loop for what the loop before it spent
    // would refuse (progn (while ...) (while ...)) for doing twice the work of something
    // allowed on its own, which is the same mistake as counting work in the first place.
    if (loop_depth == 0)
        loops_done = 0;
    loop_depth++;

    while (eval(root, env, cond) != Nil) {
        *exprs = (*list)->cdr;
        eval_list(root, env, exprs);
        (*itr)->cdr->value++;

        loop_iterated();

        if (cycle_yield)
            cycle_yield();
    }

    loop_depth--;
    (*itr)->cdr->value = outer_itr;
    return Nil;
}

// (gensym)
static Obj *prim_gensym(void *root, Obj **env, Obj **list) {
  if (*list != Nil)
      error("Malformed gensym");
  static int count = 0;
  char buf[10];
  snprintf(buf, sizeof(buf), "G__%d", count++);
  return make_symbol(root, buf);
}

// Arithmetic that refuses to be quietly wrong.
//
// These used to overflow and carry on, which on a device means a sensor total or a
// timestamp silently becoming a different number. The check that did exist could not
// work: it multiplied first and divided back to see whether it had overflowed, and
// signed overflow is undefined, so the compiler was free to assume it had not happened
// and delete the test -- which it did at every optimisation level above -O0, meaning
// the check was present in no build that ever shipped.
//
// The builtins do the arithmetic and report the overflow without ever producing the
// undefined result. Available on all three toolchains this is built with.
#define ARITH_OP(name, builtin, what)                            \
    static int32_t name(int32_t a, int32_t b) {                  \
        int32_t r;                                               \
        if (builtin(a, b, &r))                                   \
            error("Integer overflow in " what);                  \
        return r;                                                \
    }

ARITH_OP(add_checked, __builtin_add_overflow, "+")
ARITH_OP(sub_checked, __builtin_sub_overflow, "-")
ARITH_OP(mul_checked, __builtin_mul_overflow, "*")

// (+ <integer> ...)
static Obj *prim_plus(void *root, Obj **env, Obj **list) {
    int32_t sum = 0;
    for (Obj *args = eval_list(root, env, list); args != Nil; args = args->cdr) {
        if (args->car->type != TINT)
            error("+ takes only numbers");
        sum = add_checked(sum, args->car->value);
    }
    return make_int(root, sum);
}

// (- <integer> ...)
static Obj *prim_minus(void *root, Obj **env, Obj **list) {
    Obj *args = eval_list(root, env, list);
    // Without this, args is Nil, whose union is zero filled, so args->cdr reads NULL
    // rather than Nil, the single-argument branch is skipped and args->car is
    // dereferenced through a null pointer.
    if (args == Nil)
        error("Malformed -");
    for (Obj *p = args; p != Nil; p = p->cdr)
        if (p->car->type != TINT)
            error("- takes only numbers");
    if (args->cdr == Nil)
        return make_int(root, sub_checked(0, args->car->value));
    int32_t r = args->car->value;
    for (Obj *p = args->cdr; p != Nil; p = p->cdr)
        r = sub_checked(r, p->car->value);
    return make_int(root, r);
}

// (% <integer> <integer>)
static Obj *prim_modulo(void *root, Obj **env, Obj **list) {
    Obj *args = eval_list(root, env, list);
    if (length(args) != 2)
        error("Malformed %%");
    Obj *x = args->car;
    Obj *y = args->cdr->car;
    if (x->type != TINT || y->type != TINT)
        error("%% takes only numbers");

    if (y->value == 0)
        error("Division by zero");
    // The remainder of the one division that overflows. Mathematically zero, but the
    // instruction traps on some targets, so it is answered rather than computed.
    if (x->value == LISP_INT_MIN && y->value == -1)
        return make_int(root, 0);

    return make_int(root, x->value % y->value);
}

// (/ <integer> <integer> ...)
static Obj *prim_div(void *root, Obj **env, Obj **list) {
    Obj *args = eval_list(root, env, list);

    if (args == Nil)
        error("Malformed /");

    for (Obj *p = args; p != Nil; p = p->cdr)
        if (p->car->type != TINT)
            error("/ takes only numbers");

    // (/ x) is x. A reciprocal would be meaningless with integers only.
    if (args->cdr == Nil)
        return args->car;

    for (Obj *p = args->cdr; p != Nil; p = p->cdr)
        if (p->car->value == 0)
            error("Division by zero");

    if (args->car->value == 0)
        return make_int(root, 0);

    // In integers. This used to divide in a float, whose 24 bit mantissa cannot hold a
    // 32 bit integer, so (/ 100000000 3) came out as 33333334 -- and on a chip without
    // a floating point unit it was software arithmetic as well. Truncates towards zero,
    // as `%` does, so (/ x y) and (% x y) agree about which way the quotient went.
    int32_t r = args->car->value;
    for (Obj *p = args->cdr; p != Nil; p = p->cdr) {
        // The one division that overflows: INT_MIN / -1 has no positive counterpart.
        if (r == LISP_INT_MIN && p->car->value == -1)
            error("Integer overflow in /");
        r /= p->car->value;
    }

    return make_int(root, r);
}


// (* <integer> <integer> ...)
static Obj *prim_mul(void *root, Obj **env, Obj **list) {
    Obj *args = eval_list(root, env, list);

    // The identity element, as everywhere else in this family. `+` already did this.
    if (args == Nil)
        return make_int(root, 1);

    for (Obj *p = args; p != Nil; p = p->cdr)
        if (p->car->type != TINT)
            error("* takes only numbers");

    if (args->cdr == Nil)
        return args->car;

    if (args->car->value == 0)
        return make_int(root, 0);

    int32_t r = args->car->value;
    for (Obj *p = args->cdr; p != Nil; p = p->cdr)
        r = mul_checked(r, p->car->value);

    return make_int(root, r);
}

// (< <integer> <integer>)
static Obj *prim_lt(void *root, Obj **env, Obj **list) {
    Obj *args = eval_list(root, env, list);
    if (length(args) < 2)
        error("Malformed <");

    for (Obj *p = args; p != Nil; p = p->cdr)
        if (p->car->type != TINT)
            error("< takes only numbers");

    // Chained: (< 1 2 3) asks whether every adjacent pair holds.
    for (Obj *p = args; p->cdr != Nil; p = p->cdr)
        if (!(p->car->value < p->cdr->car->value))
            return Nil;
    return True;
}

// (<= <integer> <integer>)
static Obj *prim_lte(void *root, Obj **env, Obj **list) {
    Obj *args = eval_list(root, env, list);
    if (length(args) < 2)
        error("Malformed <=");

    for (Obj *p = args; p != Nil; p = p->cdr)
        if (p->car->type != TINT)
            error("<= takes only numbers");

    // Chained: (<= 1 2 3) asks whether every adjacent pair holds.
    for (Obj *p = args; p->cdr != Nil; p = p->cdr)
        if (!(p->car->value <= p->cdr->car->value))
            return Nil;
    return True;
}

// (> <integer> <integer>)
static Obj *prim_gt(void *root, Obj **env, Obj **list) {
    Obj *args = eval_list(root, env, list);
    if (length(args) < 2)
        error("Malformed >");

    for (Obj *p = args; p != Nil; p = p->cdr)
        if (p->car->type != TINT)
            error("> takes only numbers");

    // Chained: (> 1 2 3) asks whether every adjacent pair holds.
    for (Obj *p = args; p->cdr != Nil; p = p->cdr)
        if (!(p->car->value > p->cdr->car->value))
            return Nil;
    return True;
}

// (>= <integer> <integer>)
static Obj *prim_gte(void *root, Obj **env, Obj **list) {
    Obj *args = eval_list(root, env, list);
    if (length(args) < 2)
        error("Malformed >=");

    for (Obj *p = args; p != Nil; p = p->cdr)
        if (p->car->type != TINT)
            error(">= takes only numbers");

    // Chained: (>= 1 2 3) asks whether every adjacent pair holds.
    for (Obj *p = args; p->cdr != Nil; p = p->cdr)
        if (!(p->car->value >= p->cdr->car->value))
            return Nil;
    return True;
}

static Obj *handle_function(void *root, Obj **env, Obj **list, int type) {
    // The parameters are a list, possibly ending in a symbol that takes the rest, or a
    // bare symbol that takes all of them. The last is the same mechanism as the second
    // with nothing required first, and push_env already binds it: a symbol is not a cell,
    // so it falls past the loop to the line that binds whatever is left over.
    // The body may be empty -- (lambda ()) is a function of no arguments returning (),
    // as it is in the Lisp this imitates. The three places that evaluate a body already
    // answer () for an empty one; this is what lets them.
    if ((*list)->type != TCELL || !is_list((*list)->cdr) ||
        (!is_list((*list)->car) && (*list)->car->type != TSYMBOL))
        error("Malformed lambda");
    // Names must be distinct. Binding the same one twice is a typo every time -- the
    // second binding simply hides the first, which is deterministic but only if you know
    // that a later parameter is consed on later and so is found first. The standards this
    // borrows from call it an error, and it is the last thing here that was wrong quietly.
    // Quadratic, over a list short enough that it does not matter, and only when a
    // function is defined.
    Obj *p = (*list)->car;
    for (; p->type == TCELL; p = p->cdr) {
        if (p->car->type != TSYMBOL)
            error("Parameter must be a symbol");
        for (Obj *q = (*list)->car; q != p; q = q->cdr)
            if (q->car == p->car)
                error("Duplicate parameter: %s", p->car->name);
    }
    if (p != Nil) {
        if (p->type != TSYMBOL)
            error("Parameter must be a symbol");
        // The rest parameter counts too: (a . a) hid the fixed one behind it.
        for (Obj *q = (*list)->car; q->type == TCELL; q = q->cdr)
            if (q->car == p)
                error("Duplicate parameter: %s", p->name);
    }
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
    // (*list) is Nil when there are no arguments at all, and reading its car walks off a
    // literal whose union is zero filled.
    if ((*list)->type != TCELL || (*list)->car->type != TSYMBOL || (*list)->cdr->type != TCELL)
        error("Malformed %s", type == TMACRO ? "defmacro" : "defun");
    DEFINE3(fn, sym, rest);
    *sym = (*list)->car;
    *rest = (*list)->cdr;
    *fn = handle_function(root, env, rest, type);

    // Redefinition rebinds, as it does in every Lisp this borrows its names from. See
    // prim_define for why it must not shadow instead.
    Obj *bind = find(env, *sym);
    if (bind)
        bind->cdr = *fn;
    else
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
    DEFINE2(sym, value);
    *sym = (*list)->car;
    *value = (*list)->cdr->car;
    *value = eval(root, env, value);

    // Looked up after evaluating, so nothing moves under the pointer. Rebinding replaces
    // the value in the pair that is already there, which leaves the old value collectable;
    // adding a second pair would shadow it and keep it reachable from a root for ever.
    Obj *bind = find(env, *sym);
    if (bind)
        bind->cdr = *value;
    else
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
    // Evaluated, because macroexpand is a function rather than a special form: the
    // standard spelling is (macroexpand '(m 5)). Taking the form unevaluated made that
    // spelling return the quote form untouched.
    *body = eval_list(root, env, list)->car;
    return macroexpand(root, env, body);
}

// (print expr)
static Obj *prim_print(void *root, Obj **env, Obj **list) {
    DEFINE1(tmp);
    if (length(*list) != 1)
        *tmp = eval_list(root, env, list);
    else
        *tmp = eval(root, env, &(*list)->car);

    // The log sink only. What a script deliberately prints and what its forms happen to
    // evaluate to are two different things, and the host keeps them apart -- on a device
    // they are separate channels. Writing to both here collapsed that distinction and
    // put every printed line into the value stream as well.
    const int len = print_to_buf(s_value_buf, sizeof(s_value_buf), 0, *tmp);
    write_log(s_value_buf, len);

    // The object printed, as Emacs Lisp returns, so print can sit inside a larger form
    // without swallowing its argument. A top-level (print 1) does appear twice -- once
    // from here and once as the value of the form -- but those are two different sinks
    // on a device, script output and the value echo, and both are meant to show it.
    return *tmp;
}

// (apply fn args) -- calls fn with the elements of the list `args` as its arguments.
//
// The awkward half is a primitive. A primitive is handed its arguments unevaluated and
// calls eval_list on them itself, which is right when it stands in a form a user wrote
// and wrong here, where they are already values: (apply car (list (list 1 2))) would
// evaluate (1 2) and try to call 1. So each one is wrapped in a quote on the way in.
// That costs two cells per argument, and it is the only way short of a second entry
// point into every primitive.
//
// A function needs none of that. apply_func binds the values it is given and evaluates
// the body, which is what eval does for a call once eval_list has run -- so this is the
// same path with the evaluation left out, and it allocates nothing but the frame.
static Obj *prim_apply(void *root, Obj **env, Obj **list) {
    if (length(*list) != 2)
        error("Malformed apply");
    DEFINE2(fn, args);
    *fn = (*list)->car;
    *fn = eval(root, env, fn);
    *args = (*list)->cdr->car;
    *args = eval(root, env, args);
    if (!is_list(*args))
        error("apply expects a list of arguments");

    // A function needs no quoting. apply_func binds the values it is given and
    // evaluates the body, which is what eval does for a call once eval_list has run --
    // so this is the same path with the evaluation left out.
    if ((*fn)->type == TFUNCTION)
        return apply_func(root, env, fn, args);
    if ((*fn)->type != TPRIMITIVE)
        error("The head of a list must be a function");
    return apply_primitive(root, env, fn, args);
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
// Sequencing. Blockly had been wrapping multi-statement branches in (list ...) for want
// of this, which allocated a cons per statement and threw the list away.
//
// eval() intercepts this too, so the last form lands in tail position; this body only
// runs if `progn` is applied by some other path.
static Obj *prim_progn(void *root, Obj **env, Obj **list) {
    if (*list == Nil)
        return Nil;
    DEFINE1(body);
    *body = *list;
    *body = eval_but_last(root, env, body);
    return eval(root, env, body);
}

// eval() intercepts this and evaluates the branch in its own loop, so this body only
// runs if `if` is ever applied by some other path. It gives the same answer, without the
// tail benefit.
static Obj *prim_if(void *root, Obj **env, Obj **list) {
    DEFINE1(branch);
    *branch = if_branch(root, env, list);
    return eval(root, env, branch);
}

// (not expr)
// Only () is false, which is the rule `if` uses. It previously also treated 0 as false,
// so (if x ...) and (if (not (not x)) ...) disagreed when x was 0. Comparing a flag is
// what `=` is for: (= x #t) is unchanged.
static Obj *prim_not(void *root, Obj **env, Obj **list) {
    return eval_one(root, env, list, "not") == Nil ? True : Nil;
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
// eval() intercepts these two so that the last operand lands in tail position; these
// bodies only run if they are applied by some other path.
//
// Both return the operand that decided the answer, not a boolean: (and 1 2) is 2 and
// (or () 5) is 5, as in Emacs Lisp and Common Lisp. (and) is true and (or) is false.
static Obj *prim_and(void *root, Obj **env, Obj **list) {
    DEFINE1(args);
    *args = *list;
    Obj *settled = NULL;
    Obj *tail = logic_tail(root, env, args, true, &settled);
    if (!tail)
        return settled;
    *args = tail;
    return eval(root, env, args);
}

static Obj *prim_or(void *root, Obj **env, Obj **list) {
    DEFINE1(args);
    *args = *list;
    Obj *settled = NULL;
    Obj *tail = logic_tail(root, env, args, false, &settled);
    if (!tail)
        return settled;
    *args = tail;
    return eval(root, env, args);
}

// (= <integer|boolean> <integer|boolean>)
// One pair, with the deliberate coercion between numbers and booleans that lets a flag
// be compared against #t. It is not transitive -- 5 = #t and 1 = #t, but 5 is not 1 --
// which is why `eql` exists alongside it as the real equality.
static bool num_eq_pair(Obj *x, Obj *y) {
    if ((x->type != TINT && x->type != TTRUE && x->type != TNIL) ||
        (y->type != TINT && y->type != TTRUE && y->type != TNIL))
        error("= takes only numbers and booleans");

    if (x->type == TINT && y->type == TINT)
        return x->value == y->value;

    const int xb = x->type == TINT ? (x->value != 0) : (x->type == TTRUE);
    const int yb = y->type == TINT ? (y->value != 0) : (y->type == TTRUE);
    return xb == yb;
}

static Obj *prim_num_eq(void *root, Obj **env, Obj **list) {
    Obj *args = eval_list(root, env, list);
    if (length(args) < 2)
        error("Malformed =");

    // Chained, like the ordering comparisons.
    for (Obj *p = args; p->cdr != Nil; p = p->cdr)
        if (!num_eq_pair(p->car, p->cdr->car))
            return Nil;
    return True;
}

// TODO: implement null? function for checking if the object is Nil

// (eq expr expr)
// Named `eql` rather than `eq` because that is what it does. In this family of Lisps
// `eq` is identity, and identity of numbers is meaningless when they are boxed and not
// interned -- every literal is a distinct object, so (eq 1 1) is false. `eql` is identity
// for everything else and value for numbers, which is what a caller almost always wants
// and what Emacs Lisp and Common Lisp both mean by the name.
static Obj *prim_eql(void *root, Obj **env, Obj **list) {
    if (length(*list) != 2)
        error("Malformed eql");
    Obj *values = eval_list(root, env, list);
    Obj *x = values->car;
    Obj *y = values->cdr->car;
    if (x->type == TINT && y->type == TINT)
        return x->value == y->value ? True : Nil;
    return x == y ? True : Nil;
}

// Evaluates a single argument. The predicates below all want exactly this, and none of
// them allocates afterwards, so the bare pointer is safe to hand back.
static Obj *eval_one(void *root, Obj **env, Obj **list, const char *name) {
    if (length(*list) != 1)
        error("Malformed %s", name);
    return eval_list(root, env, list)->car;
}

// Type predicates. Without these a script cannot ask what it is holding, which matters
// because `=` raises on anything that is not a number or a boolean and there was no way
// to check first.
static Obj *prim_numberp(void *root, Obj **env, Obj **list) {
    return eval_one(root, env, list, "numberp")->type == TINT ? True : Nil;
}

static Obj *prim_symbolp(void *root, Obj **env, Obj **list) {
    return eval_one(root, env, list, "symbolp")->type == TSYMBOL ? True : Nil;
}

static Obj *prim_consp(void *root, Obj **env, Obj **list) {
    return eval_one(root, env, list, "consp")->type == TCELL ? True : Nil;
}

// A list is a cons or the empty list, and everything that is not a cons is an atom --
// which makes () both, as it is everywhere else in this family.
static Obj *prim_listp(void *root, Obj **env, Obj **list) {
    Obj *x = eval_one(root, env, list, "listp");
    return (x->type == TCELL || x == Nil) ? True : Nil;
}

static Obj *prim_atom(void *root, Obj **env, Obj **list) {
    return eval_one(root, env, list, "atom")->type != TCELL ? True : Nil;
}

static Obj *prim_functionp(void *root, Obj **env, Obj **list) {
    const int t = eval_one(root, env, list, "functionp")->type;
    return (t == TFUNCTION || t == TPRIMITIVE) ? True : Nil;
}

static Obj *prim_length(void *root, Obj **env, Obj **list) {
    const int len = length(eval_one(root, env, list, "length"));
    if (len < 0)
        error("length takes a list");
    return make_int(root, len);
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

void add_constant_int(void *root, Obj **env, const char *name, int32_t value) {
    DEFINE1(val);
    *val = make_int(root, value);
    add_constant(root, env, name, val);
}

Obj *get_variable(void *root, Obj **env, const char *name) {
    DEFINE2(sym, bind);
    *sym = intern(root, name);
    *bind = find(env, *sym);
    if (!*bind)
        error("Undefined symbol: %s", name);

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
    add_primitive(root, env, "eql", prim_eql);
    add_primitive(root, env, "progn", prim_progn);
    add_primitive(root, env, "setcdr", prim_setcdr);
    add_primitive(root, env, "length", prim_length);
    add_primitive(root, env, "numberp", prim_numberp);
    add_primitive(root, env, "symbolp", prim_symbolp);
    add_primitive(root, env, "consp", prim_consp);
    add_primitive(root, env, "listp", prim_listp);
    add_primitive(root, env, "atom", prim_atom);
    add_primitive(root, env, "functionp", prim_functionp);
    add_primitive(root, env, "abs", prim_abs);
    add_primitive(root, env, "print", prim_print);
    // Implemented to reduce code.
    // Most of these functions can be implemented using previously declared functions.
    add_primitive(root, env, "eval", prim_eval);
    add_primitive(root, env, "apply", prim_apply);
    add_primitive(root, env, "list", prim_list);
    add_primitive(root, env, "not", prim_not);
    add_primitive(root, env, "and", prim_and);
    add_primitive(root, env, "or", prim_or);
}

//======================================================================
// Entry point
//======================================================================

void lisp_create(size_t size, size_t max_eval_stack)
{
    if (!s_heap_created)
    {
        // Refused rather than clamped: a caller that asks for more than can be addressed
        // has misjudged something, and quietly giving it less would hide that. The heap
        // is not created and lisp_is_created() stays false.
        if (size > LISP_MAX_HEAP)
            return;
        MEMORY_SIZE = size;
        s_max_eval_stack = max_eval_stack;
        eval_stack_max = 0;
        s_stack_base = NULL;
        if (gc_heap_create())
        {
            s_heap_created = true;
            Symbols = Nil;
        }
    }
}

void lisp_destroy(void)
{
    if (s_heap_created)
    {
        gc_heap_destroy();
        s_heap_created = false;
        gc_running = false;
        current_index = 0;
    }
}

bool lisp_is_created()
{
    return s_heap_created;
}

// Loop state does not unwind. An error thrown from inside a loop jumps past the end of
// prim_while, so the depth it was keeping is never decremented and #itr keeps whatever
// iteration it had failed on. Clearing all of it where evaluation starts is what makes
// that safe, and is why prim_while needs no cleanup on its error path.
static void reset_loop_state(void *root, Obj **env)
{
    loops_done = 0;
    loop_depth = 0;
    get_variable(root, env, "#itr")->cdr->value = 0;
}

bool lisp_eval(void *root, Obj **env, const char *code)
{
    current_buffer = code;
    current_index = 0;

    // This frame is what the recursion guard measures against.
    char stack_marker;
    const char *outer_base = eval_stack_begin(&stack_marker);

    // Preserve any handler a caller already installed, so a nested evaluation does not
    // leave the outer one pointing at a frame that has returned.
    jmp_buf outer_jumper;
    const bool outer_valid = error_jumper_valid;
    if (outer_valid)
        memcpy(outer_jumper, error_jumper, sizeof(jmp_buf));

    volatile bool result = false;
    DEFINE1(expr);
    while (true)
    {
        if (setjmp(error_jumper) == 0)
        {
            error_jumper_valid = true;

            // A form evaluated inside a running evaluation leaves the loop state alone,
            // so a host callback cannot clear the state of the loop that called it.
            if (!outer_valid)
                reset_loop_state(root, env);

            *expr = read_expr(root);
            if (!*expr) {
                result = true;
                break;
            }
            if (*expr == Cparen)
                error("Stray close parenthesis");
            if (*expr == Dot)
                error("Stray dot");

            const int len = print_to_buf(s_value_buf, sizeof(s_value_buf), 0,
                                         eval(root, env, expr));
            write_out(s_value_buf, len);
        }
        else {
            result = false;
            break;
        }
    }

    error_jumper_valid = outer_valid;
    if (outer_valid)
        memcpy(error_jumper, outer_jumper, sizeof(jmp_buf));
    s_stack_base = outer_base;
    return result;
}

bool safe_eval(void *root, Obj **env, Obj **expr)
{
    char stack_marker;
    const char *outer_base = eval_stack_begin(&stack_marker);

    jmp_buf outer_jumper;
    const bool outer_valid = error_jumper_valid;
    if (outer_valid)
        memcpy(outer_jumper, error_jumper, sizeof(jmp_buf));

    volatile bool result;
    if (setjmp(error_jumper) == 0)
    {
        error_jumper_valid = true;
        if (!outer_valid)
            reset_loop_state(root, env);
        const int len = print_to_buf(s_value_buf, sizeof(s_value_buf), 0,
                                     eval(root, env, expr));
        write_out(s_value_buf, len);
        result = true;
    }
    else
        result = false;

    error_jumper_valid = outer_valid;
    if (outer_valid)
        memcpy(error_jumper, outer_jumper, sizeof(jmp_buf));
    s_stack_base = outer_base;
    return result;
}

void lisp_set_cycle_yield(yield_def yield)
{
    cycle_yield = yield;
}

void lisp_set_printers(print_def out, print_def log, print_def err)
{
    print_out = out;
    print_log = log;
    print_err = err;
}

size_t lisp_mem_used(void) {
    return gc_mem_used();
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
