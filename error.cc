/*
 * lwm, a window manager for X11
 * Copyright (C) 1997-2016 Elliott Hughes, James Carter
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#include "lwm.h"

int ignore_badwindow;

void panic(const char *s) {
  fprintf(stderr, "%s: %s\n", argv0, s);
  exit(EXIT_FAILURE);
}

int errorHandler(Display *d, XErrorEvent *e) {
  char msg[80];
  char req[80];
  char number[80];

  if (is_initialising && e->request_code == X_ChangeWindowAttributes &&
      e->error_code == BadAccess) {
    panic("another window manager is already running.");
  }

  if (ignore_badwindow &&
      (e->error_code == BadWindow || e->error_code == BadColor)) {
    return 0;
  }

  XGetErrorText(d, e->error_code, msg, sizeof(msg));
  sprintf(number, "%d", e->request_code);
  XGetErrorDatabaseText(d, "XRequest", number, number, req, sizeof(req));

  fprintf(stderr, "%s: protocol request %s on resource %#x failed: %s\n", argv0,
          req, (unsigned int)e->resourceid, msg);

  if (is_initialising) {
    panic("can't initialise.");
  }

  return 0;
}
