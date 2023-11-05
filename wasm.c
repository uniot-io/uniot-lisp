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

#include "libminilisp.h"
#include "emscripten.h"

#define BUF_OUT_SIZE    10485760 // 10 MB
#define BUF_STATES_SIZE (BUF_OUT_SIZE - 2048)
#define BUF_ERR_SIZE    256
#define MAX_TASK_ITER   9999

const char json_mask_result[] = "{ \"out\": %s, \"states\": %s, \"err\": %s, \"meta\": { \"memory\": { \"init\": %lu, \"library\": %lu, \"total\": %lu }, \"library\": \"%s\", \"task_limit\": \"%d\", \"time\": %.2g } }";
const char json_mask_err[] = "{ \"msg\": \"%s\", \"idx\": %d }";
const char json_mask_state[] = "{ \"ask\": \"%s\", \"answer\": \"%s\" }";

char json_buf_out[BUF_OUT_SIZE] = {0};
int json_buf_out_idx = 0;

char json_buf_states[BUF_STATES_SIZE] = {0};
int json_buf_states_idx = 0;

char json_buf_err[BUF_ERR_SIZE] = {0};
int json_buf_err_idx = 0;

size_t mem_used_init = 0;
size_t mem_used_by_library = 0;
size_t mem_used_total = 0;

int global_task_limiter = MAX_TASK_ITER;

void console_log(const char *msg) {
    EM_ASM({ console.log(UTF8ToString($0)); }, msg);
}

// param msg must be null-terminated
EM_JS(const char*, js_handle_lisp, (const char *msg), {
    return Asyncify.handleSleep(wake_up => {
        const value = AsciiToString(msg);
        const lisp_handler = Module.lisp_handler;
        if (lisp_handler && lisp_handler.constructor.name === 'Function') {
            lisp_handler(value, wake_up);
        } else {
            wake_up("()");
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
void print_state(const char *msg, const char *result)
{
    char buf[SYMBOL_MAX_LEN];
    int size = sprintf(buf, json_mask_state, msg, result);
    mempush(json_buf_states, &json_buf_states_idx, buf, size);
    mempush(json_buf_states, &json_buf_states_idx, ",", 1);
}

const char* js_handle_state(const char *msg) {
    const char *result = js_handle_lisp(msg);
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
        times = times > global_task_limiter ? global_task_limiter : times;
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
        for (int t = global_task_limiter; t >= 0; --t)
        {
            eval(root, env, t_obj);
            js_handle_state_task(times, ms, t);
        }
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

    const char* result = js_handle_state(msg);

    int int_result = atoi(result);
    if (int_result == 0 && strcmp(result, "0") != 0) {
        if (strcmp(result, "#t") == 0) {
            return True;
        }
        if (strcmp(result, "()") == 0) {
            return Nil;
        }
        return make_symbol(root, result);
    }
    return make_int(root, int_result);
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

int str_replace(char *dest, int dest_size, char *orig, char *rep, char *with)
{
    int len_rep;   // length of rep (the string to remove)
    int len_with;  // length of with (the string to replace rep with)
    int len_front; // distance between rep and end of last rep
    int count;     // number of replacements

    // sanity checks and initialization
    if (!orig || !rep)
        return 0;
    len_rep = strlen(rep);
    if (len_rep == 0)
        return 0; // empty rep causes infinite loop during count
    if (!with)
        with = "";
    len_with = strlen(with);

    // count the number of replacements needed
    char *tmp = NULL;
    char *ins = orig;
    for (count = 0; (tmp = strstr(ins, rep)); ++count)
        ins = tmp + len_rep;

    int size = strlen(orig) + (len_with - len_rep) * count + 1;

    if (size > dest_size)
        return -1;

    tmp = dest;
    ins = NULL;
    while (count--)
    {
        ins = strstr(orig, rep);
        len_front = ins - orig;
        tmp = strncpy(tmp, orig, len_front) + len_front;
        tmp = strcpy(tmp, with) + len_with;
        orig += len_front + len_rep; // move to next "end of rep"
    }
    strcpy(tmp, orig);
    return size;
}

EMSCRIPTEN_KEEPALIVE
int lisp_evaluate(size_t max_heap, const char *library, const char *input, char *output, int task_limiter)
{
    if (task_limiter > MAX_TASK_ITER) {
        task_limiter = MAX_TASK_ITER;
    }
    global_task_limiter = task_limiter;

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

    char json_buf_err_tmp[sizeof(json_buf_err)] = {0};
    memcpy(json_buf_err_tmp, json_buf_err, sizeof(json_buf_err_tmp));
    int size = str_replace(json_buf_err, sizeof(json_buf_err), json_buf_err_tmp, "\\", "\\\\");
    if (size < 0)
        sprintf(json_buf_err, json_mask_err, "Internal error, contact the developers", 0);

    int output_size = sprintf(output, json_mask_result, json_buf_out, json_buf_states, json_buf_err, mem_used_init, mem_used_by_library, mem_used_total, library, global_task_limiter, time_taken);

    int factor = success ? 1 : -1;
    return output_size * factor;
}
