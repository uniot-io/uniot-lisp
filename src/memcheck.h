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

#ifndef MEMCHECK_H
#define MEMCHECK_H

#include <stdlib.h>

#if __EMSCRIPTEN__
#define TOP_RESERVE_FACTOR 2
#elif ESP8266
#define TOP_RESERVE_FACTOR 2
#elif ESP32
#define TOP_RESERVE_FACTOR 2
#else
#define TOP_RESERVE_FACTOR 1
#endif

static char origin_of_stack_var;
static void *origin_ptr = &origin_of_stack_var;

static inline void set_origin_ptr(void *origin)
{
  origin_ptr = origin;
}

static inline int is_valid_ptr(void *p)
{
  void *top_ptr = malloc(1);
  free(top_ptr);
  unsigned long origin_ptr_val = (unsigned long)origin_ptr;
  unsigned long p_val = (unsigned long)p;
  unsigned long top_ptr_val = (unsigned long)top_ptr * TOP_RESERVE_FACTOR;
  // printf("%lu < %lu < %lu\n", origin_ptr_val, p_val, top_ptr_val);
  return (p_val > origin_ptr_val && p_val < top_ptr_val);
}

#endif // MEMCHECK_H
