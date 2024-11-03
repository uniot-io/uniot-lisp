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

#define LISP_VERSION SEMVER_TO_INT(0, 2, 3)

#define SYMBOL_MAX_LEN 200

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

// Flags to debug GC
extern bool gc_running;
extern bool debug_gc;
extern bool always_gc;

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

int print_to_buf(char *buf, int pos, Obj *obj);

void print(Obj *obj);

void add_primitive(void *root, Obj **env, const char *name, Primitive *fn);

void add_constant(void *root, Obj **env, const char *name, Obj **val);

void add_constant_int(void *root, Obj **env, const char *name, int value);

Obj *get_variable(void *root, Obj **env, const char *name);

void lisp_create(size_t size);

void lisp_destroy(void);

bool lisp_is_created();

bool lisp_eval(void *root, Obj **env, const char *code);

bool safe_eval(void *root, Obj **env, Obj **expr);

void lisp_set_cycle_yield(yield_def yield);

void lisp_set_printers(print_def out, print_def log, print_def err);

size_t lisp_mem_used(void);

int lisp_error_idx(void);

Obj *handle_pruner(void *root, Obj **env, Obj **list, const char *handler_name, bool include_name);
#ifdef __cplusplus
}
#endif // __cplusplus

#endif // MINILISP_H
