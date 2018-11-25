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

static Cursor colouredCursor(Display* dpy,
                             unsigned int shape,
                             XColor* fg,
                             XColor* bg) {
  Cursor res = XCreateFontCursor(dpy, shape);
  XRecolorCursor(dpy, res, fg, bg);
  return res;
}

CursorMap::CursorMap(Display* dpy) {
  XColor red, white, exact;
  Colormap cmp = DefaultColormap(dpy, 0);  // 0 = screen index 0.
  XAllocNamedColor(dpy, cmp, "red", &red, &exact);
  XAllocNamedColor(dpy, cmp, "white", &white, &exact);
  root_ = colouredCursor(dpy, XC_left_ptr, &red, &white);
  box_ = colouredCursor(dpy, XC_draped_box, &red, &white);

#define MC(e, s) edges_[e] = colouredCursor(dpy, s, &red, &white)
  MC(ETopLeft, XC_top_left_corner);
  MC(ETop, XC_top_side);
  MC(ETopRight, XC_top_right_corner);
  MC(ERight, XC_right_side);
  MC(ENone, XC_fleur);
  MC(ELeft, XC_left_side);
  MC(EBottomLeft, XC_bottom_left_corner);
  MC(EBottom, XC_bottom_side);
  MC(EBottomRight, XC_bottom_right_corner);
#undef MC
}

Cursor CursorMap::ForEdge(Edge e) const {
  auto it = edges_.find(e);
  return (it == edges_.end()) ? Root() : it->second;
}
