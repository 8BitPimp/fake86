/*
  Fake86: A portable, open-source 8086 PC emulator.
  Copyright (C)2010-2012 Mike Chambers

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
  USA.
*/

#include "common.h"

#include <stdarg.h>


static FILE *log_fd = NULL;

static const char *channel_name[] = {
  "[     ]  ",
  "[DISK ]  ",
  "[FRONT]  ",
  "[SDL  ]  ",
  "[CPU  ]  ",
  "[MEM  ]  ",
  "[VIDEO]  ",
};

void log_init() {
  log_fd = fopen(LOG_FNAME, "w");
  if (log_fd) {
    fprintf(log_fd, "(c)2019      Aidan Dodds\n");
    fprintf(log_fd, "(c)2010-2013 Mike Chambers\n");
    fprintf(log_fd, "[A portable, open-source 8086 PC emulator]\n");
    fprintf(log_fd, "build: %s\n\n", BUILD_STRING);
  }
}

void log_close() {
  if (log_fd) {
    fclose(log_fd);
    log_fd = NULL;
  }
}

void log_printf(int channel, const char *fmt, ...) {
  if (!log_fd) {
    log_init();
  }
  if (log_fd) {
    va_list vargs;
    va_start(vargs, fmt);
    fprintf(log_fd, channel_name[channel]);
    vfprintf(log_fd, fmt, vargs);
    fputc('\n', log_fd);
    va_end(vargs);
  }
}