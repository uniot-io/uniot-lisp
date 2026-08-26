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

// Line editing for the REPL: history, cursor movement, and the editing keys a shell
// user already has in their fingers. Written out rather than linked against readline
// because the rest of this repository builds with a bare `cc` and no packages, and a
// development convenience is not worth making that untrue.
//
// Only used when both ends are a terminal. Piped input -- the test suite, and running
// an example from a file -- falls through to plain reads, so nothing about the way this
// program behaves non-interactively depends on any of it.

#ifndef LINE_EDIT_H
#define LINE_EDIT_H

#include <stdbool.h>

// Reads one line. Returns it without its newline, allocated for the caller to free, or
// NULL at end of input. `prompt` is written only when interactive.
char *line_edit_read(const char *prompt);

// Remembers a line for the up arrow to find. Blank lines and immediate repeats are
// dropped, as a shell does.
void line_edit_add_history(const char *line);

// Loads and saves the history file, so a session starts where the last one left off.
// Failure is silent: no history is a smaller problem than refusing to start.
void line_edit_load_history(const char *path);
void line_edit_save_history(const char *path);

// Frees the history. The terminal is restored by an atexit handler installed on first
// use, so there is nothing to undo here.
void line_edit_free(void);

// True when reading will actually edit, which is what tells the caller whether printing
// a prompt or a banner makes any sense.
bool line_edit_interactive(void);

#endif
