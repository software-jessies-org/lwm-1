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

#include <iostream>
#include <set>

#include "ewmh.h"
#include "lwm.h"

#define MENU_Y_PADDING 6

MousePos getMousePosition() {
  Window root, child;
  MousePos res;
  memset(&res, 0, sizeof(res));
  int t1, t2;
  XQueryPointer(dpy, LScr::I->Root(), &root, &child, &res.x, &res.y, &t1, &t2,
                &res.modMask);
  return res;
}

// hiddenIDFor returns the parent Window ID for the given client. We have a
// specially-named function for this so that we don't get confused about which
// Window ID we're using, as this is used in both Hide and OpenMenu.
static Window hiddenIDFor(const Client* c) {
  return c->parent;
}

static void mapAndRaise(Window w, int xmin, int ymin, int width, int height) {
  XMoveResizeWindow(dpy, w, xmin, ymin, width, height);
  XMapRaised(dpy, w);
}

void Hider::showHighlightBox(int itemIndex) {
  // If itemIndex isn't an item, actually hide the box.
  if (itemIndex < 0 || itemIndex >= open_content_.size()) {
    hideHighlightBox();
    return;
  }
  if (!highlightL) {
    // No highlight windows created yet; create them now.
    Display* dpy = LScr::I->Dpy();
    const Window root = LScr::I->Root();
    const unsigned long col =
        Resources::I->GetColour(Resources::WINDOW_HIGHLIGHT_COLOUR);
    highlightL = XCreateSimpleWindow(dpy, root, 0, 0, 1, 1, 1, col, col);
    highlightR = XCreateSimpleWindow(dpy, root, 0, 0, 1, 1, 1, col, col);
    highlightT = XCreateSimpleWindow(dpy, root, 0, 0, 1, 1, 1, col, col);
    highlightB = XCreateSimpleWindow(dpy, root, 0, 0, 1, 1, 1, col, col);
  }
  Client* c = LScr::I->GetClient(open_content_[itemIndex].w);
  if (!c) {
    // Client has probably gone away in the meantime; no highlight to show.
    hideHighlightBox();
    return;
  }
  const int xmin = c->size.x;
  const int ymin = c->size.y - textHeight();
  const int width = c->size.width;
  const int height = c->size.height + textHeight();
  mapAndRaise(highlightL, xmin, ymin, 1, height);
  mapAndRaise(highlightR, xmin + width, ymin, 1, height);
  mapAndRaise(highlightT, xmin, ymin, width, 1);
  mapAndRaise(highlightB, xmin, ymin + height, width, 1);
}

void Hider::hideHighlightBox() {
  if (!highlightL) {
    // No highlight windows created; that means we have nothing to hide.
    return;
  }
  XUnmapWindow(dpy, highlightL);
  XUnmapWindow(dpy, highlightR);
  XUnmapWindow(dpy, highlightT);
  XUnmapWindow(dpy, highlightB);
}

void Hider::Hide(Client* c) {
  hidden_.push_front(hiddenIDFor(c));

  // Actually hide the window.
  XUnmapWindow(dpy, c->parent);
  XUnmapWindow(dpy, c->window);

  c->hidden = true;

  // If the window was the current window, it isn't any more...
  if (c == current) {
    Client_Focus(NULL, CurrentTime);
  }
  c->SetState(IconicState);
}

void Hider::Unhide(Client* c) {
  // If anyone ever hides so many windows that we notice the O(n) scan, they're
  // doing something wrong.
  for (auto it = hidden_.begin(); it != hidden_.end(); ++it) {
    if (*it == c->parent) {
      hidden_.erase(it);
      c->hidden = false;
      break;
    }
  }
  // Always raise and give focus if we're trying to unhide, even if it wasn't
  // hidden.
  XMapWindow(dpy, c->parent);
  XMapWindow(dpy, c->window);
  Client_Raise(c);
  c->SetState(NormalState);
  // It feels right that the unhidden window gets focus always.
  Client_Focus(c, CurrentTime);
}

static int menuItemHeight() {
  return textHeight() + MENU_Y_PADDING;
}

static int menuIconYPad() {
  return 1;
}

static int menuIconXPad() {
  return 5;
}

static int menuIconSize() {
  return menuItemHeight() - menuIconYPad() * 2;
}

static int menuLMargin() {
  return menuItemHeight() + menuIconXPad() * 2;
}

static int menuRMargin() {
  return menuItemHeight();
}

static int menuMargins() {
  return menuLMargin() + menuRMargin();
}

// Returns val unless it's larger than max, in which case it returns max.
// If either val or max is < 0, returns 0.
static int clamp(int val, int max) {
  if (val > max) {
    val = max;
  }
  return val < 0 ? 0 : val;
}

void Hider::OpenMenu(XButtonEvent* e) {
  Client_ResetAllCursors();
  open_content_.clear();
  width_ = 0;

  // Add all hidden windows.
  std::set<Window> added;
  // It's possible for a client to disappear while hidden, for example if you
  // run 'sleep 5; exit' in an xterm, then hide it, you end up with a hidden
  // item with no Client. So while iterating over the hidden windows, we
  // also clean up any that have gone away.
  // Note: we have to handle the iteration carefully, as the 'erase' function
  // essentially iterates for us.
  for (std::list<Window>::iterator it = hidden_.begin(); it != hidden_.end();) {
    const Window w = *it;
    if (LScr::I->GetClient(w)) {
      open_content_.push_back(Item(w, true));
      added.insert(w);
      ++it;
    } else {
      it = hidden_.erase(it);  // Implicitly increments 'it'.
    }
  }

  // Add all other clients which haven't already been added.
  for (const auto& it : LScr::I->Clients()) {
    const Client* c = it.second;
    const Window w = hiddenIDFor(c);
    if (!c->framed || added.count(w)) {
      continue;
    }
    open_content_.push_back(Item(w, false));
    added.insert(w);
  }

  // Now we've got all clients in open_content_ in the right order, go through
  // and fill in their names, and find the longest.
  for (int i = 0; i < open_content_.size(); i++) {
    const Client* c = LScr::I->GetClient(open_content_[i].w);
    if (!c) {
      continue;
    }
    open_content_[i].name = c->MenuName();
    const int tw = textWidth(open_content_[i].name) + menuMargins();
    if (tw > width_) {
      width_ = tw;
    }
  }

  height_ = open_content_.size() * menuItemHeight();

  // Arrange for centre of first menu item to be under pointer,
  // unless that would put the menu off-screen.
  x_min_ = clamp(e->x - width_ / 2, LScr::I->Width() - width_);
  y_min_ = clamp(e->y - menuItemHeight() / 2, LScr::I->Height() - height_);

  current_item_ = itemAt(e->x_root, e->y_root);
  showHighlightBox(current_item_);
  mapAndRaise(LScr::I->Popup(), x_min_, y_min_, width_, height_);
  XChangeActivePointerGrab(dpy,
                           ButtonMask | ButtonMotionMask | OwnerGrabButtonMask,
                           None, CurrentTime);
  // This is how we tell disp.cc to direct messages our way.
  // TODO: try to figure out a nicer way to do this.
  mode = wm_menu_up;
}

int Hider::itemAt(int x, int y) const {
  x -= x_min_;
  y -= y_min_;
  if (x < 0 || y < 0 || x >= width_ || y >= height_) {
    return -1;
  }
  return y / menuItemHeight();
}

void Hider::Paint() {
  // We have to repaint from scratch. While this can cause a little flickering,
  // it's necessary to first blank the window background, so that we don't
  // corrupt our display when the red highlight box windows open and close over
  // the top of the menu.
  XClearWindow(dpy, LScr::I->Popup());
  const int itemHeight = menuItemHeight();
  const auto popup = LScr::I->Popup();
  const auto gc = LScr::I->GetMenuGC();
  for (int i = 0; i < open_content_.size(); i++) {
    const int y = i * itemHeight;
    const int textY = y + g_font->ascent + MENU_Y_PADDING / 2;
    drawString(popup, menuLMargin(), textY, open_content_[i].name,
               &g_font_popup_colour);
    // Show a dotted line to separate the last hidden window from the first
    // non-hidden one.
    if (!open_content_[i].hidden && (i == 0 || open_content_[i - 1].hidden)) {
      XSetLineAttributes(dpy, gc, 1, LineOnOffDash, CapButt, JoinMiter);
      XDrawLine(dpy, popup, gc, 0, y, width_, y);
    }

    Client* c = LScr::I->GetClient(open_content_[i].w);
    if (c && c->Icon()) {
      c->Icon()->Paint(popup, menuIconXPad(), y + menuIconYPad(),
                       menuIconSize(), menuIconSize());
    }
  }
  drawHighlight(current_item_);
}

void Hider::drawHighlight(int itemIndex) {
  if (itemIndex == -1) {
    return;
  }
  const int ih = menuItemHeight();
  const int y = itemIndex * ih;
  XFillRectangle(dpy, LScr::I->Popup(), LScr::I->GetMenuGC(), menuLMargin(), y,
                 width_ - menuMargins(), ih);
}

void Hider::MouseMotion(XEvent* ev) {
  const int old = current_item_;  // Old menu position.
  current_item_ = itemAt(ev->xbutton.x_root, ev->xbutton.y_root);
  if (current_item_ != old) {
    // In order to avoid too much flickering, and to avoid weird corruption
    // in our popup window, we first make the red highlight box disappear,
    // then update the menu item highlight (by EORing the old and new highlight
    // positions), and then finally reopen the red highlight box in its new
    // position. This seems to be the smoothest and least flickery/error-prone
    // way of updating the menu.
    hideHighlightBox();
    drawHighlight(old);
    drawHighlight(current_item_);
    showHighlightBox(current_item_);
  }
}

void Hider::MouseRelease(XEvent* ev) {
  hideHighlightBox();
  const int n = itemAt(ev->xbutton.x_root, ev->xbutton.y_root);
  XUnmapWindow(dpy, LScr::I->Popup());  // Hide popup window.
  if (n < 0) {
    return;  // User just released the mouse without having selected anything.
  }
  Client* c = LScr::I->GetClient(open_content_[n].w);
  if (c == nullptr) {
    return;  // Window must have disappeared, and we've lost the client.
  }
  Unhide(c);
  // Colourmap scum? Is the following really needed? I'd imagine this is the
  // wrong place for this to be done, anyway.
  if (current) {
    cmapfocus(current);
  }
}
