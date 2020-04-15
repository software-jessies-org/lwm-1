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
Window hiddenIDFor(const Client* c) { return c->parent; }

void mapAndRaise(Window w, int xmin, int ymin, int width, int height) {
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
  // Remove input focus, and drop from focus history.
  LScr::I->GetFocuser()->UnfocusClient(c);
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
  // Windows are given input focus when they're unhidden.
  LScr::I->GetFocuser()->FocusClient(c);

  // The following is to counteract a really weird bug which happens when
  // unhiding a window. To reproduce the bug (with the moveresize call below
  // commented out), do this:
  // 1: Move some window into a position where you can remember exactly where
  //    it was (eg it's obscuring some text in an underlying window at a
  //    specific point).
  // 2: Hide the window (right-click on title bar).
  // 3: Hold the right button over the root window to bring up the unhide menu -
  //    note that the red box is showing the correct location of the window.
  // 4: Release the right button to make the window reappear - note that without
  //    the following conditional code, the re-opened window appears
  //    approximately 'textHeight()' pixels lower than it previously was.
  if (c->framed) {
    XMoveResizeWindow(dpy, c->parent, c->size.x, c->size.y - textHeight(),
                      c->size.width, c->size.height + textHeight());
    XMoveWindow(dpy, c->window, borderWidth(), borderWidth() + textHeight());
    sendConfigureNotify(c);
  }
}

int menuItemHeight() { return textHeight() + MENU_Y_PADDING; }

int menuIconYPad() { return 1; }

int menuIconXPad() { return 5; }

int menuIconSize() { return menuItemHeight() - menuIconYPad() * 2; }

int menuLHighlight() { return menuItemHeight() + menuIconXPad() * 2; }

int menuRHighlight() { return menuItemHeight() - menuIconXPad(); }

int menuHighlightMargins() { return menuLHighlight() + menuRHighlight(); }

int menuLMargin() { return menuItemHeight() + menuIconXPad() * 3; }

int menuRMargin() { return menuItemHeight(); }

int menuMargins() { return menuLMargin() + menuRMargin(); }

// Returns val if it's within the range described by min and max, or min or
// max according to which side val extends off.
int clamp(int val, int min, int max) {
  if (val >= min && val < max) {
    return val;
  }
  return (val < min) ? min : max;
}

// visibleAreaAt returns the rectangle describing the current visible area which
// contains the given coordinates. This allows us to keep the popup menu within
// a single monitor at a time.
Rect visibleAreaAt(int x, int y) {
  for (const Rect& r : LScr::I->VisibleAreas(true)) {
    if (r.contains(x, y)) {
      return r;
    }
  }
  // Mouse pointer outside the screen? Weird. Anyway, just return the first one.
  return LScr::I->VisibleAreas(false)[0];
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
    const Client* c = LScr::I->GetClient(w);
    // The following check for c->IsHidden is mainly there to avoid listing
    // 'withdrawn' windows, but as we're constructing the list of windows which
    // are iconified, this is the right check to perform here.
    // Update(2020-04-15): Actually, checking for IsHidden here breaks all
    // window hiding, and causes hidden windows not to be visible in the unhide
    // menu.
    if (c/* && c->IsHidden()*/) {
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
    // The following check for c->IsNormal implicitly cuts out any windows which
    // are in withdrawn state.
    // This fixes a bug where Rhythmbox's preferences dialog would never
    // disappear from the list of windows, because it was withdrawn and kept,
    // and not destroyed.
    // To verify this bug is fixed, do the following:
    // 1: Open Rhythmbox.
    // 2: Open the Rhythmbox Preferences window.
    // 3: Verify the preferences window appears in the right-click unhide menu.
    // 4: Click on the X icon of the preferences window.
    // 5: Verify the preferences window no longer appears in the unhide menu.
    if (!c->framed || added.count(w) || !c->IsNormal()) {
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
  const Rect scr = visibleAreaAt(e->x, e->y);
  x_min_ = clamp(e->x - width_ / 2, scr.xMin, scr.xMax - width_);
  y_min_ = clamp(e->y - menuItemHeight() / 2, scr.yMin, scr.yMax - height_);

  current_item_ = itemAt(e->x_root, e->y_root);
  showHighlightBox(current_item_);
  mapAndRaise(LScr::I->Menu(), x_min_, y_min_, width_, height_);
  XChangeActivePointerGrab(dpy,
                           ButtonMask | ButtonMotionMask | OwnerGrabButtonMask,
                           None, CurrentTime);
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
  XClearWindow(dpy, LScr::I->Menu());
  const int itemHeight = menuItemHeight();
  const auto popup = LScr::I->Menu();
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
    if (c && c->Icon() && Resources::I->AppIconInUnhideMenu()) {
      c->Icon()->PaintMenu(popup, menuIconXPad(), y + menuIconYPad(),
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
  XFillRectangle(dpy, LScr::I->Menu(), LScr::I->GetMenuGC(), menuLHighlight(),
                 y, width_ - menuHighlightMargins(), ih);
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
  XUnmapWindow(dpy, LScr::I->Menu());
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
  cmapfocus(Client::FocusedClient());
}
