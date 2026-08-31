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


// Garbage collector: copying (Cheney)
//
// Included by libminilisp.c, not compiled on its own: everything here reaches the rest
// of the interpreter through file-level statics -- the literals, the symbol table, the
// root chain, error() -- and making it a translation unit of its own would mean giving
// all of that external linkage to no benefit. The split is for reading. Two collectors
// and their two verifiers came to 29% of a 2900 line file, and a reader looking for
// `gc` landed in whichever one happened to come first.
//
// Selected by MINILISP_GC == MINILISP_GC_COPYING.
//======================================================================


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
    if (offset < 0 || s_heap_requested <= (size_t)offset)
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
    void *space = malloc(s_heap_requested);
    if (space != NULL)
        STATS_SYSTEM_ADD(s_heap_requested);
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
    // This is a copying collector, so a collection needs a second heap the same size as
    // the one already held. On a fragmented heap that request can fail even when the
    // total free memory looks sufficient, because it must be contiguous. Before this check, the collector copied into the NULL it got back.
    void *to_space = alloc_semispace();
    if (to_space == NULL) {
        // gc_running is cleared by error() before it unwinds.
        error("Out of memory: cannot allocate a GC semi-space of %u bytes",
              (unsigned)s_heap_requested);
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
    STATS_SYSTEM_SUB(s_heap_requested);
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
    lisp_stats.free_largest = s_heap_requested - live;
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
        STATS_SYSTEM_SUB(s_heap_requested);
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
    if (!always_gc && s_heap_requested < mem_nused + size)
        gc(root);

    // Give up if the request still cannot be satisfied, which happens when it is larger than the
    // heap or when the from-space held too many live objects. This collector compacts, so what
    // is free is one run and its size is the whole story.
    if (s_heap_requested < mem_nused + size)
        error("Memory exhausted: %u wanted, %u free",
              (unsigned)size, (unsigned)(s_heap_requested - mem_nused));

    Obj *obj = (Obj *)((char *)memory + mem_nused);
    mem_nused += size;
    obj->size = (uint16_t)size;
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
        if ((size_t)obj->size < offsetof(Obj, value))
            gc_verify_fail("GC verify: implausible size %u at offset %u",
                           (unsigned)obj->size, (unsigned)offset);
        if ((size_t)obj->size % sizeof(void *) != 0)
            gc_verify_fail("GC verify: misaligned size %u at offset %u",
                           (unsigned)obj->size, (unsigned)offset);
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
