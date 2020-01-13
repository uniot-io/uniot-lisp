/*
 * This is a part of the Uniot project. The following is the user apps interpreter.
 * Copyright (C) 2019-2020 Uniot <info.uniot@gmail.com>
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

#include "libminilisp.h"
#include "emscripten.h"

const char json_mask_result[] = "{ \"out\": %s, \"states\": %s, \"err\": %s, \"meta\": { \"memory\": { \"init\": %lu, \"library\": %lu, \"total\": %lu }, \"library\": \"%s\", \"time\": %.2g } }";
const char json_mask_err[] = "{ \"msg\": \"%s\", \"idx\": %d }";
const char json_mask_state[] = "{ \"ask\": \"%s\", \"answer\": %d }";

char json_buf_out[102400] = {0};
int json_buf_out_idx = 0;

char json_buf_states[102400] = {0};
int json_buf_states_idx = 0;

char json_buf_err[256] = {0};
int json_buf_err_idx = 0;

size_t mem_used_init = 0;
size_t mem_used_by_library = 0;
size_t mem_used_total = 0;

// param msg must be null-terminated
EM_JS(int, js_handle_lisp, (const char *msg), {
    return Asyncify.handleSleep(wake_up => {
        const value = AsciiToString(msg);
        const lisp_handler = Module.lisp_handler;
        if (lisp_handler && lisp_handler.constructor.name === 'Function') {
            lisp_handler(value, wake_up);
        } else {
            wake_up(0);
        }
    });
})

static inline void mempush(char *buf, int *ptr, const char *value, int size)
{
    memcpy(buf + *ptr, value, size);
    *ptr += size;
}

void print_out(const char *msg, int size)
{
    mempush(json_buf_out, &json_buf_out_idx, "\"", 1);
    mempush(json_buf_out, &json_buf_out_idx, msg, size);
    mempush(json_buf_out, &json_buf_out_idx, "\"", 1);
    mempush(json_buf_out, &json_buf_out_idx, ",", 1);
}

// param msg must be null-terminated
void print_state(const char *msg, int result)
{
    char buf[SYMBOL_MAX_LEN];
    int size = sprintf(buf, json_mask_state, msg, result);
    mempush(json_buf_states, &json_buf_states_idx, buf, size);
    mempush(json_buf_states, &json_buf_states_idx, ",", 1);
}

int js_handle_state(const char *msg) {
    int result = js_handle_lisp(msg);
    print_state(msg, result);
    return result;
}

void js_handle_state_task(int times, int ms, int pass)
{
    char buf[SYMBOL_MAX_LEN];
    sprintf(buf, "task %d %d %d", times, ms, pass);
    js_handle_state(buf);
}

void print_err(const char *msg, int size)
{
    sprintf(json_buf_err, json_mask_err, msg, lisp_error_idx());
}

static void attach_task(void *root, struct Obj **env, int ms, int times)
{
    DEFINE2(t_obj, t_pass);
    *t_pass = get_variable(root, env, "#t_pass");
    *t_obj = get_variable(root, env, "#t_obj")->cdr;

    if (times > 0)
    {
        for (int t = times; t >= 0; --t)
        {
            // TODO: disallow endless loops
            (*t_pass)->cdr->value = t;
            eval(root, env, t_obj);
            js_handle_state_task(times, ms, t);
        }
    }
    else
    {
        (*t_pass)->cdr->value = -1;
        eval(root, env, t_obj);
    }
}

static struct Obj *prim_task(void *root, struct Obj **env, struct Obj **list)
{
    Obj *args = eval_list(root, env, list);
    if (length(args) != 3)
        error("Malformed task");

    Obj *times = args->car;
    Obj *ms = args->cdr->car;
    Obj *obj = args->cdr->cdr->car;

    // TODO: check types

    DEFINE1(t_obj);
    *t_obj = get_variable(root, env, "#t_obj");
    (*t_obj)->cdr = obj;

    attach_task(root, env, ms->value, times->value);
    return True;
}

static struct Obj *prim_tojs(void *root, struct Obj **env, struct Obj **list)
{
    Obj *args = eval_list(root, env, list);

    char buf[SYMBOL_MAX_LEN];
    int size = print_to_buf(buf, 0, args);
    buf[size - 1] = 0; // do not display ")"
    char *msg = buf + 1; // do not display "("

    return make_int(root, js_handle_state(msg));
}

// (defjs <symbol> (<symbol> ...))
static Obj *prim_defjs(void *root, Obj **env, Obj **list)
{
    return handle_pruner(root, env, list, "tojs", true);
}

static void define_custom_items(void *root, struct Obj **env)
{
    add_primitive(root, env, "defjs", prim_defjs);
    add_primitive(root, env, "tojs", prim_tojs);

    add_primitive(root, env, "task", prim_task);
    add_constant(root, env, "#t_obj", &Nil);
    add_constant_int(root, env, "#t_pass", 0);
}

static bool lisp_shoot_once(size_t max_heap, const char *library, const char *input)
{
    void *root = NULL;
    DEFINE1(env);
    
    lisp_create(max_heap);

    *env = make_env(root, &Nil, &Nil);
    define_constants(root, env);
    define_primitives(root, env);
    define_custom_items(root, env);

    mem_used_init = lisp_mem_used();
    lisp_set_printers(NULL, NULL);
    lisp_eval(root, env, library);
    mem_used_by_library = lisp_mem_used();
    lisp_set_printers(print_out, print_err);
    bool success = lisp_eval(root, env, input);
    mem_used_total = lisp_mem_used();

    lisp_destroy();

    return success;
}

EMSCRIPTEN_KEEPALIVE
int version()
{
    return LIB_VERSION;
}

EMSCRIPTEN_KEEPALIVE
int lisp_evaluate(size_t max_heap, const char *library, const char *input, char *output)
{
    memset(json_buf_out, 0, sizeof(json_buf_out));
    memset(json_buf_states, 0, sizeof(json_buf_states));
    memset(json_buf_err, 0, sizeof(json_buf_err));
    memcpy(json_buf_err, "null", 5);
    json_buf_out_idx = 0;
    json_buf_states_idx = 0;
    json_buf_err_idx = 0;

    bool success = false;
    double time_taken = 0;

    if (max_heap >= 2000) {
        mempush(json_buf_out, &json_buf_out_idx, "[", 1);
        mempush(json_buf_states, &json_buf_states_idx, "[", 1);
        double time_started = emscripten_get_now();
        success = lisp_shoot_once(max_heap, library, input);
        time_taken = emscripten_get_now() - time_started;
        if (',' == json_buf_out[json_buf_out_idx - 1])
            json_buf_out_idx--;
        if (',' == json_buf_states[json_buf_states_idx - 1])
            json_buf_states_idx--;
        mempush(json_buf_out, &json_buf_out_idx, "]", 1);
        mempush(json_buf_states, &json_buf_states_idx, "]", 1);
    } else {
        memcpy(json_buf_out, "[]", 3);
        memcpy(json_buf_states, "[]", 3);
        sprintf(json_buf_err, json_mask_err, "Heap must be at least 2000 bytes", 0);
    }

    int output_size = sprintf(output, json_mask_result, json_buf_out, json_buf_states, json_buf_err, mem_used_init, mem_used_by_library, mem_used_total, library, time_taken);

    int factor = success ? 1 : -1;
    return output_size * factor;
}
