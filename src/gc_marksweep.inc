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


// Garbage collector: mark-sweep
//
// Included by libminilisp.c, not compiled on its own: everything here reaches the rest
// of the interpreter through file-level statics -- the literals, the symbol table, the
// root chain, error() -- and making it a translation unit of its own would mean giving
// all of that external linkage to no benefit. The split is for reading. Two collectors
// and their two verifiers came to 29% of a 2900 line file, and a reader looking for
// `gc` landed in whichever one happened to come first.
//
// Selected by MINILISP_GC == MINILISP_GC_MARK_SWEEP. This is the default.
//======================================================================


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
//
// That last one is what invites a compacting collector, and it was measured rather than
// assumed. Soak runs totalling some nineteen thousand collections, on a host and on an
// ESP8266, settle within a few hundred collections and then never drift: sweep rebuilds
// the free list from scratch every time, merging neighbours, so the heap keeps no memory
// of its own history and its shape follows where the live data is. The worst largest
// free block seen on target was 692 bytes, against a largest possible object of 72 -- a
// symbol at SYMBOL_MAX_LEN.
//
// Worth revisiting if a variable-length type with large values ever arrives, real strings
// or a much larger SYMBOL_MAX_LEN, because that margin is what makes this safe. A device
// reporting "Memory exhausted" with a large free total and a small largest block is the
// same thing said out loud.

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
    block->size = (uint16_t)size;
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
    // One free block has to be able to span the whole heap, so the heap cannot be longer
    // than the size field can count. lisp_create() refuses such a request before getting
    // here; this is the backstop for anyone setting MEMORY_SIZE directly.
    if (heap_size < MS_MIN_BLOCK || heap_size > LISP_MAX_HEAP)
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
    block->size = (uint16_t)heap_size;
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
#if MINILISP_GC_STATS
        lisp_stats.alloc_hops++;
#endif
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
            remainder->size = (uint16_t)rest;
            ms_set_next_free(remainder, next);
            next = remainder;
            block->size = (uint16_t)size;
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
        if ((size_t)obj->size < MS_MIN_BLOCK)
            gc_verify_fail("GC verify: implausible size %u at offset %u",
                           (unsigned)obj->size, (unsigned)offset);
        if ((size_t)obj->size % sizeof(void *) != 0)
            gc_verify_fail("GC verify: misaligned size %u at offset %u",
                           (unsigned)obj->size, (unsigned)offset);
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
