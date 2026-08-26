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

#include "line_edit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define HISTORY_MAX 512

static char *history[HISTORY_MAX];
static int history_len = 0;

static struct termios saved_termios;
static bool raw_installed = false;

// The line being edited, and where the cursor sits in it.
typedef struct {
    char *buf;
    size_t len, cap, pos;
} Line;

// Everything this file puts on the terminal goes through here. The rest of the program
// prints with stdio, which buffers; this writes to the descriptor, which does not. Left
// to themselves the two arrive out of order -- a colour reset still sitting in the stdio
// buffer lands after the prompt has been drawn, and paints the prompt with it.
static void emit(const char *bytes, size_t len) {
    fflush(stdout);
    if (write(STDOUT_FILENO, bytes, len) < 0)
        return;
}

//======================================================================
// Terminal mode
//======================================================================

static void disable_raw(void) {
    if (raw_installed) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_termios);
        raw_installed = false;
    }
}

// Raw mode lasts for one read and no longer. Holding it across evaluation would leave
// OPOST off while the interpreter prints, and without it a newline moves down a line
// without returning to the left margin -- every line of output would start one column
// further right than the one before it.
//
// The atexit handler is registered once and covers the paths that never come back here.
static bool enable_raw(void) {
    static bool atexit_installed = false;

    if (raw_installed)
        return true;
    if (tcgetattr(STDIN_FILENO, &saved_termios) == -1)
        return false;

    struct termios raw = saved_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        return false;

    raw_installed = true;
    if (!atexit_installed) {
        atexit(disable_raw);
        atexit_installed = true;
    }
    return true;
}

bool line_edit_interactive(void) {
    return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
}

//======================================================================
// The line buffer
//======================================================================

static bool line_reserve(Line *l, size_t need) {
    if (need + 1 <= l->cap)
        return true;
    size_t cap = l->cap ? l->cap : 64;
    while (cap < need + 1)
        cap *= 2;
    char *grown = realloc(l->buf, cap);
    if (!grown)
        return false;
    l->buf = grown;
    l->cap = cap;
    return true;
}

static void line_insert(Line *l, char c) {
    if (!line_reserve(l, l->len + 1))
        return;
    memmove(l->buf + l->pos + 1, l->buf + l->pos, l->len - l->pos);
    l->buf[l->pos++] = c;
    l->buf[++l->len] = '\0';
}

static void line_delete_at(Line *l, size_t at) {
    if (at >= l->len)
        return;
    memmove(l->buf + at, l->buf + at + 1, l->len - at - 1);
    l->buf[--l->len] = '\0';
}

static void line_set(Line *l, const char *text) {
    const size_t n = strlen(text);
    if (!line_reserve(l, n))
        return;
    memcpy(l->buf, text, n + 1);
    l->len = l->pos = n;
}

//======================================================================
// Drawing
//======================================================================

// Redraws the whole line and puts the cursor back where it belongs. One write, so the
// line never appears half-updated. Long lines are not scrolled: they wrap, which is
// what a terminal does anyway and is honest about how much has been typed.
static void refresh(const char *prompt, const Line *l) {
    char seq[64];
    const size_t prompt_len = strlen(prompt);

    emit("\r", 1);
    emit(prompt, prompt_len);
    emit(l->buf, l->len);
    emit("\x1b[0K", 4);

    const int n = snprintf(seq, sizeof(seq), "\r\x1b[%uC", (unsigned)(prompt_len + l->pos));
    emit(seq, (size_t)n);
}

//======================================================================
// History
//======================================================================

void line_edit_add_history(const char *line) {
    if (!line)
        return;

    const char *p = line;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '\0')
        return;
    if (history_len > 0 && strcmp(history[history_len - 1], line) == 0)
        return;

    char *copy = strdup(line);
    if (!copy)
        return;
    if (history_len == HISTORY_MAX) {
        free(history[0]);
        memmove(history, history + 1, sizeof(char *) * (HISTORY_MAX - 1));
        history_len--;
    }
    history[history_len++] = copy;
}

void line_edit_load_history(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, f)) > 0) {
        if (line[n - 1] == '\n')
            line[n - 1] = '\0';
        line_edit_add_history(line);
    }
    free(line);
    fclose(f);
}

void line_edit_save_history(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    for (int i = 0; i < history_len; i++)
        fprintf(f, "%s\n", history[i]);
    fclose(f);
}

void line_edit_free(void) {
    for (int i = 0; i < history_len; i++)
        free(history[i]);
    history_len = 0;
}

//======================================================================
// Reading
//======================================================================

// Reads a line with no editing at all: not a terminal, so there is no cursor to move
// and nothing that would understand an escape sequence if we sent one.
static char *read_plain(void) {
    char *line = NULL;
    size_t cap = 0;
    const ssize_t n = getline(&line, &cap, stdin);
    if (n < 0) {
        free(line);
        return NULL;
    }
    if (n > 0 && line[n - 1] == '\n')
        line[n - 1] = '\0';
    return line;
}

enum { KEY_CTRL_A = 1, KEY_CTRL_B = 2, KEY_CTRL_C = 3, KEY_CTRL_D = 4, KEY_CTRL_E = 5,
       KEY_CTRL_F = 6, KEY_BACKSPACE_ALT = 8, KEY_TAB = 9, KEY_CTRL_K = 11, KEY_CTRL_L = 12,
       KEY_ENTER = 13, KEY_CTRL_N = 14, KEY_CTRL_P = 16, KEY_CTRL_T = 20, KEY_CTRL_U = 21,
       KEY_CTRL_W = 23, KEY_ESC = 27, KEY_BACKSPACE = 127 };

// Walks the history in place. `index` is counted back from the end, 0 meaning the line
// the user was typing, which is kept so that arrowing away and back does not lose it.
static void history_move(Line *l, int *index, char **pending, int delta) {
    if (history_len == 0)
        return;

    if (*index == 0 && delta > 0) {
        free(*pending);
        *pending = strdup(l->buf);
    }

    int next = *index + delta;
    if (next < 0)
        next = 0;
    if (next > history_len)
        next = history_len;
    if (next == *index)
        return;
    *index = next;

    line_set(l, *index == 0 ? (*pending ? *pending : "") : history[history_len - *index]);
}

char *line_edit_read(const char *prompt) {
    if (!line_edit_interactive())
        return read_plain();
    if (!enable_raw())
        return read_plain();

    Line l = {NULL, 0, 0, 0};
    if (!line_reserve(&l, 0))
        return NULL;
    l.buf[0] = '\0';

    int hist_index = 0;
    char *pending = NULL;
    refresh(prompt, &l);

    for (;;) {
        char c;
        const ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) {
            disable_raw();
            free(l.buf);
            free(pending);
            return NULL;
        }

        switch (c) {
        case KEY_ENTER:
            emit("\r\n", 2);
            disable_raw();
            free(pending);
            return l.buf;

        case KEY_CTRL_C:
            // Abandons the line, as it does in a shell, and leaves it visible above so
            // it can be read back. The caller sees an empty line, not end of input.
            emit("^C\r\n", 4);
            disable_raw();
            l.buf[0] = '\0';
            free(pending);
            return l.buf;

        case KEY_CTRL_D:
            // End of input only on an empty line, which is the usual rule. With text to
            // the right of the cursor it deletes a character instead.
            if (l.len == 0) {
                emit("\r\n", 2);
                disable_raw();
                free(l.buf);
                free(pending);
                return NULL;
            }
            line_delete_at(&l, l.pos);
            break;

        case KEY_BACKSPACE:
        case KEY_BACKSPACE_ALT:
            if (l.pos > 0)
                line_delete_at(&l, --l.pos);
            break;

        case KEY_CTRL_A:
            l.pos = 0;
            break;
        case KEY_CTRL_E:
            l.pos = l.len;
            break;
        case KEY_CTRL_B:
            if (l.pos > 0)
                l.pos--;
            break;
        case KEY_CTRL_F:
            if (l.pos < l.len)
                l.pos++;
            break;

        case KEY_CTRL_K:
            l.len = l.pos;
            l.buf[l.len] = '\0';
            break;

        case KEY_CTRL_U:
            memmove(l.buf, l.buf + l.pos, l.len - l.pos + 1);
            l.len -= l.pos;
            l.pos = 0;
            break;

        case KEY_CTRL_W: {
            const size_t end = l.pos;
            while (l.pos > 0 && l.buf[l.pos - 1] == ' ')
                l.pos--;
            while (l.pos > 0 && l.buf[l.pos - 1] != ' ')
                l.pos--;
            memmove(l.buf + l.pos, l.buf + end, l.len - end + 1);
            l.len -= end - l.pos;
            break;
        }

        case KEY_CTRL_T:
            // Swaps the two characters before the cursor, which is how a typo of two
            // transposed letters is usually undone.
            if (l.pos > 1) {
                const char tmp = l.buf[l.pos - 2];
                l.buf[l.pos - 2] = l.buf[l.pos - 1];
                l.buf[l.pos - 1] = tmp;
            }
            break;

        case KEY_CTRL_P:
            history_move(&l, &hist_index, &pending, 1);
            break;
        case KEY_CTRL_N:
            history_move(&l, &hist_index, &pending, -1);
            break;

        case KEY_CTRL_L:
            emit("\x1b[H\x1b[2J", 7);
            break;

        case KEY_TAB:
            // Indentation, not completion. There is nothing to complete against that
            // would be worth the surprise of a key that sometimes does nothing.
            line_insert(&l, ' ');
            line_insert(&l, ' ');
            break;

        case KEY_ESC: {
            char seq[3];
            if (read(STDIN_FILENO, &seq[0], 1) != 1 || read(STDIN_FILENO, &seq[1], 1) != 1)
                break;
            if (seq[0] != '[' && seq[0] != 'O')
                break;

            if (seq[1] >= '0' && seq[1] <= '9') {
                // An extended sequence: ESC [ n ~, which is how Home, End and Delete
                // arrive from some terminals.
                if (read(STDIN_FILENO, &seq[2], 1) != 1 || seq[2] != '~')
                    break;
                if (seq[1] == '3')
                    line_delete_at(&l, l.pos);
                else if (seq[1] == '1' || seq[1] == '7')
                    l.pos = 0;
                else if (seq[1] == '4' || seq[1] == '8')
                    l.pos = l.len;
                break;
            }

            switch (seq[1]) {
            case 'A':
                history_move(&l, &hist_index, &pending, 1);
                break;
            case 'B':
                history_move(&l, &hist_index, &pending, -1);
                break;
            case 'C':
                if (l.pos < l.len)
                    l.pos++;
                break;
            case 'D':
                if (l.pos > 0)
                    l.pos--;
                break;
            case 'H':
                l.pos = 0;
                break;
            case 'F':
                l.pos = l.len;
                break;
            }
            break;
        }

        default:
            // Anything else printable goes in. Control characters that got this far are
            // dropped rather than inserted, where they would corrupt the display.
            if ((unsigned char)c >= 32)
                line_insert(&l, c);
            break;
        }

        refresh(prompt, &l);
    }
}
