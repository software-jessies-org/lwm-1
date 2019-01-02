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

Edge interacting_edge;

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
  const int w = windowWidth - 2 * x;
  return Rect{x, 0, w, titleBarHeight()};
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
  const std::vector<Edge> movementEdges{
      ETopLeft, ETopRight, ERight, ELeft, EBottomLeft, EBottom, EBottomRight};
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
  if (active) {
    // Give the title a nice background, and differentiate it from the
    // rest of the furniture to show it acts differently (moves the window
    // rather than resizing it).
    const int x = borderWidth() + 3 * quarter;
    const int w = size.width - 2 * x;
    XFillRectangle(dpy, parent, lscr->GetTitleGC(), x, 0, w,
                   textHeight() + borderWidth());
  }

  // Find where the title stuff is going to go.
  int x = borderWidth() + 2 + (3 * quarter);
  int y = borderWidth() / 2 + g_font->ascent;

  // Do we have an icon? If so, draw it to the left of the title text.
  if (Icon()) {
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

void Client_Remove(Client* c) {
  if (c == 0) {
    return;
  }
  // As pointed out by J. Han, if a window disappears while it's
  // being reshaped you need to get rid of the size indicator.
  if (c->HasFocus() && mode == wm_reshaping) {
    XUnmapWindow(dpy, LScr::I->Popup());
    mode = wm_idle;
  }

  if (c->parent != LScr::I->Root()) {
    XDestroyWindow(dpy, c->parent);
  }
  LScr::I->Remove(c);
  ewmh_set_client_list();
  ewmh_set_strut();
}

void Client_MakeSane(Client* c, Edge edge, int* x, int* y, int* dx, int* dy) {
  bool horizontal_ok = true;
  bool vertical_ok = true;

  if (edge != ENone) {
    // Make sure we're not making the window too small.
    if (*dx < c->size.min_width) {
      horizontal_ok = false;
    }
    if (*dy < c->size.min_height) {
      vertical_ok = false;
    }

    // Make sure we're not making the window too large.
    if (c->size.flags & PMaxSize) {
      if (*dx > c->size.max_width) {
        horizontal_ok = false;
      }
      if (*dy > c->size.max_height) {
        vertical_ok = false;
      }
    }

    // Make sure the window's width & height are multiples of
    // the width & height increments (not including the base size).
    if (c->size.width_inc > 1) {
      int apparent_dx = *dx - 2 * borderWidth() - c->size.base_width;
      int x_fix = apparent_dx % c->size.width_inc;

      if (isLeftEdge(edge)) {
        *x += x_fix;
      }
      if (isLeftEdge(edge) || isRightEdge(edge)) {
        *dx -= x_fix;
      }
    }

    if (c->size.height_inc > 1) {
      int apparent_dy = *dy - 2 * borderWidth() - c->size.base_height;
      int y_fix = apparent_dy % c->size.height_inc;

      if (isTopEdge(edge)) {
        *y += y_fix;
      }
      if (isTopEdge(edge) || isBottomEdge(edge)) {
        *dy -= y_fix;
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
  const EWMHStrut& scrStrut = lscr->Strut();
  if (c->strut.left == 0 && c->strut.right == 0 && c->strut.top == 0 &&
      c->strut.bottom == 0) {
    if ((int)(*y + borderWidth()) >= (int)(lscr->Height() - scrStrut.bottom)) {
      *y = lscr->Height() - scrStrut.bottom - borderWidth();
    }
    if ((int)(*y + c->size.height - borderWidth()) <= (int)scrStrut.top) {
      *y = scrStrut.top + borderWidth() - c->size.height;
    }
    if ((int)(*x + borderWidth()) >= (int)(lscr->Width() - scrStrut.right)) {
      *x = lscr->Width() - scrStrut.right - borderWidth();
    }
    if ((int)(*x + c->size.width - borderWidth()) <= (int)scrStrut.left) {
      *x = scrStrut.left + borderWidth() - c->size.width;
    }
  }

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
    // Introduce a resistance to the workarea edge, so that windows may
    // be "thrown" to the edge of the workarea without precise mousing,
    // as requested by MAD.
    if (*x<(int)scrStrut.left&& * x>((int)scrStrut.left - EDGE_RESIST)) {
      *x = (int)scrStrut.left;
    }
    if ((*x + c->size.width) > (int)(lscr->Width() - scrStrut.right) &&
        (*x + c->size.width) <
            (int)(lscr->Width() - scrStrut.right + EDGE_RESIST)) {
      *x = (int)(lscr->Width() - scrStrut.right - c->size.width);
    }
    if ((*y - textHeight()) < (int)scrStrut.top &&
        (*y - textHeight()) > ((int)scrStrut.top - EDGE_RESIST)) {
      *y = (int)scrStrut.top + textHeight();
    }
    if ((*y + c->size.height) > (int)(lscr->Height() - scrStrut.bottom) &&
        (*y + c->size.height) <
            (int)(lscr->Height() - scrStrut.bottom + EDGE_RESIST)) {
      *y = (int)(lscr->Height() - scrStrut.bottom - c->size.height);
    }
  }

  // Update that part of the client information that we're happy with.
  if (interacting_edge != ENone) {
    if (horizontal_ok) {
      c->size.x = *x;
      c->size.width = *dx;
    }
    if (vertical_ok) {
      c->size.y = *y;
      c->size.height = *dy;
    }
  } else {
    if (horizontal_ok) {
      c->size.x = *x;
    }
    if (vertical_ok) {
      c->size.y = *y;
    }
  }
}

static std::string makeSizeString(int x, int y) {
  std::ostringstream buf;
  buf << x << " x " << y;
  return buf.str();
}

void Client_SizeFeedback() {
  // Make the popup 10% wider than the widest string it needs to show.
  popup_width = textWidth(makeSizeString(LScr::I->Width(), LScr::I->Height()));
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

extern void Client_ReshapeEdge(Client* c, Edge edge) {
  if (c == 0) {
    return;
  }
  // Find out where we've got hold of the window.
  MousePos mp = getMousePosition();
  const int sx = c->size.x - mp.x;
  const int sy = c->size.y - mp.y;

  Cursor cursor = LScr::I->Cursors()->ForEdge(edge);
  XChangeActivePointerGrab(dpy,
                           ButtonMask | PointerMotionHintMask |
                               ButtonMotionMask | OwnerGrabButtonMask,
                           cursor, CurrentTime);

  // Store some state so that we can get back into the main event
  // dispatching thing.
  interacting_edge = edge;
  start_x = sx;
  start_y = sy;
  mode = wm_reshaping;
  ewmh_set_client_list();
}

extern void Client_Move(Client* c) {
  Client_ReshapeEdge(c, ENone);
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
  const int scrWidth = LScr::I->Width();
  const int scrHeight = LScr::I->Height();
  if (c->framed) {
    c->size.x = fs.x = -borderWidth();
    c->size.y = fs.y = -borderWidth();
    c->size.width = fs.width = scrWidth + 2 * borderWidth();
    c->size.height = fs.height = scrHeight + 2 * borderWidth();
    XConfigureWindow(dpy, c->parent, CWX | CWY | CWWidth | CWHeight, &fs);

    fs.x = borderWidth();
    fs.y = borderWidth();
    fs.width = scrWidth;
    fs.height = scrHeight;
    XConfigureWindow(dpy, c->window, CWX | CWY | CWWidth | CWHeight, &fs);
    XRaiseWindow(dpy, c->parent);
  } else {
    c->size.x = c->size.y = fs.x = fs.y = 0;
    c->size.width = fs.width = scrWidth;
    c->size.height = fs.height = scrHeight;
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

    fs.x = borderWidth();
    fs.y = borderWidth() + textHeight();
    fs.width = c->size.width - (2 * borderWidth());
    fs.height = c->size.height - (2 * borderWidth());
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
  FocusClient(c, time);
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
    was_focused->DrawBorder();
  }
  c->DrawBorder();
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
