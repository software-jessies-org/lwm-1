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

#include "lwm.h"

struct CursorMapping {
  Edge edge;
  int font_char;
};

static CursorMapping cursor_map[] = {
    {ETopLeft, XC_top_left_corner},
    {ETop, XC_top_side},
    {ETopRight, XC_top_right_corner},
    {ERight, XC_right_side},
    {ENone, XC_fleur},
    {ELeft, XC_left_side},
    {EBottomLeft, XC_bottom_left_corner},
    {EBottom, XC_bottom_side},
    {EBottomRight, XC_bottom_right_corner},

    {ENone, 0},
};

extern void initialiseCursors() {
  XColor red, white, exact;
  Colormap cmp = DefaultColormap(dpy, 0);  // 0 = screen index 0.

  XAllocNamedColor(dpy, cmp, "red", &red, &exact);
  XAllocNamedColor(dpy, cmp, "white", &white, &exact);

  screen->root_cursor = XCreateFontCursor(dpy, XC_left_ptr);
  XRecolorCursor(dpy, screen->root_cursor, &red, &white);

  screen->box_cursor = XCreateFontCursor(dpy, XC_draped_box);
  XRecolorCursor(dpy, screen->box_cursor, &red, &white);

  for (int i = 0; cursor_map[i].font_char != 0; i++) {
    Edge e = cursor_map[i].edge;
    screen->cursor_map[e] = XCreateFontCursor(dpy, cursor_map[i].font_char);
    XRecolorCursor(dpy, screen->cursor_map[e], &red, &white);
  }
}

extern Cursor getEdgeCursor(Edge edge) { return screen->cursor_map[edge]; }
