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

#ifndef MINILISP_H
#define MINILISP_H

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

#define SEMVER_TO_INT(major, minor, patch) ((major * 10000) + (minor * 100) + patch)

#define LISP_VERSION SEMVER_TO_INT(0, 2, 5)

// The three limits below used to be one constant, which meant a symbol name, an error
// message and a printed value were all bounded by whatever suited the largest of them.
// They are unrelated, and sizing them apart costs less RAM while giving the one a user
// actually sees more room.

// Longest symbol name that can be read. The longest in use anywhere is thirteen
// characters, so this is five times what is needed; a longer name raises.
#define SYMBOL_MAX_LEN 64

// Longest error or log message: the longest format string in the library plus a symbol
// name substituted into it, with room to spare.
#define LISP_MESSAGE_MAX_LEN 128

// Longest printed value. This is what a script's output is truncated to, so of the three
// it is the one a user notices.
#define LISP_PRINT_MAX_LEN 256

// Two bounds, not one, because there are two ways for a script to repeat forever and
// they are not the same shape. A `while` whose condition never goes false spins inside
// prim_while; a tail-recursive function that never reaches a base case spins inside a
// single eval() frame. Each is caught where it happens.
//
// What neither of these counts is work. A count of everything eval does cannot tell a
// program that is stuck from one that is merely large -- eight queens is two and a half
// million evaluations with nothing whatever wrong with it, and it contains no loop at
// all. So these bound repetition of a construct, not the total cost of a program.

// Iterations of `while`, summed across every loop in the top-level form rather than
// counted per loop. Per loop means nothing: a loop inside a loop multiplies while both
// stay under their own limit, and two nested 9999s are 10^8 iterations. Summing the
// inner loop's iterations over every pass of the outer one is that product.
//
// The number is a length of time in disguise, so it was measured rather than guessed.
// One iteration on an ESP8266 at 80MHz, from tools/LispGC in uniot-core:
//
//   empty body                41 us      a bare (while c 1)
//   (setq n (+ n 1))          78 us      arithmetic and a rebinding
//   a call to a function     112 us      what a rule of any substance does
//
// so this is between 0.8 and 2.2 seconds of a device sitting on a loop that is never
// going to end. A workstation is 130 to 150 times faster per iteration, which is why
// picking this from host timings does not work: 100000 measures as a second there and
// as four to eleven on the hardware it is meant to protect.
//
// Generous against what a loop legitimately does rather than against the clock. The
// largest real nest to be found on a device is a scan of something like a 100x100 grid,
// which is 10000, and this leaves room over it.
#ifndef MINILISP_MAX_LOOP_ITERATIONS
#define MINILISP_MAX_LOOP_ITERATIONS 20000
#endif

// Tail calls made without leaving one eval() frame. Eliminating tail calls turned them
// into a loop inside that frame, which cost them the guard they used to have: a runaway
// tail call no longer runs out of stack, it just spins. This is what replaces it.
//
// The count lives in the frame because that is exactly where a runaway lives too. A
// chain that never reaches a base case never leaves the frame, so its count never
// starts over and it always trips. Ordinary recursion returns, and every return is a
// frame gone and a count with it, so calling one function a million times costs nothing
// here -- which is the whole point, and what a count of work got wrong.
//
// Passes, not iterations: an `if` around the recursive call is itself a tail position and
// costs one, so a tail-recursive loop gets through roughly half this number of turns.
#ifndef MINILISP_MAX_TAIL_CALLS
#define MINILISP_MAX_TAIL_CALLS MINILISP_MAX_LOOP_ITERATIONS
#endif

// The range a script sees, and the format for printing one. PRId32 rather than "%d"
// because int32_t is `long` on RISC-V targets and `int` elsewhere, so no single literal
// specifier is right for all of them.
#define LISP_INT_MAX INT32_MAX
#define LISP_INT_MIN INT32_MIN
#define LISP_INT_FMT PRId32

// Where the strings live.
//
// On an ESP8266 .rodata is placed in DRAM, so every message this library carries costs
// RAM -- measured at 2096 bytes across 134 strings, of which the error and format
// messages are 1743. PSTR puts a literal in a flash section instead, and vsnprintf_P
// reads it back from there. Nothing else needs it: an ESP32 and an ESP32-C3 already
// place .rodata in mapped flash, and a host has no such distinction.
//
// Applied through these rather than at the call sites, so that `error("...")` keeps
// reading as an ordinary call and cannot be written the wrong way by accident. The
// format string must be a literal, which every one of them is.
#if defined(__ets__) || defined(ARDUINO_ARCH_ESP8266) || defined(ESP8266)
// ESP8266: .rodata is linked into DRAM, so a literal costs RAM until it is moved.
#include <sys/pgmspace.h>
#define LISP_FMT(s) PSTR(s)
#define LISP_VSNPRINTF vsnprintf_P
#elif defined(ARDUINO_ARCH_ESP32) || defined(ESP32)
// ESP32 and ESP32-C3 link .rodata into mapped flash already, and their pgmspace defines
// PSTR(s) as (s) and vsnprintf_P as vsnprintf. Naming them anyway costs nothing and puts
// the intent in the code: a reader asking "where do the messages live on an ESP32?" gets
// an answer here rather than having to know what the linker script does. It also means a
// toolchain that stops placing them in flash is a definition to change, not a silent
// regression. Bare ESP-IDF has no pgmspace and falls through below, which is correct for
// the same reason.
#include <pgmspace.h>
#define LISP_FMT(s) PSTR(s)
#define LISP_VSNPRINTF vsnprintf_P
#else
// A host, and WebAssembly: no such distinction to make.
#define LISP_FMT(s) (s)
#define LISP_VSNPRINTF vsnprintf
#endif

#define ROOT_END ((void *)-1)

#define ADD_ROOT(size)                   \
    void *root_ADD_ROOT_[size + 2];      \
    root_ADD_ROOT_[0] = root;            \
    for (int i = 1; i <= size; i++)      \
        root_ADD_ROOT_[i] = NULL;        \
    root_ADD_ROOT_[size + 1] = ROOT_END; \
    root = root_ADD_ROOT_

#define DEFINE1(var1) \
    ADD_ROOT(1);      \
    Obj **var1 = (Obj **)(root_ADD_ROOT_ + 1)

#define DEFINE2(var1, var2)                    \
    ADD_ROOT(2);                               \
    Obj **var1 = (Obj **)(root_ADD_ROOT_ + 1); \
    Obj **var2 = (Obj **)(root_ADD_ROOT_ + 2)

#define DEFINE3(var1, var2, var3)              \
    ADD_ROOT(3);                               \
    Obj **var1 = (Obj **)(root_ADD_ROOT_ + 1); \
    Obj **var2 = (Obj **)(root_ADD_ROOT_ + 2); \
    Obj **var3 = (Obj **)(root_ADD_ROOT_ + 3)

#define DEFINE4(var1, var2, var3, var4)        \
    ADD_ROOT(4);                               \
    Obj **var1 = (Obj **)(root_ADD_ROOT_ + 1); \
    Obj **var2 = (Obj **)(root_ADD_ROOT_ + 2); \
    Obj **var3 = (Obj **)(root_ADD_ROOT_ + 3); \
    Obj **var4 = (Obj **)(root_ADD_ROOT_ + 4)

#define DEFINE5(var1, var2, var3, var4, var5)  \
    ADD_ROOT(5);                               \
    Obj **var1 = (Obj **)(root_ADD_ROOT_ + 1); \
    Obj **var2 = (Obj **)(root_ADD_ROOT_ + 2); \
    Obj **var3 = (Obj **)(root_ADD_ROOT_ + 3); \
    Obj **var4 = (Obj **)(root_ADD_ROOT_ + 4); \
    Obj **var5 = (Obj **)(root_ADD_ROOT_ + 5)

#define DEFINE6(var1, var2, var3, var4, var5, var6) \
    ADD_ROOT(6);                               \
    Obj **var1 = (Obj **)(root_ADD_ROOT_ + 1); \
    Obj **var2 = (Obj **)(root_ADD_ROOT_ + 2); \
    Obj **var3 = (Obj **)(root_ADD_ROOT_ + 3); \
    Obj **var4 = (Obj **)(root_ADD_ROOT_ + 4); \
    Obj **var5 = (Obj **)(root_ADD_ROOT_ + 5); \
    Obj **var6 = (Obj **)(root_ADD_ROOT_ + 6)

// Note that ADD_ROOT reassigns `root` to the frame it declares, so none of these may be
// used inside a loop body: a second pass would link the frame to itself and the
// collector's walk of the root chain would never terminate. Declare them before the
// loop. eval() needs seven for exactly that reason.
#define DEFINE7(var1, var2, var3, var4, var5, var6, var7) \
    ADD_ROOT(7);                               \
    Obj **var1 = (Obj **)(root_ADD_ROOT_ + 1); \
    Obj **var2 = (Obj **)(root_ADD_ROOT_ + 2); \
    Obj **var3 = (Obj **)(root_ADD_ROOT_ + 3); \
    Obj **var4 = (Obj **)(root_ADD_ROOT_ + 4); \
    Obj **var5 = (Obj **)(root_ADD_ROOT_ + 5); \
    Obj **var6 = (Obj **)(root_ADD_ROOT_ + 6); \
    Obj **var7 = (Obj **)(root_ADD_ROOT_ + 7)

#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//======================================================================
// Lisp objects
//======================================================================

// The Lisp object type
enum
{
    // Regular objects visible from the user
    TINT = 1,
    TCELL,
    TSYMBOL,
    TPRIMITIVE,
    TFUNCTION,
    TMACRO,
    TENV,
    // The marker that indicates the object has been moved to other location by GC. The new location
    // can be found at the forwarding pointer. Only the functions to do garbage collection set and
    // handle the object of this type. Other functions will never see the object of this type.
    TMOVED,
    // Const objects. They are statically allocated and will never be managed by GC.
    TTRUE,
    TNIL,
    TDOT,
    TCPAREN,
    // The marker for a block of heap that holds no object. Only a collector that leaves
    // objects where they are has any use for it, so only the mark-sweep strategy sets it.
    // Seeing it anywhere else means a freed object is still being read.
    TFREE,
};

// Pins the value union to exactly the alignment a pointer needs: no less, so nothing is
// under-aligned, and no more, so the two tag bytes ahead of it are not padded out.
//
// Insurance rather than a fix for anything currently wrong. Xtensa rounds an aggregate's
// alignment up to its size when that size is a power of two, so a union of two pointers
// lands on eight and pushes the union to offset eight, undoing the narrow size field. The
// union below is twelve bytes and escapes that, but it would not take much of a change to
// walk into it, and the assertion after the struct is what would catch it.
#if defined(__GNUC__) || defined(__clang__)
#define LISP_HAVE_ALIGN_ATTRIBUTE 1
#define LISP_ALIGN_AS_POINTER __attribute__((aligned(__alignof__(void *))))
#else
// Without it every object is four bytes larger on a 32-bit target. That costs heap, not
// correctness.
#define LISP_ALIGN_AS_POINTER
#endif

// Typedef for the primitive function
struct Obj;
typedef struct Obj *Primitive(void *root, struct Obj **env, struct Obj **args);

// The object type
typedef struct Obj
{
    // The first word of the object represents the type of the object. Any code that handles object
    // needs to check its type first, then access the following union members.
    unsigned char type;

    // It indicates if object is a constant value.
    unsigned char constant;

    // The total size of the object, including "type" field, this field, the contents, and the
    // padding at the end of the object.
    //
    // Sixteen bits rather than a full int, which is what makes the header four bytes on a
    // 32-bit target instead of eight: an int here would sit on a four-byte boundary and
    // leave two bytes of padding behind the two tag bytes. That is four bytes saved on
    // every object, and the objects are small -- an integer is eight bytes with this
    // field and twelve without. The ceiling it imposes is LISP_MAX_HEAP.
    uint16_t size;

    // Object values.
    union {
        // Int. Fixed at 32 bits rather than left as `int`, because the width is part of
        // what the language promises now that the arithmetic raises on overflow: the
        // limit a script runs into is this type's, so it should be stated here and not
        // inherited from whatever the target calls an int.
        int32_t value;
        // Cell
        struct
        {
            struct Obj *car;
            struct Obj *cdr;
        };
        // Symbol
        char name[1];
        // Primitive
        Primitive *fn;
        // Function or Macro
        struct
        {
            struct Obj *params;
            struct Obj *body;
            struct Obj *env;
        };
        // Environment frame. This is a linked list of association lists
        // containing the mapping from symbols to their value.
        struct
        {
            struct Obj *vars;
            struct Obj *up;
        };
        // Forwarding pointer
        void *moved;
    } LISP_ALIGN_AS_POINTER;
} Obj;

// The narrow size field only pays off if the union starts immediately after it, giving a
// four byte header on a 32-bit target instead of eight. That is worth a third of the size
// of a small object, so a change that silently gave it back should stop the build rather
// than quietly shrink the usable heap.
#if defined(LISP_HAVE_ALIGN_ATTRIBUTE)
typedef char lisp_assert_header_is_packed[
    (sizeof(void *) != 4 || offsetof(Obj, value) == 4) ? 1 : -1];
#endif

typedef void (*yield_def)();
typedef void (*print_def)(const char *msg, int size);

// Constants
extern Obj *True;
extern Obj *Nil;
extern Obj *Dot;
extern Obj *Cparen;

// True while a collection is in progress.
extern bool gc_running;

// When set, a collection runs before every allocation, so that every object moves and a
// stale reference from C is caught at once rather than by luck.
extern bool always_gc;

// Diagnostics for working on the collector: a log line per collection, an allocation
// failure hook, heap verification after every collection, and the interpreter's internal
// invariant checks.
//
// Compiled out by default. A device pays for them permanently, and they are wanted only
// while chasing a bug in the collector or in a host's primitives, which is bench work.
// Build with -D MINILISP_GC_DEBUG=1 to include them; the REPL already does.
#ifndef MINILISP_GC_DEBUG
#define MINILISP_GC_DEBUG 0
#endif

#if MINILISP_GC_DEBUG
// Logs a line for each collection.
extern bool debug_gc;

// When set, heap allocation reports failure without calling malloc, so the out-of-memory
// recovery path can be exercised deterministically.
extern bool fail_gc_alloc;

// When set, the heap is walked after every collection and any broken invariant stops the
// program. Combine with always_gc to check the whole heap after every allocation.
extern bool verify_gc;
#endif

// Collector instrumentation, for comparing one strategy against another. Compiled out by
// default: the counters are individually trivial but they sit on the allocation path,
// which is the hottest path in the interpreter, and a device has no use for them.
#ifndef MINILISP_GC_STATS
#define MINILISP_GC_STATS 0
#endif

#if MINILISP_GC_STATS
// A monotonic clock, in whatever unit suits the caller. The library has none of its own
// that is portable across a workstation, a device and a browser, so the caller supplies
// one. Left NULL, the timing figures stay zero and everything else is still counted.
typedef uint64_t (*clock_def)(void);
extern clock_def lisp_stats_clock;

typedef struct {
    // Work done
    size_t collections;
    size_t allocations;
    size_t alloc_requested;  // bytes alloc() asked the collector for
    size_t alloc_supplied;   // bytes it actually handed over; the difference is what is
                             // lost to blocks too small to split, and is always zero for
                             // a collector that allocates by bumping a pointer
    size_t traced_bytes;     // live bytes the collector had to handle: copied, by the
                             // copying collector; marked, by mark-sweep
    size_t walked_bytes;     // heap bytes stepped over whether live or not. Zero for a
                             // copying collector, which never looks at dead data; for
                             // mark-sweep this is the whole heap, once per pass

    // Time, in the clock's units
    uint64_t gc_time;
    uint64_t gc_time_max;    // the longest single collection, which is the figure that
                             // decides whether a collection can be felt

    // Memory
    size_t live_peak;        // most bytes live at any one time
    size_t system_now;       // bytes currently held from the system allocator
    size_t system_peak;      // the most ever held at once. This is where the copying
                             // collector's second semi-space becomes visible, and it is
                             // the number that decides whether a heap fits on a device

    // The shape of the free space after the last collection. A collector that compacts
    // leaves one run; one that does not leaves as many pieces as it leaves.
    size_t free_blocks;
    size_t free_largest;

    // Free-list nodes stepped over across all allocations. Divided by `allocations`
    // this is how far the average request has to look for a block that fits, which is
    // the figure that decides whether the search strategy is worth changing.
    size_t alloc_hops;

    // Mark-sweep bookkeeping
    size_t mark_stack_peak;
    size_t mark_rescues;     // passes over the heap made because the work stack filled
} LispStats;

extern LispStats lisp_stats;

// Zeroes the counters. The amount currently held from the system is kept, since it
// describes the heap rather than the work done, and the peak restarts from it.
void lisp_stats_reset(void);
#endif

// The most C stack a single evaluation has consumed since this was last cleared, in
// bytes. This is the quantity the recursion guard is measured against, so it is the one
// to read when choosing a limit for a target. Cleared by lisp_create().
extern size_t eval_stack_max;

// Garbage collection strategy. Override it with a build flag.
//
//   COPYING     Cheney. Allocation is a pointer bump and collection touches only live
//               data, but every collection asks the system for a second heap of
//               MEMORY_SIZE bytes, and objects move, so a reference held anywhere
//               outside the root chain goes stale.
//
//   MARK_SWEEP  Objects never move and a collection needs no memory beyond the heap
//               itself, which removes the failure a small fragmented device heap is
//               prone to. In exchange, allocation searches a free list and a collection
//               walks the whole heap rather than just the live part.
#define MINILISP_GC_COPYING 1
#define MINILISP_GC_MARK_SWEEP 2

// Mark-sweep by default, and deliberately the same everywhere. The two collectors do
// not merely differ in speed: they run out of memory under different conditions, so a
// script that fits under one can fail under the other. Selecting per platform would mean
// the same program behaving differently on different boards, and on the browser
// simulator differently again from both. One collector everywhere is worth more than the
// few percent the other is faster by.
//
// Mark-sweep is the one that fits the tightest target, which is what makes it the right
// default for all of them: it needs no memory beyond the heap it already holds, so a
// collection cannot fail for want of a contiguous block.
#ifndef MINILISP_GC
#define MINILISP_GC MINILISP_GC_MARK_SWEEP
#endif

// Depth of the mark-sweep collector's explicit work stack, in entries. Marking may not
// recurse: the C stack on a device is measured in single-digit kilobytes and a structure
// as deep as a long list would overrun it. Overflow is not an error, only slower -- the
// collector falls back to rescanning the heap -- so this trades a fixed slab of RAM
// against collection time.
//
// 128 comes from measuring rather than guessing. Across the benchmark workloads, 64
// entries overflowed constantly (24 to 91 rescans of the whole heap in the call-heavy
// ones) and 128 removed nearly all of them, cutting time spent collecting by about a
// fifth; 256 bought almost nothing more. At four bytes an entry that is 512 bytes.
#ifndef MINILISP_GC_MARK_STACK
#define MINILISP_GC_MARK_STACK 128
#endif

// The largest heap that can be asked for. Every block records its own length in the
// sixteen-bit `size` field above, and under mark-sweep a run of free memory is one block
// that can span the whole heap, so the heap cannot be longer than that field can count.
// lisp_create() refuses a larger request and lisp_is_created() stays false.
#define LISP_MAX_HEAP 65535

// The size of the heap in byte
extern size_t MEMORY_SIZE;

void gc(void *root);

Obj *read_expr(void *root);

Obj *eval(void *root, Obj **env, Obj **obj);

Obj *eval_list(void *root, Obj **env, Obj **list);

Obj *make_int(void *root, int32_t value);

Obj *make_symbol(void *root, const char *name);

struct Obj *make_env(void *root, Obj **vars, Obj **up);

int length(Obj *list);

void __attribute((noreturn)) lisp_error(const char *fmt, ...);

// The format string goes to flash where the target keeps .rodata in RAM. A macro, so a
// caller writes error("...") and gets it without knowing.
#define error(fmt, ...) lisp_error(LISP_FMT(fmt), ##__VA_ARGS__)

void define_constants(void *root, Obj **env);

void define_primitives(void *root, Obj **env);

int print_to_buf(char *buf, size_t size, int pos, Obj *obj);

void print(Obj *obj);

void add_primitive(void *root, Obj **env, const char *name, Primitive *fn);

void add_constant(void *root, Obj **env, const char *name, Obj **val);

void add_constant_int(void *root, Obj **env, const char *name, int32_t value);

Obj *get_variable(void *root, Obj **env, const char *name);

// Creates the interpreter.
//
// `size` is the heap in bytes. `max_eval_stack` caps how much C stack a single
// evaluation may consume, also in bytes; zero lifts the cap. Recursion in a script runs
// on the C stack, and overrunning it crashes the host rather than raising, so the cost
// is measured as it is incurred instead of being inferred.
//
// Bytes rather than a count of nesting levels, because levels are not what runs out.
// Nesting is only a proxy for stack, and a poor one: a level that prints costs several
// times what a bare call costs, so a nesting limit is either too tight for simple
// scripts or too loose for the ones that do something. It also has to be retuned for
// every chip, and cannot see the stack the host's own error handler will need on top.
//
// Pick it as: stack available where lisp_eval() is called, less what raising an error
// costs on this host, less a margin. tools/LispEvalDepth measures all three on target.
void lisp_create(size_t size, size_t max_eval_stack);

void lisp_destroy(void);

bool lisp_is_created();

bool lisp_eval(void *root, Obj **env, const char *code);

bool safe_eval(void *root, Obj **env, Obj **expr);

void lisp_set_cycle_yield(yield_def yield);

// Installs the handlers that receive output, log lines and errors.
//
// Two things are required of a handler, because the text it is given lives in a buffer
// the interpreter reuses rather than in one allocated per call:
//
//   - Consume the text before returning. Copy it; do not keep the pointer.
//   - Do not evaluate Lisp from inside a handler, directly or indirectly. An error
//     handler in particular runs part-way through raising, with the interpreter's state
//     half unwound.
//
// Both are easy to meet -- copy the bytes, and queue anything else for later rather than
// doing it inline -- but neither is checked, and breaking either corrupts the message
// rather than failing loudly.
void lisp_set_printers(print_def out, print_def log, print_def err);

size_t lisp_mem_used(void);

int lisp_error_idx(void);

Obj *handle_pruner(void *root, Obj **env, Obj **list, const char *handler_name, bool include_name);
#ifdef __cplusplus
}
#endif // __cplusplus

#endif // MINILISP_H
