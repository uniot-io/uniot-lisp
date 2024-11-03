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

#define MIN_HEAP_SIZE   2000
#define BUF_OUT_SIZE    10485760 // 10 MB
#define BUF_STATES_SIZE (BUF_OUT_SIZE - 2048)
#define BUF_ERR_SIZE    512
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
bool global_task_terminator = false;

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

int escape_json_string(const char *input, char *output, int max_output_size)
{
    int j = 0;
    for (int i = 0; input[i] != '\0'; ++i) {
        if (j >= max_output_size - 2) { // Reserve space for null terminator
            return -1; // Not enough space
        }
        switch (input[i]) {
            case '\"': memcpy(output + j, "\\\"", 2); j += 2; break;
            case '\\': memcpy(output + j, "\\\\", 2); j += 2; break;
            case '\b': memcpy(output + j, "\\b", 2); j += 2; break;
            case '\f': memcpy(output + j, "\\f", 2); j += 2; break;
            case '\n': memcpy(output + j, "\\n", 2); j += 2; break;
            case '\r': memcpy(output + j, "\\r", 2); j += 2; break;
            case '\t': memcpy(output + j, "\\t", 2); j += 2; break;
            default:
                if ((unsigned char)input[i] < 0x20) {
                    // Encode control characters as \uXXXX
                    if (j + 6 > max_output_size) return -1;
                    sprintf(output + j, "\\u%04x", input[i]);
                    j += 6;
                } else {
                    output[j++] = input[i];
                }
        }
    }
    output[j] = '\0';
    return j;
}

static inline bool mempush(char *buf, int *ptr, const char *value, int size, int buf_size)
{
    if (*ptr + size > buf_size) {
        // Handle buffer overflow, e.g., truncate, log error, etc.
        return false;
    }
    memcpy(buf + *ptr, value, size);
    *ptr += size;
    return true;
}

void print_err(const char *msg, int size)
{
    char escaped_msg[BUF_ERR_SIZE - 100]; // Adjust size based on `json_mask_err`
    int escaped_size = escape_json_string(msg, escaped_msg, sizeof(escaped_msg));
    if (escaped_size < 0) {
        snprintf(json_buf_err, BUF_ERR_SIZE, json_mask_err, "Failed to escape error message", 0);
        return;
    }

    int formatted_size = snprintf(json_buf_err, BUF_ERR_SIZE, json_mask_err, escaped_msg, lisp_error_idx());
    if (formatted_size < 0 || formatted_size >= BUF_ERR_SIZE) {
        snprintf(json_buf_err, BUF_ERR_SIZE, json_mask_err, "Error message too long", lisp_error_idx());
    }
}

void print_out(const char *msg, int size)
{
    char escaped_msg[SYMBOL_MAX_LEN * 2];
    int escaped_size = escape_json_string(msg, escaped_msg, sizeof(escaped_msg));
    if (escaped_size < 0) {
        print_err("Failed to escape JSON string in print_out", 0);
        return;
    }

    if (!mempush(json_buf_out, &json_buf_out_idx, "\"", 1, sizeof(json_buf_out))) return;
    if (!mempush(json_buf_out, &json_buf_out_idx, escaped_msg, escaped_size, sizeof(json_buf_out))) return;
    if (!mempush(json_buf_out, &json_buf_out_idx, "\"", 1, sizeof(json_buf_out))) return;
    if (!mempush(json_buf_out, &json_buf_out_idx, ",", 1, sizeof(json_buf_out))) return;
}

// param msg must be null-terminated
void print_state(const char *msg, const char *result)
{
    char escaped_msg[SYMBOL_MAX_LEN * 2];
    char escaped_result[SYMBOL_MAX_LEN * 2];
    int escaped_size_msg = escape_json_string(msg, escaped_msg, sizeof(escaped_msg));
    int escaped_size_result = escape_json_string(result, escaped_result, sizeof(escaped_result));

    if (escaped_size_msg < 0 || escaped_size_result < 0) {
        print_err("Failed to escape JSON string in print_state", 0);
        return;
    }

    char buf[SYMBOL_MAX_LEN * 2];
    int size = snprintf(buf, sizeof(buf), json_mask_state, escaped_msg, escaped_result);
    if (size < 0 || size >= sizeof(buf)) {
        print_err("Buffer overflow in print_state", 0);
        return;
    }

    if (!mempush(json_buf_states, &json_buf_states_idx, buf, size, sizeof(json_buf_states))) return;
    if (!mempush(json_buf_states, &json_buf_states_idx, ",", 1, sizeof(json_buf_states))) return;
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

static void attach_task(void *root, struct Obj **env, int ms, int times)
{
    DEFINE2(t_obj, t_pass);
    *t_pass = get_variable(root, env, "#t_pass");
    *t_obj = get_variable(root, env, "#t_obj")->cdr;

    if (times > 0)
    {
        times -= 1;
        times = times > global_task_limiter ? global_task_limiter : times;
        for (int t = times; t >= 0; --t)
        {
            if (global_task_terminator) {
                break;
            }
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
            if (global_task_terminator) {
                break;
            }
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

    if (times->type != TINT || ms->type != TINT || obj->type != TCELL)
        error("Task expects (times ms obj) with (Int Int Cell) types");

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
    lisp_eval(root, env, "(defjs is_event (event)) (defjs pop_event (event)) (defjs push_event (event value))");

    mem_used_init = lisp_mem_used();
    lisp_set_printers(NULL, NULL, NULL);
    lisp_eval(root, env, library);
    mem_used_by_library = lisp_mem_used();
    lisp_set_printers(print_out, NULL, print_err);
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
void terminate()
{
    global_task_terminator = true;
}

EMSCRIPTEN_KEEPALIVE
int lisp_evaluate(size_t max_heap, const char *library, const char *input, char *output, int task_limiter)
{
    if (task_limiter > MAX_TASK_ITER) {
        task_limiter = MAX_TASK_ITER;
    }
    global_task_limiter = task_limiter;
    global_task_terminator = false;

    memset(json_buf_out, 0, sizeof(json_buf_out));
    memset(json_buf_states, 0, sizeof(json_buf_states));
    memset(json_buf_err, 0, sizeof(json_buf_err));
    memcpy(json_buf_err, "null", 5);
    json_buf_out_idx = 0;
    json_buf_states_idx = 0;
    json_buf_err_idx = 0;

    bool success = false;
    double time_taken = 0;

    if (max_heap >= MIN_HEAP_SIZE) {
        do {
            if (!mempush(json_buf_out, &json_buf_out_idx, "[", 1, sizeof(json_buf_out))) {
                strcpy(json_buf_out, "[]");
                strcpy(json_buf_states, "[]");
                snprintf(json_buf_err, BUF_ERR_SIZE, json_mask_err, "Output buffer overflow", 0);
                break;
            }
            if (!mempush(json_buf_states, &json_buf_states_idx, "[", 1, sizeof(json_buf_states))) {
                strcpy(json_buf_out, "[]");
                strcpy(json_buf_states, "[]");
                snprintf(json_buf_err, BUF_ERR_SIZE, json_mask_err, "States buffer overflow", 0);
                break;
            }

            double time_started = emscripten_get_now();
            success = lisp_shoot_once(max_heap, library, input);
            time_taken = emscripten_get_now() - time_started;

            // Remove trailing commas
            if (json_buf_out_idx > 0 && json_buf_out[json_buf_out_idx - 1] == ',')
                json_buf_out_idx--;
            if (json_buf_states_idx > 0 && json_buf_states[json_buf_states_idx - 1] == ',')
                json_buf_states_idx--;

            if (!mempush(json_buf_out, &json_buf_out_idx, "]", 1, sizeof(json_buf_out))) {
                strcpy(json_buf_out, "[]");
                strcpy(json_buf_states, "[]");
                snprintf(json_buf_err, BUF_ERR_SIZE, json_mask_err, "Output buffer overflow", 0);
                break;
            }
            if (!mempush(json_buf_states, &json_buf_states_idx, "]", 1, sizeof(json_buf_states))) {
                strcpy(json_buf_out, "[]");
                strcpy(json_buf_states, "[]");
                snprintf(json_buf_err, BUF_ERR_SIZE, json_mask_err, "States buffer overflow", 0);
                break;
            }
        } while (0);
    } else {
        strcpy(json_buf_out, "[]");
        strcpy(json_buf_states, "[]");
        snprintf(json_buf_err, BUF_ERR_SIZE, json_mask_err, "Heap must be at least 2000 bytes", 0);
    }

    // Safely format the final JSON output
    int output_size = snprintf(output, BUF_OUT_SIZE, json_mask_result, json_buf_out, json_buf_states, json_buf_err, mem_used_init, mem_used_by_library, mem_used_total, library, global_task_limiter, time_taken);
    if (output_size < 0 || output_size >= BUF_OUT_SIZE) {
        // Handle output buffer overflow
        return -1;
    }

    int factor = success ? 1 : -1;
    return output_size * factor;
}
