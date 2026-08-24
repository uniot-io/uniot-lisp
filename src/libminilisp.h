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

#define MAX_LOOP_ITERATIONS 9999

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

#include <assert.h>
#include <ctype.h>
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
    int size;

    // Object values.
    union {
        // Int
        int value;
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
    };
} Obj;

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

    // Mark-sweep bookkeeping
    size_t mark_stack_peak;
    size_t mark_rescues;     // passes over the heap made because the work stack filled
} LispStats;

extern LispStats lisp_stats;

// Zeroes the counters. The amount currently held from the system is kept, since it
// describes the heap rather than the work done, and the peak restarts from it.
void lisp_stats_reset(void);
#endif

// Current eval nesting, and the deepest nesting reached since eval_depth_max was last
// cleared. Clear eval_depth_max yourself before a measurement and read it afterwards.
extern int eval_depth;
extern int eval_depth_max;

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

#ifndef MINILISP_GC
#define MINILISP_GC MINILISP_GC_COPYING
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

// The size of the heap in byte
extern size_t MEMORY_SIZE;

void gc(void *root);

Obj *read_expr(void *root);

Obj *eval(void *root, Obj **env, Obj **obj);

Obj *eval_list(void *root, Obj **env, Obj **list);

Obj *make_int(void *root, int value);

Obj *make_symbol(void *root, const char *name);

struct Obj *make_env(void *root, Obj **vars, Obj **up);

int length(Obj *list);

void __attribute((noreturn)) error(const char *fmt, ...);

void define_constants(void *root, Obj **env);

void define_primitives(void *root, Obj **env);

int print_to_buf(char *buf, size_t size, int pos, Obj *obj);

void print(Obj *obj);

void add_primitive(void *root, Obj **env, const char *name, Primitive *fn);

void add_constant(void *root, Obj **env, const char *name, Obj **val);

void add_constant_int(void *root, Obj **env, const char *name, int value);

Obj *get_variable(void *root, Obj **env, const char *name);

// Creates the interpreter.
//
// `size` is the heap in bytes. `max_eval_depth` caps the nesting of eval(): recursion
// runs on the C stack, and overrunning it crashes the host instead of raising an error,
// so the depth is checked rather than discovered. Pass 0 for no limit.
//
// One level costs 168 bytes on an ESP8266 and 184 on an ESP32-C3, measured on target.
// A host with a large stack can pass a generous value; an embedded host should measure.
void lisp_create(size_t size, int max_eval_depth);

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
