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
#include <string.h>
#include <time.h>
#include <sstream>

#include <unistd.h>

#include "ewmh.h"
#include "lwm.h"
#include "xlib.h"

static int popup_width;  // The width of the size-feedback window.

static void sendClientMessage(Window, Atom, long, long);

// Returns the total height, in pixels, of the window title bar.
int titleBarHeight() {
  return textHeight() + borderWidth();
}

// closeBounds returns the bounding box of the close icon cross.
// If displayBounds is true, the returned box is the cross itself; if false,
// it's the active area (which extends down to the client window, and across
// to the start of the title bar).
// The reason for the difference is simple usability: particularly on large 4k
// displays, it's tricky to hit the cross itself, and easy to instead click on
// the area below and to the right of the cross, which would result in the
// window being resized. However, resizing from that position seems weird; one
// would more naturally pick the outer edge for such an action, so it makes
// more sense to have that close the window too.
Rect closeBounds(bool displayBounds) {
  const int quarter = (borderWidth() + textHeight()) / 4;
  const int cMin = quarter + 2;
  const int cMax = displayBounds ? 3 * quarter : titleBarHeight();
  return Rect{cMin, cMin, cMax, cMax};
}

Rect titleBarBounds(int windowWidth) {
  const int x = titleBarHeight();
  const int topB = topBorderWidth();
  const int w = windowWidth - 2 * x;
  return Rect{x, topB, w, titleBarHeight() - topB};
}

std::ostream& operator<<(std::ostream& os, const Client& c) {
  os << WinID(c.window);
  if (c.parent) {
    os << " (frame=" << WinID(c.parent) << ")";
  }
  if (c.trans) {
    os << " (trans=" << WinID(c.trans) << ")";
  }
  os << " outer=" << c.RectWithBorder() << " inner=" << c.RectNoBorder() << " ";
  if (c.hidden) {
    os << "(";
  }
  os << "\"" << c.Name() << "\"";
  if (c.hidden) {
    os << ")";
  }
  return os;
}

std::ostream& operator<<(std::ostream& os, const WinID& w) {
  os << "0x" << std::hex << w.w << std::dec;
  return os;
}

Rect Client::edgeBounds(Edge e) const {
  const int inset = titleBarHeight();
  const int wh = size.height + textHeight();
  Rect res{inset, inset, size.width - inset, wh - inset};
  if (isLeftEdge(e)) {
    res.xMin = 0;
    res.xMax = inset;
  } else if (isRightEdge(e)) {
    res.xMin = size.width - inset;
    res.xMax = size.width;
  }
  if (isTopEdge(e)) {
    res.yMin = 0;
    res.yMax = inset;
  } else if (isBottomEdge(e)) {
    res.yMin = wh - inset;
    res.yMax = wh;
  }
  return res;
}

// Truncate names to this many characters (UTF8 characters, naturally). Much
// simpler than trying to calculate the 'best' length based on the render text
// width, which is quite unnecessary anyway.
static constexpr int maxMenuNameChars = 100;

std::string Client::MenuName() const {
  if (name_.size() <= maxMenuNameChars) {
    return name_;
  }
  int chars = 0;
  int uniLeft = 0;
  for (int i = 0; i < name_.size(); i++) {
    if (uniLeft && --uniLeft)
      continue;  // Skip trailing UTF8 only.
    char ch = name_[i];
    chars++;
    if (chars == maxMenuNameChars) {
      // i must be at the start of a unicode character (or ascii), and we've
      // seen as many visible characters as we wanted.
      return name_.substr(0, i) + "...";
    }
    int mask = 0xf8;
    int val = 0xf0;
    uniLeft = 4;
    while (mask && uniLeft) {
      if ((ch & mask) == val) {
        break;
      }
      uniLeft--;
      mask = (mask << 1) & 0xff;
      val = (val << 1) & 0xff;
    }
  }
  // Dropped off the end? The name must be under maxMenuNameChars UTF8
  // characters in length then.
  return name_;
}

void Client::Hide() {
  LScr::I->GetHider()->Hide(this);
}

void Client::Unhide() {
  LScr::I->GetHider()->Unhide(this);
}

Edge Client::EdgeAt(Window w, int x, int y) const {
  if (w != parent) {
    return EContents;
  }
  if (closeBounds(false).contains(x, y)) {  // false -> get action bounds.
    return EClose;
  }
  if (titleBarBounds(size.width).contains(x, y)) {
    return ENone;  // Rename to ETitleBar.
  }
  const std::vector<Edge> movementEdges{ETopLeft, ETop,        ETopRight,
                                        ERight,   ELeft,       EBottomLeft,
                                        EBottom,  EBottomRight};
  for (Edge e : movementEdges) {
    if (edgeBounds(e).contains(x, y)) {
      return e;
    }
  }
  return ENone;
}

void Client::SetIcon(ImageIcon* icon) {
  if (icon) {
    icon_ = icon;
  }
}

static void focusChildrenOf(Window parent) {
  WindowTree wtree = WindowTree::Query(dpy, parent);
  for (Window win : wtree.children) {
    XWindowAttributes attr;
    XGetWindowAttributes(dpy, win, &attr);
    if (attr.all_event_masks & FocusChangeMask) {
      XSetInputFocus(dpy, win, RevertToPointerRoot, CurrentTime);
    }
  }
}

void Client::FocusGained() {
  if (framed && Resources::I->ClickToFocus()) {
    // In click-to-focus mode, our FocusLost function will grab button events
    // on the client's window. We must relinquish this grabbing when we gain
    // focus, otherwise the client itself won't get the events when it is
    // focused.
    XUngrabButton(dpy, AnyButton, AnyModifier, window);
  }
  DrawBorder();
}

void Client::FocusLost() {
  if (framed && Resources::I->ClickToFocus()) {
    // In click-to-focus mode, we need to intercept button clicks within the
    // client window, so we can give the window focus. While some applications,
    // notably java apps, will grab input focus when clicked on, xterm and
    // many others do not. Thus, we need to grab click notifications ourselves
    // so that we can properly support click-to-focus.
    XGrabButton(dpy, AnyButton, AnyModifier, window, false,
                ButtonPressMask | ButtonReleaseMask, GrabModeAsync,
                GrabModeSync, None, None);
  }
  DrawBorder();
}

void Client::DrawBorder() {
  if (!framed) {
    return;
  }
  const int quarter = (titleBarHeight()) / 4;

  LScr* lscr = LScr::I;
  if (parent == lscr->Root() || parent == 0 || !framed || wstate.fullscreen) {
    return;
  }

  const bool active = HasFocus();

  XSetWindowBackground(dpy, parent,
                       active ? lscr->ActiveBorder() : lscr->InactiveBorder());
  XClearWindow(dpy, parent);

  // Cross for the close icon.
  const Rect r = closeBounds(true);  // true -> get display bounds.
  const GC close_gc = lscr->GetCloseIconGC(active);
  XDrawLine(dpy, parent, close_gc, r.xMin, r.yMin, r.xMax, r.yMax);
  XDrawLine(dpy, parent, close_gc, r.xMin, r.yMax, r.xMax, r.yMin);
  const int bw = borderWidth();
  if (active) {
    // Give the title a nice background, and differentiate it from the
    // rest of the furniture to show it acts differently (moves the window
    // rather than resizing it).
    // However, skip the top few pixels if the 'topBorderWidth' is non-zero, to
    // show where the resize handle is.
    const int topBW = topBorderWidth();
    const int x = bw + 3 * quarter;
    const int w = size.width - 2 * x;
    const int h = textHeight() + bw - topBW;
    XFillRectangle(dpy, parent, lscr->GetTitleGC(), x, topBW, w, h);
  }

  // Find where the title stuff is going to go.
  int x = bw + 2 + (3 * quarter);
  int y = bw / 2 + g_font->ascent;

  // Do we have an icon? If so, draw it to the left of the title text.
  if (Icon() && Resources::I->AppIconInWindowTitle()) {
    if (active) {
      Icon()->PaintActive(parent, x, 0, titleBarHeight(), titleBarHeight());
    } else {
      Icon()->PaintInactive(parent, x, 0, titleBarHeight(), titleBarHeight());
    }
    x += titleBarHeight();  // Title bar text must come after.
  }

  // Draw window title.
  XftColor* color = active ? &g_font_active_title : &g_font_inactive_title;
  drawString(parent, x, y, Name(), color);
}

void Client::SetSize(const Rect& r) {
  size.x = r.xMin;
  size.y = r.yMin;
  size.width = r.width();
  size.height = r.height();
}

Rect Client::RectWithBorder() const {
  Rect res = Rect{size.x, size.y, size.x + size.width, size.y + size.height};
  if (framed) {
    res.yMin -= textHeight();
  }
  return res;
}

Rect Client::RectNoBorder() const {
  Rect res = Rect{size.x, size.y, size.x + size.width, size.y + size.height};
  if (framed) {
    const int bw = borderWidth();
    res.xMin += bw;
    res.xMax -= bw;
    res.yMin += bw;
    res.yMax -= bw;
  }
  return res;
}

void Client_Remove(Client* c) {
  if (c == 0) {
    return;
  }
  if (c->parent != LScr::I->Root()) {
    XDestroyWindow(dpy, c->parent);
  }
  LScr::I->Remove(c);
  ewmh_set_client_list();
  ewmh_set_strut();
}

// The diff is expected to be the difference between a window position and some
// barrier (eg edge of a screen). If that difference is within 0..EDGE_RESIST,
// we return it; otherwise we return 0.
// This makes the code to apply edge resistance a simple matter of subtracting
// or adding the returned value.
static int getResistanceOffset(int diff) {
  if (diff <= 0 || diff > EDGE_RESIST) {
    return 0;
  }
  return diff;
}

bool Client_MakeSaneAndMove(Client* c, Edge edge, int x, int y, int w, int h) {
  const Rect before = c->RectNoBorder();
  Client_MakeSane(c, edge, x, y, w, h);
  const Rect after = c->RectNoBorder();
  LOGD(c) << "Sanity changed rect from " << before << " to " << after;
  const bool resized =
      (before.width() != after.width()) || (before.height() != after.height());
  const bool moved = (before.xMin != after.xMin) || (before.yMin != after.yMin);
  if (resized) {
    // May need to deal with framed windows here.
    const int th = textHeight();
    XMoveResizeWindow(dpy, c->parent, c->size.x, c->size.y - th, c->size.width,
                      c->size.height + th);
    const int border = borderWidth();
    // We used to use some odd logic to optionally send a configureNotify.
    // However, from my reading of this page:
    // https://tronche.com/gui/x/xlib/events/window-state-change/configure.html
    // ...it seems the X server is responsible for sending such things; our
    // only job is to actually move/resize windows. So let's just do that.
    XMoveResizeWindow(dpy, c->window, border, border + th,
                      c->size.width - 2 * border, c->size.height - 2 * border);
    sendConfigureNotify(c);
  } else if (moved) {
    if (c->framed) {
      XMoveWindow(dpy, c->parent, c->size.x, c->size.y - textHeight());
    } else {
      XMoveWindow(dpy, c->parent, c->size.x, c->size.y);
    }
    // Do I need to send a configure notify? According to this:
    // https://tronche.com/gui/x/xlib/events/window-state-change/configure.html
    // ...it looks like the job of the X server itself.
    sendConfigureNotify(c);
  }
  return moved || resized;
}

// x and y are the proposed new coordinates of the window. w and h are the
// proposed new width and height, or zero if the size should remain unchanged.
// Returns true if the window size or location was modified.
bool Client_MakeSane(Client* c, Edge edge, int x, int y, int w, int h) {
  const Rect old_pos =
      Rect::FromXYWH(c->size.x, c->size.y, c->size.width, c->size.height);
  bool horizontal_ok = true;
  bool vertical_ok = true;
  if (w == 0) {
    w = c->size.width;
  }
  if (h == 0) {
    h = c->size.height;
  }

  if (edge != ENone) {
    // Make sure we're not making the window too small.
    if (w < c->size.min_width) {
      horizontal_ok = false;
    }
    if (h < c->size.min_height) {
      vertical_ok = false;
    }

    // Make sure we're not making the window too large.
    if (c->size.flags & PMaxSize) {
      if (w > c->size.max_width) {
        horizontal_ok = false;
      }
      if (h > c->size.max_height) {
        vertical_ok = false;
      }
    }

    // Make sure the window's width & height are multiples of
    // the width & height increments (not including the base size).
    if (c->size.width_inc > 1) {
      int apparent_w = w - 2 * borderWidth() - c->size.base_width;
      int x_fix = apparent_w % c->size.width_inc;

      if (isLeftEdge(edge)) {
        x += x_fix;
      }
      if (isLeftEdge(edge) || isRightEdge(edge)) {
        w -= x_fix;
      }
    }

    if (c->size.height_inc > 1) {
      int apparent_h = h - 2 * borderWidth() - c->size.base_height;
      int y_fix = apparent_h % c->size.height_inc;

      if (isTopEdge(edge)) {
        y += y_fix;
      }
      if (isTopEdge(edge) || isBottomEdge(edge)) {
        h -= y_fix;
      }
    }

    // Check that we may change the client horizontally and vertically.
    if (c->size.width_inc == 0) {
      horizontal_ok = false;
    }
    if (c->size.height_inc == 0) {
      vertical_ok = false;
    }
  }

  /* Ensure that at least one border is not entirely within the
   * reserved areas. Keeping clients completely within the
   * the workarea is too restrictive, but this measure means they
   * should always be accessible.
   * Of course all of this is only applicable if the client doesn't
   * set a strut itself.					jfc
   */
  LScr* lscr = LScr::I;
  // Go through all screens, finding the smallest movement (in both x and y, to
  // cope with the display area shrinking) to ensure the window is visible on a
  // screen.
  int best_x_fix = INT_MAX;
  int best_y_fix = INT_MAX;
  // We get visible areas with or without the effect of struts, based on whether
  // the client sets struts itself. If it does, we must ignore struts so we
  // don't prevent the client being placed on its own reserved area.
  for (const auto& r : lscr->VisibleAreas(!c->HasStruts())) {
    const int bw = borderWidth();
    int x_fix = 0;
    int y_fix = 0;
    if (x + bw >= r.xMax) {
      x_fix = r.xMax - (x + bw);
    } else if (x + w - bw <= r.xMin) {
      x_fix = r.xMin - (x + w - bw);
    }
    if (y + bw >= r.yMax) {
      y_fix = r.yMax - (y + bw);
    } else if (y + h - bw <= r.yMin) {
      y_fix = r.yMin - (y + h - bw);
    }
    // If we need fixing for this screen, we find the worse offender of the two
    // axes (one of them may be zero), and check if that's the best solution
    // yet.
    if (std::max(abs(x_fix), abs(y_fix)) <
        std::max(abs(best_x_fix), abs(best_y_fix))) {
      best_x_fix = x_fix;
      best_y_fix = y_fix;
    }
  }
  // If we have found a best fix, we must fix it!
  x += best_x_fix;
  y += best_y_fix;

  // If the edge resistance code is used for window sizes, we get funny effects
  // during some resize events.
  // For example if a window is very close to the bottom-right corner of the
  // screen and is made smaller suddenly using the top-left corner, the bottom-
  // right corner of the window moves slightly up and to the left, such that it
  // is effectively being resized from two directions. This is wrong and
  // annoying.
  // Edge resistance is only useful for moves anyway, so simply disable the code
  // for resizes to avoid the bug.
  if (edge == ENone) {
    // Implement edge resistance for all of the visible areas. There can be
    // several if we're using multiple monitors with xrandr, and they can be
    // offset from each other. However, for each box, ensure that some part of
    // the window is interacting with an edge.
    for (const auto& r : lscr->VisibleAreas(!c->HasStruts())) {
      // Check for top/bottom if the horizontal location of the window overlaps
      // with that of the screen area.
      if ((x < r.xMax) && (x + w > r.xMin)) {
        y += getResistanceOffset(r.yMin - (y - textHeight()));  // Top.
        y -= getResistanceOffset((y + h) - r.yMax);             // Bottom.
      }
      // Check for left/right if the vertical location of the window overlaps
      // with that of the screen area.
      if ((y < r.yMax) && (y + h > r.yMin)) {
        x += getResistanceOffset(r.xMin - x);        // Left.
        x -= getResistanceOffset((x + w) - r.xMax);  // Right.
      }
    }
    if (horizontal_ok) {
      c->size.x = x;
    }
    if (vertical_ok) {
      c->size.y = y;
    }
  } else {
    if (horizontal_ok) {
      c->size.x = x;
      c->size.width = w;
    }
    if (vertical_ok) {
      c->size.y = y;
      c->size.height = h;
    }
  }
  const Rect new_pos = Rect::FromXYWH(x, y, w, h);
  return new_pos != old_pos;
}

static std::string makeSizeString(int x, int y) {
  std::ostringstream buf;
  buf << x << " x " << y;
  return buf.str();
}

void Client_SizeFeedback() {
  // Make the popup 10% wider than the widest string it needs to show.
  popup_width =
      textWidth(makeSizeString(DisplayWidth(dpy, 0), DisplayHeight(dpy, 0)));
  popup_width += popup_width / 10;

  // Put the popup in the right place to report on the window's size.
  const MousePos mp = getMousePosition();
  XMoveResizeWindow(dpy, LScr::I->Popup(), mp.x + 8, mp.y + 8, popup_width,
                    textHeight() + 1);
  XMapRaised(dpy, LScr::I->Popup());

  // Ensure that the popup contents get redrawn. Eventually, the function
  // size_expose will get called to do the actual redraw.
  XClearArea(dpy, LScr::I->Popup(), 0, 0, 0, 0, true);
}

void size_expose() {
  Client* c = LScr::I->GetFocuser()->GetFocusedClient();
  if (!c) {
    return;
  }
  int width = c->size.width - 2 * borderWidth();
  int height = c->size.height - 2 * borderWidth();

  // This dance ensures that we report 80x24 for an xterm even when
  // it has a scrollbar.
  if (c->size.flags & (PMinSize | PBaseSize) && c->size.flags & PResizeInc) {
    if (c->size.flags & PBaseSize) {
      width -= c->size.base_width;
      height -= c->size.base_height;
    } else {
      width -= c->size.min_width;
      height -= c->size.min_height;
    }
  }

  if (c->size.width_inc != 0) {
    width /= c->size.width_inc;
  }
  if (c->size.height_inc != 0) {
    height /= c->size.height_inc;
  }

  const std::string text = makeSizeString(width, height);
  const int x = (popup_width - textWidth(text)) / 2;
  drawString(LScr::I->Popup(), x, g_font->ascent + 1, text,
             &g_font_popup_colour);
}

void Client_Lower(Client* c) {
  if (c == 0) {
    return;
  }
  XLowerWindow(dpy, c->window);
  if (c->framed) {
    XLowerWindow(dpy, c->parent);
  }
  ewmh_set_client_list();
}

void Client_Raise(Client* c) {
  if (c == 0) {
    return;
  }
  if (c->framed) {
    XRaiseWindow(dpy, c->parent);
  }
  XRaiseWindow(dpy, c->window);

  for (auto it : LScr::I->Clients()) {
    Client* tr = it.second;
    if (tr->trans != c->window && !(c->framed && tr->trans == c->parent)) {
      continue;
    }
    if (tr->framed) {
      XRaiseWindow(dpy, tr->parent);
    }
    XRaiseWindow(dpy, tr->window);
  }
  ewmh_set_client_list();
}

void Client_Close(Client* c) {
  if (c == 0) {
    return;
  }
  // Terminate the client nicely if possible. Be brutal otherwise.
  if (c->proto & Pdelete) {
    sendClientMessage(c->window, wm_protocols, wm_delete, CurrentTime);
  } else {
    XKillClient(dpy, c->window);
  }
}

void Client::SetState(int state) {
  long data[2];

  data[0] = (long)state;
  data[1] = (long)None;

  state_ = state;
  XChangeProperty(dpy, window, wm_state, wm_state, 32, PropModeReplace,
                  (unsigned char*)data, 2);
  ewmh_set_state(this);
}

static void sendClientMessage(Window w, Atom a, long data0, long data1) {
  XEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.xclient.type = ClientMessage;
  ev.xclient.window = w;
  ev.xclient.message_type = a;
  ev.xclient.format = 32;
  ev.xclient.data.l[0] = data0;
  ev.xclient.data.l[1] = data1;
  const long mask = (w == LScr::I->Root()) ? SubstructureRedirectMask : 0L;
  XSendEvent(dpy, w, false, mask, &ev);
}

extern void Client_ResetAllCursors() {
  for (auto it : LScr::I->Clients()) {
    Client* c = it.second;
    if (!c->framed) {
      continue;
    }
    XSetWindowAttributes attr;
    attr.cursor = LScr::I->Cursors()->Root();
    XChangeWindowAttributes(dpy, c->parent, CWCursor, &attr);
    c->cursor = ENone;
  }
}

extern void Client_FreeAll() {
  for (auto it : LScr::I->Clients()) {
    Client* c = it.second;

    // Reparent the client window to the root, to elide our furniture window.
    XReparentWindow(dpy, c->window, LScr::I->Root(), c->size.x, c->size.y);
    if (c->hidden) {
      // The window was iconised, so map it back into view so it isn't lost
      // forever, but lower it so it doesn't jump all over the foreground.
      XMapWindow(dpy, c->window);
      XLowerWindow(dpy, c->window);
    }

    // Give it back its initial border width.
    XWindowChanges wc;
    wc.border_width = c->border;
    XConfigureWindow(dpy, c->window, CWBorderWidth, &wc);
  }
}

extern void Client_ColourMap(XEvent* e) {
  for (auto it : LScr::I->Clients()) {
    Client* c = it.second;
    for (int i = 0; i < c->ncmapwins; i++) {
      if (c->cmapwins[i] == e->xcolormap.window) {
        c->wmcmaps[i] = e->xcolormap.colormap;
        if (c->HasFocus()) {
          cmapfocus(c);
        }
        return;
      }
    }
  }
}

extern void Client_EnterFullScreen(Client* c) {
  XWindowChanges fs;

  memcpy(&c->return_size, &c->size, sizeof(XSizeHints));
  // For now, just find the 'main screen' and use that.
  // Ideally, we'd actually try to find the largest contiguous rectangle, as
  // someone might be using two identical-sized monitors next to each other
  // to get a bigger view of what they're killing, but for now we'll save that
  // for another day.
  const Rect scr = LScr::I->GetPrimaryVisibleArea(false);  // Without struts.
  if (c->framed) {
    const int bw = borderWidth();
    c->size.x = fs.x = scr.xMin - bw;
    c->size.y = fs.y = scr.yMin - bw;
    c->size.width = fs.width = scr.width() + 2 * bw;
    c->size.height = fs.height = scr.height() + 2 * bw;
    XConfigureWindow(dpy, c->parent, CWX | CWY | CWWidth | CWHeight, &fs);

    fs.x = 0;
    fs.y = 0;
    fs.width = scr.width();
    fs.height = scr.height();
    XConfigureWindow(dpy, c->window, CWX | CWY | CWWidth | CWHeight, &fs);
    XRaiseWindow(dpy, c->parent);
  } else {
    c->size.x = fs.x = scr.xMin;
    c->size.y = fs.y = scr.yMin;
    c->size.width = fs.width = scr.width();
    c->size.height = fs.height = scr.height();
    XConfigureWindow(dpy, c->window, CWX | CWY | CWWidth | CWHeight, &fs);
    XRaiseWindow(dpy, c->window);
  }
  sendConfigureNotify(c);
}

extern void Client_ExitFullScreen(Client* c) {
  XWindowChanges fs;

  memcpy(&c->size, &c->return_size, sizeof(XSizeHints));
  if (c->framed) {
    fs.x = c->size.x;
    fs.y = c->size.y - textHeight();
    fs.width = c->size.width;
    fs.height = c->size.height + textHeight();
    XConfigureWindow(dpy, c->parent, CWX | CWY | CWWidth | CWHeight, &fs);

    const int bw = borderWidth();
    fs.x = bw;
    fs.y = bw + textHeight();
    fs.width = c->size.width - 2 * bw;
    fs.height = c->size.height - 2 * bw;
    XConfigureWindow(dpy, c->window, CWX | CWY | CWWidth | CWHeight, &fs);
  } else {
    fs.x = c->size.x;
    fs.y = c->size.y;
    fs.width = c->size.width;
    fs.height = c->size.height;
    XConfigureWindow(dpy, c->window, CWX | CWY | CWWidth | CWHeight, &fs);
  }
  sendConfigureNotify(c);
}

void Client::SetName(const char* c, int len) {
  name_ = (c && len) ? std::string(c, len) : "";
}

bool Client::HasFocus() const {
  return this == LScr::I->GetFocuser()->GetFocusedClient();
}

// static
Client* Client::FocusedClient() {
  return LScr::I->GetFocuser()->GetFocusedClient();
}

void Focuser::EnterWindow(Window w, Time time) {
  // If the window being entered is still part of the same client, we do
  // nothing. This avoids giving focus to a window in the following situation:
  // 1: Mouse pointer is over window X.
  // 2: Window Y is opened and is given focus.
  // 3: Mouse pointer is moved such that it crosses into a different window in
  //    the client of X.
  // In this situation, window Y should still keep focus.
  Client* c = LScr::I->GetClient(w);
  const Window le = last_entered_;
  last_entered_ = w;
  if (!c || (c == LScr::I->GetClient(le))) {
    return;  // No change in pointed-at client, so we have nothing to do.
  }
  if (!Resources::I->ClickToFocus()) {
    FocusClient(c, time);
  }
}

void Focuser::UnfocusClient(Client* c) {
  const bool had_focus = c->HasFocus();
  removeFromHistory(c);
  if (!had_focus) {
    return;
  }
  // The given client used to have input focus; give focus to the next in line.
  if (focus_history_.empty()) {
    return;  // No one left to give focus to.
  }
  reallyFocusClient(focus_history_.front(), CurrentTime);
}

void Focuser::FocusClient(Client* c, Time time) {
  // If this window is already focused, ignore.
  if (!c->HasFocus()) {
    reallyFocusClient(c, time);
    // Old LWM seems to always have raised the window being focused, so let's
    // copy that. Maybe it should be a separate resource option though?
    if (Resources::I->ClickToFocus()) {
      Client_Raise(c);
    }
  }
}

void Focuser::reallyFocusClient(Client* c, Time time) {
  Client* was_focused = GetFocusedClient();
  removeFromHistory(c);
  focus_history_.push_front(c);

  XDeleteProperty(dpy, LScr::I->Root(), ewmh_atom[_NET_ACTIVE_WINDOW]);
  // There was a check for 'c->IsHidden()' here. Needed?
  if (c->accepts_focus) {
    XSetInputFocus(dpy, c->window, RevertToPointerRoot, CurrentTime);
    // Also send focus messages to child windows that can receive
    // focus events.
    // This fixes a bug in focus-follows-mouse whereby Java apps,
    // which have a child window called FocusProxy which must be
    // given the focus event, would not get input focus when the
    // mouse was moved into them.
    focusChildrenOf(c->window);
    if (c->proto & Ptakefocus) {
      sendClientMessage(c->window, wm_protocols, wm_take_focus, time);
    }
    cmapfocus(c);
  } else {
    // FIXME: is this sensible?
    XSetInputFocus(dpy, None, RevertToPointerRoot, CurrentTime);
  }
  XChangeProperty(dpy, LScr::I->Root(), ewmh_atom[_NET_ACTIVE_WINDOW],
                  XA_WINDOW, 32, PropModeReplace, (unsigned char*)&c->window,
                  1);

  if (was_focused && (was_focused != c)) {
    was_focused->FocusLost();
  }
  c->FocusGained();
}

void Focuser::removeFromHistory(Client* c) {
  for (std::list<Client*>::iterator it = focus_history_.begin();
       it != focus_history_.end(); it++) {
    if (*it == c) {
      focus_history_.erase(it);
      return;
    }
  }
}

Client* Focuser::GetFocusedClient() {
  if (focus_history_.empty()) {
    return nullptr;
  }
  return focus_history_.front();
}
