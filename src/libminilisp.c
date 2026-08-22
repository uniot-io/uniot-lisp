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

static bool cycle_in_progress = false;

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
            n = vsnprintf(dest + pos, remaining, fmt, args);
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
        char buf[SYMBOL_MAX_LEN];
        int n = vsnprintf(buf, sizeof(buf), fmt, args);
        if (n < 0)
            n = 0;
        if ((size_t)n > sizeof(buf) - 1)
            n = (int)sizeof(buf) - 1;
        written = n;
        print_out(buf, written);
    }

    va_end(args);

    return written;
}

static int print_to_log(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int size = 0;

    if (print_log) {
        char buf[SYMBOL_MAX_LEN];
        size = vsnprintf(buf, sizeof(buf), fmt, args);
        if (size < 0)
            size = 0;
        if ((size_t)size > sizeof(buf) - 1)
            size = (int)sizeof(buf) - 1;
        print_log(buf, size);
    }

    va_end(args);

    return size;
}

static void vsprintf_error_to_handler(const char *fmt, va_list ap)
{
    if (print_err) {
        // Reachable from user code, e.g. error("Undefined symbol: %s", name) with a
        // name approaching SYMBOL_MAX_LEN.
        char buf[SYMBOL_MAX_LEN];
        int size = vsnprintf(buf, sizeof(buf), fmt, ap);
        if (size < 0)
            size = 0;
        if ((size_t)size > sizeof(buf) - 1)
            size = (int)sizeof(buf) - 1;
        print_err(buf, size);
    }
}
// TODO: --------------------------------------------------------------------

void __attribute((noreturn)) error(const char *fmt, ...) {
    cycle_in_progress = false;

    // A longjmp out of a nested eval() skips its decrements, so the counter has to be
    // cleared here rather than unwound.
    eval_depth = 0;

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
// Garbage collector: copying (Cheney)
//======================================================================
#if MINILISP_GC == MINILISP_GC_COPYING

// The available memory is split into two halves and all objects are moved from one half to another
// every time GC is invoked. That means the address of the object keeps changing, which is why every
// reference from C has to live in the root chain rather than in a bare local.

// The pointer pointing to the beginning of the current heap
static void *memory = NULL;

// The pointer pointing to the beginning of the old heap
static void *from_space = NULL;

// The number of bytes allocated from the heap
static size_t mem_nused = 0;

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
#if MINILISP_GC_DEBUG
    if (fail_gc_alloc)
        return NULL;
#endif
    void *space = malloc(MEMORY_SIZE);
    if (space != NULL)
        STATS_SYSTEM_ADD(MEMORY_SIZE);
    return space;
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
    LISP_ASSERT(!gc_running);
    gc_running = true;

#if MINILISP_GC_STATS
    // Started here rather than after the allocation below, because obtaining the second
    // semi-space is part of what a collection costs under this strategy.
    const uint64_t stats_started = stats_now();
#endif

    // Allocate a new semi-space. `memory` and `from_space` are left untouched until
    // the allocation has succeeded, so that a failure leaves the current heap intact
    // and error() can unwind to safe_eval() with the interpreter still usable.
    //
    // This is a copying collector, so a collection needs a second heap of MEMORY_SIZE
    // bytes in addition to the one already held. On a fragmented heap that request can
    // fail even when the total free memory looks sufficient, because it must be
    // contiguous. Before this check, the collector copied into the NULL it got back.
    void *to_space = alloc_semispace();
    if (to_space == NULL) {
        // gc_running is cleared by error() before it unwinds.
        error("Out of memory: cannot allocate a GC semi-space of %u bytes",
              (unsigned)MEMORY_SIZE);
    }
    from_space = memory;
    memory = to_space;

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
            // Not recoverable: roots have been partially forwarded, so unwinding here
            // leaves the heap inconsistent and leaks from_space. It signals a bug in
            // the collector rather than a runtime condition.
            error("Bug: copy: unknown type %d", scan1->type);
        }
        scan1 = (Obj *)((uint8_t *)scan1 + scan1->size);
    }

    // Finish up GC.
    free(from_space);
    STATS_SYSTEM_SUB(MEMORY_SIZE);
    const size_t live = (size_t)((uint8_t *)scan1 - (uint8_t *)memory);
#if MINILISP_GC_DEBUG
    // mem_nused still holds the pre-collection figure at this point.
    if (debug_gc)
        printf_to_handler(NULL, 0, 0, "GC: %zu bytes out of %zu bytes copied.\n", live, mem_nused);
#endif
    mem_nused = live;
    gc_running = false;

#if MINILISP_GC_STATS
    lisp_stats.collections++;
    // Every live byte was physically moved, and no dead byte was looked at.
    lisp_stats.traced_bytes += live;
    // Compaction leaves the free space in one run, always.
    lisp_stats.free_blocks = 1;
    lisp_stats.free_largest = MEMORY_SIZE - live;
    stats_add_gc_time(stats_started);
#endif

#if MINILISP_GC_DEBUG
    if (verify_gc)
        gc_verify(root);
#endif
}

//----------------------------------------------------------------------
// Strategy interface
//----------------------------------------------------------------------

static bool gc_heap_create(void) {
    memory = alloc_semispace();
    mem_nused = 0;
    return memory != NULL;
}

static void gc_heap_destroy(void) {
    if (memory != NULL)
        STATS_SYSTEM_SUB(MEMORY_SIZE);
    free(memory);
    memory = NULL;
    from_space = NULL;
    mem_nused = 0;
}

static Obj *gc_alloc_raw(void *root, size_t size) {
    // With the debug flag on, collect before every allocation so that every object moves and any
    // stale C-side pointer is caught immediately rather than by luck.
    if (always_gc && !gc_running)
        gc(root);

    // Otherwise collect only when the remaining space is not large enough.
    if (!always_gc && MEMORY_SIZE < mem_nused + size)
        gc(root);

    // Give up if the request still cannot be satisfied, which happens when it is larger than the
    // heap or when the from-space held too many live objects. This collector compacts, so what
    // is free is one run and its size is the whole story.
    if (MEMORY_SIZE < mem_nused + size)
        error("Memory exhausted: %u wanted, %u free",
              (unsigned)size, (unsigned)(MEMORY_SIZE - mem_nused));

    Obj *obj = (Obj *)((char *)memory + mem_nused);
    mem_nused += size;
    obj->size = (int)size;
    return obj;
}

static size_t gc_mem_used(void) {
    return mem_nused;
}

static bool gc_owns(const void *p) {
    if (memory == NULL)
        return false;
    const uint8_t *base = (const uint8_t *)memory;
    const uint8_t *ptr = (const uint8_t *)p;
    return ptr >= base && ptr < base + mem_nused;
}

//----------------------------------------------------------------------
// Heap verification
//----------------------------------------------------------------------
#if MINILISP_GC_DEBUG

// Every reference held by a live object must address either managed storage or one of
// the statically allocated literals. A reference pointing anywhere else survived a
// collection without being updated, which is the failure a copying collector is prone
// to. NULL is tolerated: the root macros zero their slots before they are filled.
static void gc_verify_ref(const Obj *ref, const char *field, size_t offset) {
    if (ref == NULL)
        return;
    if (ref >= &literals[0] && ref < &literals[sizeof(literals) / sizeof(literals[0])])
        return;
    if (gc_owns(ref))
        return;
    gc_verify_fail("GC verify: %s of the object at offset %u points outside the heap",
                   field, (unsigned)offset);
}

static void gc_verify(void *root) {
    const uint8_t *base = (const uint8_t *)memory;
    const uint8_t *end = base + mem_nused;
    const uint8_t *p = base;
    size_t counted = 0;

    while (p < end) {
        const Obj *obj = (const Obj *)p;
        const size_t offset = (size_t)(p - base);

        if (!gc_verify_type_known(obj->type))
            gc_verify_fail("GC verify: unknown type %d at offset %u", obj->type, (unsigned)offset);
        if (obj->type == TMOVED)
            gc_verify_fail("GC verify: forwarding tombstone left live at offset %u", (unsigned)offset);
        if (obj->type >= TTRUE)
            gc_verify_fail("GC verify: statically allocated type %d found in the heap at offset %u",
                           obj->type, (unsigned)offset);
        if (obj->size <= 0 || (size_t)obj->size < offsetof(Obj, value))
            gc_verify_fail("GC verify: implausible size %d at offset %u", obj->size, (unsigned)offset);
        if ((size_t)obj->size % sizeof(void *) != 0)
            gc_verify_fail("GC verify: misaligned size %d at offset %u", obj->size, (unsigned)offset);
        if (p + obj->size > end)
            gc_verify_fail("GC verify: the object at offset %u runs past the end of the heap",
                           (unsigned)offset);

        switch (obj->type) {
        case TCELL:
            gc_verify_ref(obj->car, "car", offset);
            gc_verify_ref(obj->cdr, "cdr", offset);
            break;
        case TFUNCTION:
        case TMACRO:
            gc_verify_ref(obj->params, "params", offset);
            gc_verify_ref(obj->body, "body", offset);
            gc_verify_ref(obj->env, "env", offset);
            break;
        case TENV:
            gc_verify_ref(obj->vars, "vars", offset);
            gc_verify_ref(obj->up, "up", offset);
            break;
        default:
            break;
        }

        counted += (size_t)obj->size;
        p += obj->size;
    }

    if (counted != mem_nused)
        gc_verify_fail("GC verify: live objects total %u bytes but mem_nused is %u",
                       (unsigned)counted, (unsigned)mem_nused);

    // The roots are what the collection was driven from, so a stale one here means an
    // object was reached but never updated.
    for (void **frame = (void **)root; frame; frame = *(void ***)frame)
        for (int i = 1; frame[i] != ROOT_END; i++)
            gc_verify_ref((const Obj *)frame[i], "root slot", 0);

    gc_verify_ref(Symbols, "the symbol table", 0);
}
#endif  // MINILISP_GC_DEBUG

//======================================================================
// Garbage collector: mark-sweep
//======================================================================
#elif MINILISP_GC == MINILISP_GC_MARK_SWEEP

// Objects are cut out of a free list and then stay where they were put. A collection
// marks everything reachable from the roots, then walks the heap from one end to the
// other and gives back what is not marked, merging neighbouring holes as it goes.
//
// Nothing is copied, so a collection needs no memory beyond the heap it already holds.
// That is the reason to want this strategy on a device: the copying collector has to ask
// the system for a second heap of MEMORY_SIZE contiguous bytes at the exact moment
// memory is tight, and on a fragmented heap that request can fail while plenty of total
// memory remains. Here there is nothing to fail. Objects also keep their addresses, so a
// reference that escaped the root chain still points at its object.
//
// The costs sit on the other side. Allocation searches for a block that fits instead of
// bumping a pointer, a collection touches dead objects as well as live ones, and the free
// space ends up in pieces rather than in one run.

// The heap, and the length actually used. MEMORY_SIZE is rounded down to a whole number
// of words: every block size is a multiple of the word size, so a ragged tail could never
// be handed out anyway, and truncating keeps every block on an aligned address.
static uint8_t *heap = NULL;
static size_t heap_size = 0;

// The free blocks, in address order. Sweep rebuilds this from nothing each time.
static Obj *free_list = NULL;

// The number of bytes held by live objects. Free blocks are not counted, and neither is
// the handful of bytes lost when a block is handed over whole.
static size_t mem_nused = 0;

// The smallest block that can be described: a free block has to hold the tag, the size,
// and the link to the next free block. No object is smaller than this either -- the
// smallest is an integer, which is the same tag and size plus one word of value -- so any
// block that falls free is large enough to say so.
#define MS_MIN_BLOCK (offsetof(Obj, value) + sizeof(void *))

// The mark bit lives in the top bit of the tag, which the twelve tags leave spare, so
// marking costs no memory at all. It is set only between the first and last line of
// gc(), and no interpreter code runs in that window.
#define MS_MARK 0x80

static inline bool ms_marked(const Obj *obj) { return (obj->type & MS_MARK) != 0; }
static inline void ms_set_mark(Obj *obj) { obj->type = (unsigned char)(obj->type | MS_MARK); }
static inline unsigned char ms_type(const Obj *obj) { return (unsigned char)(obj->type & ~MS_MARK); }
static inline void ms_clear_mark(Obj *obj) { obj->type = ms_type(obj); }

// A free block keeps the link to its successor where an object would keep its value.
static inline Obj *ms_next_free(const Obj *block) { return (Obj *)block->moved; }
static inline void ms_set_next_free(Obj *block, Obj *next) { block->moved = next; }

static inline bool ms_in_heap(const void *p) {
    return heap != NULL && (const uint8_t *)p >= heap && (const uint8_t *)p < heap + heap_size;
}

//----------------------------------------------------------------------
// Marking
//----------------------------------------------------------------------

// Marking may not recurse. The C stack on a device is a few kilobytes and a list is as
// deep as it is long, so the work waits in an array of fixed size instead.
static Obj *mark_stack[MINILISP_GC_MARK_STACK];
static int mark_sp = 0;
static bool mark_overflow = false;

// Marks an object and notes that its references still have to be followed. Anything that
// is not in the heap -- a null slot, one of the statically allocated literals -- is not
// ours to mark and is simply ignored.
//
// The stack filling up is not a failure. The object is marked either way and only the
// note is lost, which ms_mark() recovers by rereading the heap. The overflow flag is set
// only on the branch that has just marked something new, so each recovery pass marks at
// least one more object and the process ends.
static void ms_mark_push(Obj *obj) {
    if (obj == NULL || !ms_in_heap(obj) || ms_marked(obj))
        return;
    ms_set_mark(obj);
    if (mark_sp < MINILISP_GC_MARK_STACK) {
        mark_stack[mark_sp++] = obj;
#if MINILISP_GC_STATS
        if ((size_t)mark_sp > lisp_stats.mark_stack_peak)
            lisp_stats.mark_stack_peak = (size_t)mark_sp;
#endif
    } else {
        mark_overflow = true;
    }
}

// Marks everything the given object refers to.
static void ms_mark_children(Obj *obj) {
    switch (ms_type(obj)) {
    case TINT:
    case TSYMBOL:
    case TPRIMITIVE:
        // None of these refers to a collected object.
        break;
    case TCELL:
        ms_mark_push(obj->car);
        ms_mark_push(obj->cdr);
        break;
    case TFUNCTION:
    case TMACRO:
        ms_mark_push(obj->params);
        ms_mark_push(obj->body);
        ms_mark_push(obj->env);
        break;
    case TENV:
        ms_mark_push(obj->vars);
        ms_mark_push(obj->up);
        break;
    default:
        // Unlike the copying collector, this is worth surviving. Nothing has been moved
        // or freed yet and the marks are cleared at the start of every collection, so
        // unwinding from here leaves a heap that still works.
        error("Bug: mark: unknown type %d", ms_type(obj));
    }
}

static void ms_mark_drain(void) {
    while (mark_sp > 0)
        ms_mark_children(mark_stack[--mark_sp]);
}

static void ms_mark(void *root) {
    // Clear every mark first. A collection that unwound partway -- a bug caught above,
    // an error raised out of a primitive -- leaves marks behind otherwise, and a stale
    // mark is the one kind of leftover that loses data: the next collection takes the
    // object for already scanned, never follows its references, and frees what it holds.
    for (uint8_t *p = heap; p < heap + heap_size; ) {
        Obj *obj = (Obj *)p;
        ms_clear_mark(obj);
        p += obj->size;
    }
#if MINILISP_GC_STATS
    lisp_stats.walked_bytes += heap_size;
#endif

    mark_sp = 0;
    mark_overflow = false;

    ms_mark_push(Symbols);
    for (void **frame = (void **)root; frame; frame = *(void ***)frame)
        for (int i = 1; frame[i] != ROOT_END; i++)
            if (frame[i])
                ms_mark_push((Obj *)frame[i]);
    ms_mark_drain();

    // Recover whatever the work stack could not hold. A marked object may still have
    // references that were never followed, and the heap itself records which objects are
    // marked, so it can be read back as the queue that overflowed.
    while (mark_overflow) {
        mark_overflow = false;
#if MINILISP_GC_STATS
        lisp_stats.mark_rescues++;
        lisp_stats.walked_bytes += heap_size;
#endif
        for (uint8_t *p = heap; p < heap + heap_size; ) {
            Obj *obj = (Obj *)p;
            const size_t size = (size_t)obj->size;
            if (ms_marked(obj)) {
                ms_mark_children(obj);
                ms_mark_drain();
            }
            p += size;
        }
    }
}

//----------------------------------------------------------------------
// Sweeping
//----------------------------------------------------------------------

// Turns a run of adjacent dead or already free blocks into one free block and appends it
// to the list being built. Merging here is what keeps the free space in usable pieces;
// without it a heap that had held many small objects could not satisfy a large request
// however much of it was free.
static void ms_release(uint8_t *start, size_t size, Obj **head, Obj **tail) {
    Obj *block = (Obj *)start;
#if MINILISP_GC_DEBUG
    // Scribble over the part of the block the free header does not need, so that an
    // object read through a pointer that outlived it shows nothing plausible.
    memset(start + MS_MIN_BLOCK, 0xDD, size - MS_MIN_BLOCK);
#endif
    block->type = TFREE;
    block->constant = false;
    block->size = (int)size;
    ms_set_next_free(block, NULL);

    if (*tail == NULL)
        *head = block;
    else
        ms_set_next_free(*tail, block);
    *tail = block;
}

static void ms_sweep(void) {
    Obj *head = NULL;
    Obj *tail = NULL;
    const uint8_t *end = heap + heap_size;
    uint8_t *run = NULL;  // start of the run of free memory being accumulated
    size_t run_size = 0;
    size_t live = 0;

    for (uint8_t *p = heap; p < end; ) {
        Obj *obj = (Obj *)p;
        const size_t size = (size_t)obj->size;

        if (ms_marked(obj)) {
            ms_clear_mark(obj);
            live += size;
            if (run != NULL) {
                ms_release(run, run_size, &head, &tail);
                run = NULL;
            }
        } else {
            // Dead, or free already. Either way it joins the run, and that is where
            // neighbouring holes become one block again.
            if (run == NULL) {
                run = p;
                run_size = 0;
            }
            run_size += size;
        }
        p += size;
    }
    if (run != NULL)
        ms_release(run, run_size, &head, &tail);

    free_list = head;
    mem_nused = live;

#if MINILISP_GC_STATS
    // Sweep is the one pass that has to touch dead data as well as live, which is the
    // cost this strategy pays for not moving anything.
    lisp_stats.walked_bytes += heap_size;
    lisp_stats.free_blocks = 0;
    lisp_stats.free_largest = 0;
    for (const Obj *b = free_list; b != NULL; b = ms_next_free(b)) {
        lisp_stats.free_blocks++;
        if ((size_t)b->size > lisp_stats.free_largest)
            lisp_stats.free_largest = (size_t)b->size;
    }
#endif
}

void gc(void *root) {
    LISP_ASSERT(!gc_running);
    gc_running = true;

#if MINILISP_GC_DEBUG
    const size_t before = mem_nused;
#endif
#if MINILISP_GC_STATS
    const uint64_t stats_started = stats_now();
#endif

    ms_mark(root);
    ms_sweep();

#if MINILISP_GC_STATS
    lisp_stats.collections++;
    // What survived is what marking had to reach and sweep had to keep.
    lisp_stats.traced_bytes += mem_nused;
    stats_add_gc_time(stats_started);
#endif

#if MINILISP_GC_DEBUG
    if (debug_gc)
        printf_to_handler(NULL, 0, 0, "GC: %zu bytes out of %zu bytes retained.\n", mem_nused, before);
#endif

    gc_running = false;

#if MINILISP_GC_DEBUG
    if (verify_gc)
        gc_verify(root);
#endif
}

//----------------------------------------------------------------------
// Strategy interface
//----------------------------------------------------------------------

static bool gc_heap_create(void) {
#if MINILISP_GC_DEBUG
    // This strategy allocates only here, so this is the only place the hook can act.
    // There is no counterpart to the copying collector's mid-run semi-space failure,
    // because a collection never asks for memory.
    if (fail_gc_alloc)
        return false;
#endif

    heap_size = MEMORY_SIZE & ~(sizeof(void *) - 1);
    // The block size is held in an int, and one block has to be able to span the heap.
    if (heap_size < MS_MIN_BLOCK || heap_size > (size_t)INT_MAX)
        return false;

    heap = (uint8_t *)malloc(heap_size);
    if (heap == NULL) {
        heap_size = 0;
        return false;
    }
    STATS_SYSTEM_ADD(heap_size);

    // The whole heap begins as a single free block.
    Obj *block = (Obj *)heap;
    block->type = TFREE;
    block->constant = false;
    block->size = (int)heap_size;
    ms_set_next_free(block, NULL);

    free_list = block;
    mem_nused = 0;
    return true;
}

static void gc_heap_destroy(void) {
    if (heap != NULL)
        STATS_SYSTEM_SUB(heap_size);
    free(heap);
    heap = NULL;
    heap_size = 0;
    free_list = NULL;
    mem_nused = 0;
    mark_sp = 0;
    mark_overflow = false;
}

// Takes the first block large enough. What is left over goes back on the list if it is
// big enough to describe itself; otherwise the whole block goes to the caller and the few
// spare bytes travel with the object until it is freed again.
//
// First fit walks the list on every allocation, which is the price of not moving objects.
// A heap broken into many small pieces makes that walk long, so if this ever shows up in
// a profile, resuming the search where the last one stopped is the cheapest thing to try.
static Obj *ms_take(size_t size) {
    Obj *prev = NULL;
    for (Obj *block = free_list; block != NULL; block = ms_next_free(block)) {
        const size_t avail = (size_t)block->size;
        if (avail < size) {
            prev = block;
            continue;
        }

        Obj *next = ms_next_free(block);
        const size_t rest = avail - size;
        if (rest >= MS_MIN_BLOCK) {
            Obj *remainder = (Obj *)((uint8_t *)block + size);
            remainder->type = TFREE;
            remainder->constant = false;
            remainder->size = (int)rest;
            ms_set_next_free(remainder, next);
            next = remainder;
            block->size = (int)size;
        }

        if (prev == NULL)
            free_list = next;
        else
            ms_set_next_free(prev, next);

        mem_nused += (size_t)block->size;
        return block;
    }
    return NULL;
}

static Obj *gc_alloc_raw(void *root, size_t size) {
    bool collected = false;

    // With the debug flag on, collect before every allocation. Objects do not move here,
    // so this no longer shakes out stale pointers the way it does under a copying
    // collector; what it does catch is an object freed too early, because the block is
    // retagged TFREE and scribbled over the moment nothing points at it.
    if (always_gc && !gc_running) {
        gc(root);
        collected = true;
    }

    Obj *obj = ms_take(size);
    if (obj == NULL && !collected && !gc_running) {
        // Nothing fits. Collect and look again. This cannot fail for want of system
        // memory, because collecting asks for none.
        gc(root);
        obj = ms_take(size);
    }
    if (obj == NULL) {
        // Two very different situations end here, and a bare "Memory exhausted" cannot
        // tell them apart: a heap that is genuinely full, and one with plenty of free
        // bytes that are broken into pieces too small to use. Whoever has to size a
        // device heap needs to know which, and only the collector is in a position to
        // say, so the numbers go in the message. The walk costs nothing that matters:
        // it happens once, on the way to raising.
        size_t total = 0, largest = 0;
        for (const Obj *b = free_list; b != NULL; b = ms_next_free(b)) {
            total += (size_t)b->size;
            if ((size_t)b->size > largest)
                largest = (size_t)b->size;
        }
        // total close to size means full; total far above it with a small largest means
        // fragmented.
        error("Memory exhausted: %u wanted, %u free, largest block %u",
              (unsigned)size, (unsigned)total, (unsigned)largest);
    }

    return obj;
}

static size_t gc_mem_used(void) {
    return mem_nused;
}

// Ownership, not liveness: the answer is the same for a free block, which is what the
// caller of is_valid_ptr wants, since reading a freed block raises rather than crashes.
static bool gc_owns(const void *p) {
    return ms_in_heap(p);
}

//----------------------------------------------------------------------
// Heap verification
//----------------------------------------------------------------------
#if MINILISP_GC_DEBUG

// Every reference held by a live object must address either a live object in the heap or
// one of the statically allocated literals. Pointing at a free block is the failure a
// non-moving collector is prone to: the object was collected while something still held
// it. NULL is tolerated, because the root macros zero their slots before filling them.
static void gc_verify_ref(const Obj *ref, const char *field, size_t offset) {
    if (ref == NULL)
        return;
    if (ref >= &literals[0] && ref < &literals[sizeof(literals) / sizeof(literals[0])])
        return;
    if (!gc_owns(ref))
        gc_verify_fail("GC verify: %s of the object at offset %u points outside the heap",
                       field, (unsigned)offset);
    if (ms_type(ref) == TFREE)
        gc_verify_fail("GC verify: %s of the object at offset %u points at a freed block",
                       field, (unsigned)offset);
    if (!gc_verify_type_known(ms_type(ref)))
        gc_verify_fail("GC verify: %s of the object at offset %u points at type %d, which is not an object",
                       field, (unsigned)offset, ms_type(ref));
}

static void gc_verify(void *root) {
    const uint8_t *end = heap + heap_size;
    size_t live = 0;
    size_t free_bytes = 0;
    size_t free_blocks = 0;
    bool prev_was_free = false;

    for (const uint8_t *p = heap; p < end; ) {
        const Obj *obj = (const Obj *)p;
        const size_t offset = (size_t)(p - heap);
        const unsigned char type = ms_type(obj);

        // Marks belong to a collection in progress and nothing else.
        if (ms_marked(obj))
            gc_verify_fail("GC verify: mark left set at offset %u", (unsigned)offset);
        if (obj->size <= 0 || (size_t)obj->size < MS_MIN_BLOCK)
            gc_verify_fail("GC verify: implausible size %d at offset %u", obj->size, (unsigned)offset);
        if ((size_t)obj->size % sizeof(void *) != 0)
            gc_verify_fail("GC verify: misaligned size %d at offset %u", obj->size, (unsigned)offset);
        if (p + obj->size > end)
            gc_verify_fail("GC verify: the block at offset %u runs past the end of the heap",
                           (unsigned)offset);

        if (type == TFREE) {
            // Sweep merges neighbours and a split cannot produce a pair, so two free
            // blocks in a row mean the merge was missed and the space is now unusable
            // for anything larger than either piece.
            if (prev_was_free)
                gc_verify_fail("GC verify: two free blocks in a row at offset %u, which sweep should have merged",
                               (unsigned)offset);
            free_bytes += (size_t)obj->size;
            free_blocks++;
            prev_was_free = true;
        } else {
            if (!gc_verify_type_known(type))
                gc_verify_fail("GC verify: unknown type %d at offset %u", type, (unsigned)offset);
            if (type == TMOVED)
                gc_verify_fail("GC verify: a forwarding tombstone at offset %u, which this collector never writes",
                               (unsigned)offset);
            if (type >= TTRUE)
                gc_verify_fail("GC verify: statically allocated type %d found in the heap at offset %u",
                               type, (unsigned)offset);

            switch (type) {
            case TCELL:
                gc_verify_ref(obj->car, "car", offset);
                gc_verify_ref(obj->cdr, "cdr", offset);
                break;
            case TFUNCTION:
            case TMACRO:
                gc_verify_ref(obj->params, "params", offset);
                gc_verify_ref(obj->body, "body", offset);
                gc_verify_ref(obj->env, "env", offset);
                break;
            case TENV:
                gc_verify_ref(obj->vars, "vars", offset);
                gc_verify_ref(obj->up, "up", offset);
                break;
            default:
                break;
            }

            live += (size_t)obj->size;
            prev_was_free = false;
        }

        p += obj->size;
    }

    if (live != mem_nused)
        gc_verify_fail("GC verify: live objects total %u bytes but mem_nused is %u",
                       (unsigned)live, (unsigned)mem_nused);
    // Nothing may go missing. Every byte of the heap is either in an object or on the
    // free list, and a walk that ends anywhere else means a block size is wrong.
    if (live + free_bytes != heap_size)
        gc_verify_fail("GC verify: %u live bytes plus %u free bytes do not account for the %u byte heap",
                       (unsigned)live, (unsigned)free_bytes, (unsigned)heap_size);

    // The free list has to name exactly the free blocks the walk found, once each. A
    // shorter list has mislaid memory; a longer one is about to hand the same block to
    // two callers.
    size_t listed = 0;
    for (const Obj *block = free_list; block != NULL; block = ms_next_free(block)) {
        if (!ms_in_heap(block))
            gc_verify_fail("GC verify: the free list leaves the heap");
        if (ms_type(block) != TFREE)
            gc_verify_fail("GC verify: a live object is on the free list at offset %u",
                           (unsigned)((const uint8_t *)block - heap));
        if (++listed > free_blocks)
            gc_verify_fail("GC verify: the free list is longer than the %u free blocks in the heap, so it repeats or loops",
                           (unsigned)free_blocks);
    }
    if (listed != free_blocks)
        gc_verify_fail("GC verify: the heap holds %u free blocks but the free list names %u",
                       (unsigned)free_blocks, (unsigned)listed);

    // The roots are what the marking was driven from, so a bad one here means an object
    // was reached through something that is no longer an object.
    for (void **frame = (void **)root; frame; frame = *(void ***)frame)
        for (int i = 1; frame[i] != ROOT_END; i++)
            gc_verify_ref((const Obj *)frame[i], "root slot", 0);

    gc_verify_ref(Symbols, "the symbol table", 0);
}
#endif  // MINILISP_GC_DEBUG

#else
#error "Unknown MINILISP_GC strategy"
#endif

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
int print_to_buf(char *buf, size_t size, int pos, Obj *obj) {
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
            pos = printf_to_handler(buf, size, pos, " ");
            obj = obj->cdr;
        }
        pos = printf_to_handler(buf, size, pos, ")");
        return pos;

#define CASE(type, ...)                                     \
    case type:                                              \
        return printf_to_handler(buf, size, pos, __VA_ARGS__);
    CASE(TINT, "%d", obj->value);
    CASE(TSYMBOL, "%s", obj->name);
    CASE(TPRIMITIVE, "<primitive>");
    CASE(TFUNCTION, "<function>");
    CASE(TMACRO, "<macro>");
    CASE(TMOVED, "<moved>");
    CASE(TFREE, "<free>");
    CASE(TTRUE, "#t");
    CASE(TNIL, "()");
#undef CASE
    default:
        error("Bug: print: Unknown tag type: %d", obj->type);
    }
}

void print(Obj *obj) {
    print_to_buf(NULL, 0, 0, obj);
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

static Obj *eval_inner(void *root, Obj **env, Obj **obj);

// Current eval nesting, and the deepest nesting reached since eval_depth_max was
// last cleared. Exposed so a recursion limit can be sized from measurements on the
// target rather than guessed. eval_depth is reset by error(), because a longjmp out
// of a nested eval skips the decrements below.
int eval_depth = 0;
int eval_depth_max = 0;

// Nesting cap, taken from lisp_create(). Zero means no limit.
static int s_max_eval_depth = 0;

// Evaluates the S expression.
Obj *eval(void *root, Obj **env, Obj **obj) {
    if (++eval_depth > eval_depth_max)
        eval_depth_max = eval_depth;
    if (s_max_eval_depth > 0 && eval_depth > s_max_eval_depth) {
        // error() clears eval_depth before unwinding, so no decrement is owed here.
        error("Recursion too deep: eval nesting exceeded %d", s_max_eval_depth);
    }
    Obj *result = eval_inner(root, env, obj);
    --eval_depth;
    return result;
}

static Obj *eval_inner(void *root, Obj **env, Obj **obj) {
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

        if ((*itr)->cdr->value > MAX_LOOP_ITERATIONS) {
            cycle_in_progress = false;
            error("Maximum loop iterations (%d) exceeded. Possible infinite loop detected.", MAX_LOOP_ITERATIONS);
        }

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
    print_to_buf(buf, sizeof(buf), 0, *tmp);
    printf_to_handler(NULL, 0, 0, "%s", buf);
    print_to_log("%s", buf);
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

// (= <integer|boolean> <integer|boolean>)
static Obj *prim_num_eq(void *root, Obj **env, Obj **list) {
    if (length(*list) != 2)
        error("Malformed =");
    Obj *values = eval_list(root, env, list);
    Obj *x = values->car;
    Obj *y = values->cdr->car;
    if ((x->type != TINT && x->type != TTRUE && x->type != TNIL) ||
        (y->type != TINT && y->type != TTRUE && y->type != TNIL))
        error("= takes only numbers and booleans");

    if (x->type == TINT && y->type == TINT)
        return x->value == y->value ? True : Nil;

    int x_bool_val = x->type == TINT ? (x->value == 0 ? 0 : 1) : x->type == TTRUE ? 1 : 0;
    int y_bool_val = y->type == TINT ? (y->value == 0 ? 0 : 1) : y->type == TTRUE ? 1 : 0;

    // TODO: implement list matching?

    return x_bool_val == y_bool_val ? True : Nil;
}

// TODO: implement null? function for checking if the object is Nil

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

void lisp_create(size_t size, int max_eval_depth)
{
    if (!s_heap_created)
    {
        MEMORY_SIZE = size;
        s_max_eval_depth = max_eval_depth;
        eval_depth = 0;
        eval_depth_max = 0;
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

bool lisp_eval(void *root, Obj **env, const char *code)
{
    current_buffer = code;
    current_index = 0;

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
            *expr = read_expr(root);
            if (!*expr) {
                result = true;
                break;
            }
            if (*expr == Cparen)
                error("Stray close parenthesis");
            if (*expr == Dot)
                error("Stray dot");

            char buf[SYMBOL_MAX_LEN];
            print_to_buf(buf, sizeof(buf), 0, eval(root, env, expr));
            printf_to_handler(NULL, 0, 0, "%s", buf);
        }
        else {
            result = false;
            break;
        }
    }

    error_jumper_valid = outer_valid;
    if (outer_valid)
        memcpy(error_jumper, outer_jumper, sizeof(jmp_buf));
    return result;
}

bool safe_eval(void *root, Obj **env, Obj **expr)
{
    jmp_buf outer_jumper;
    const bool outer_valid = error_jumper_valid;
    if (outer_valid)
        memcpy(outer_jumper, error_jumper, sizeof(jmp_buf));

    volatile bool result;
    if (setjmp(error_jumper) == 0)
    {
        error_jumper_valid = true;
        char buf[SYMBOL_MAX_LEN];
        print_to_buf(buf, sizeof(buf), 0, eval(root, env, expr));
        printf_to_handler(NULL, 0, 0, "%s", buf);
        result = true;
    }
    else
        result = false;

    error_jumper_valid = outer_valid;
    if (outer_valid)
        memcpy(error_jumper, outer_jumper, sizeof(jmp_buf));
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
