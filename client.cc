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

static Client* current;
static Client* last_focus = NULL;

static int popup_width;  // The width of the size-feedback window.

Edge interacting_edge;

static void sendClientMessage(Window, Atom, long, long);

Rect closeBounds() {
  const int quarter = (borderWidth() + textHeight()) / 4;
  const int cMin = quarter + 2;
  const int cMax = 3 * quarter;
  return Rect{cMin, cMin, cMax, cMax};
}

// Returns the total height, in pixels, of the window title bar.
int titleBarHeight() {
  return textHeight() + borderWidth();
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
  if (closeBounds().contains(x, y)) {
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

void Client::SetIconPixmap(Pixmap icon, Pixmap mask) {
  icon_ = ImageIcon::Create(icon, mask);
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

  if (active) {
    // Cross for the close icon.
    const Rect r = closeBounds();
    XDrawLine(dpy, parent, lscr->GetGC(), r.xMin, r.yMin, r.xMax, r.yMax);
    XDrawLine(dpy, parent, lscr->GetGC(), r.xMin, r.yMax, r.xMax, r.yMin);

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
    Icon()->Paint(parent, x, 0, titleBarHeight(), titleBarHeight());
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
  // Note: here was some code to unhide the menu (without unhiding it) to remove
  // it from the linked list. This should no longer be required, as we just
  // reference windows by their ID (and keep a copy of their name) in the menu
  // structure itself.

  // Take note of any special status this window had, and then nullify all the
  // ugly global variables that currently point to it. This prevents the
  // structure's future use in other code, which can cause crashes.
  const bool wasCurrent = c == current;
  const bool wasLastFocus = (current == NULL && c == last_focus);
  current = last_focus = nullptr;

  // A deleted window can no longer be the current window.
  if (wasCurrent || wasLastFocus) {
    Client* focus = NULL;

    // As pointed out by J. Han, if a window disappears while it's
    // being reshaped you need to get rid of the size indicator.
    if (wasCurrent && mode == wm_reshaping) {
      XUnmapWindow(dpy, LScr::I->Popup());
      mode = wm_idle;
    }
    Client_Focus(focus, CurrentTime);
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
  int width = current->size.width - 2 * borderWidth();
  int height = current->size.height - 2 * borderWidth();

  // This dance ensures that we report 80x24 for an xterm even when
  // it has a scrollbar.
  if (current->size.flags & (PMinSize | PBaseSize) &&
      current->size.flags & PResizeInc) {
    if (current->size.flags & PBaseSize) {
      width -= current->size.base_width;
      height -= current->size.base_height;
    } else {
      width -= current->size.min_width;
      height -= current->size.min_height;
    }
  }

  if (current->size.width_inc != 0) {
    width /= current->size.width_inc;
  }
  if (current->size.height_inc != 0) {
    height /= current->size.height_inc;
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
        if (c == current) {
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

extern void Client_Focus(Client* c, Time time) {
  std::vector<Client*> redraw;
  if (current) {
    redraw.push_back(current);
    XDeleteProperty(dpy, LScr::I->Root(), ewmh_atom[_NET_ACTIVE_WINDOW]);
  }

  // If c != NULL, and we have a current window, store current as being the
  // last_focused window, so that later we can restore focus to it if c closes.
  if (c && current) {
    last_focus = current;
  }
  // If c == NULL, then we should instead try to restore focus to last_focus,
  // if it is not itself NULL.
  if (!c && last_focus) {
    c = last_focus;
    last_focus = NULL;
  }
  current = c;
  if (c) {
    redraw.push_back(c);
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
                    XA_WINDOW, 32, PropModeReplace,
                    (unsigned char*)&current->window, 1);
  }
  for (Client* c : redraw) {
    c->DrawBorder();
  }
}

void Client::SetName(const char* c, int len) {
  name_ = (c && len) ? std::string(c, len) : "";
}

bool Client::HasFocus() const {
  return this == current;
}

// static
Client* Client::FocusedClient() {
  return current;
}
