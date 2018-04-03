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

#include <stdio.h>
#include <stdlib.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <X11/X.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xos.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>

#include "lwm.h"

const char *font_name;       /* User's choice of titlebar font. */
const char *popup_font_name; /* User's choice of menu font. */
const char *btn1_command;    /* User's choice of button 1 command. */
const char *btn2_command;    /* User's choice of button 2 command. */
int border;            /* User's choice of border size. */
FocusMode focus_mode;  /* User's choice of focus mode (default enter) */

char *sdup(char *p) {
  char *s = malloc(strlen(p) + 1);
  if (s == 0) {
    panic("malloc failed.");
  }
  return strcpy(s, p);
}

static const char *getResource(XrmDatabase *db, const char *name,
                               const char *cls, const char *const dflt) {
  char *type;
  XrmValue value;
  if (XrmGetResource(*db, name, cls, &type, &value) == True) {
    if (strcmp(type, "String") == 0) {
      return sdup((char *)value.addr);
    }
  }
  return dflt;
}

extern void parseResources(void) {
  // Set our fall-back defaults.
  font_name = DEFAULT_TITLE_FONT;
  popup_font_name = DEFAULT_POPUP_FONT;
  border = DEFAULT_BORDER;
  btn1_command = 0;
  btn2_command = DEFAULT_TERMINAL;
  focus_mode = focus_enter;

  char *resource_manager = XResourceManagerString(dpy);
  if (resource_manager == 0) {
    return;
  }

  XrmInitialize();
  XrmDatabase db = XrmGetStringDatabase(resource_manager);
  if (db == 0) {
    return;
  }
  
  // Simple string resources.
  font_name = getResource(&db, "lwm.titleFont", "Font", DEFAULT_TITLE_FONT);
  popup_font_name = getResource(&db, "lwm.popupFont", "Font", DEFAULT_POPUP_FONT);
  btn1_command = getResource(&db, "lwm.button1", "Command", NULL);
  btn2_command = getResource(&db, "lwm.button2", "Command", DEFAULT_TERMINAL);
  
  // Resources that require some interpretation.
  const char* focus = getResource(&db, "lwm.focus", "FocusMode", "enter");
  if (!strcmp(focus, "click")) {
    focus_mode = focus_click;
  }
  const char* brdr = getResource(&db, "lwm.border", "Border", NULL);
  if (brdr) {
    border = (int)strtol(brdr, (char **)0, 0);
  }
}
