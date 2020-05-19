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

#include "ewmh.h"
#include "lwm.h"

void EvExpose(XEvent* ev) {
  // Only handle the last in a group of Expose events.
  if (ev->xexpose.count != 0) {
    return;
  }

  Window w = ev->xexpose.window;

  // We don't draw on the root window so that people can have
  // their favourite Spice Girls backdrop...
  if (w == LScr::I->Root()) {
    return;
  }

  // Decide what needs redrawing: window frame or menu?
  if (w == LScr::I->Popup()) {
    size_expose();
  } else if (w == LScr::I->Menu()) {
    LScr::I->GetHider()->Paint();
  } else {
    Client* c = LScr::I->GetClient(w);
    if (c != 0) {
      c->DrawBorder();
    }
  }
}

static DragHandler* current_dragger = nullptr;

// Use this to set or clear the drag handler. Will destroy the old handler if
// one is present. The new handler's Start() function is called with ev.
void startDragging(DragHandler* handler, XEvent* ev) {
  delete current_dragger;
  current_dragger = handler;
  if (current_dragger) {
    current_dragger->Start(ev);
  }
}

// Stops the dragging, calling the Stop handler of current_dragger if there is
// one.
void stopDragging(XEvent* ev) {
  if (ev && current_dragger) {
    current_dragger->End(ev);
  }
  startDragging(nullptr, nullptr);
}

class MenuDragger : public DragHandler {
 public:
  MenuDragger() = default;

  virtual void Start(XEvent* ev) {
    LScr::I->GetHider()->OpenMenu(&ev->xbutton);
  }

  virtual bool Move(XEvent* ev) {
    LScr::I->GetHider()->MouseMotion(ev);
    return true;
  }

  virtual void End(XEvent* ev) { LScr::I->GetHider()->MouseRelease(ev); }
};

// WindowDragger handles the shared bit of actions which involve dragging, such
// as moving or resizing windows.
// This class keeps track of where the mouse pointer was when the button was
// pressed, and where it ends up, and sends the offset from one to the other
// to the subclass.
// Subclasses must implement the moveImpl function.
class WindowDragger : public DragHandler {
 public:
  WindowDragger(Client* c) : window_(c->parent) {}

  virtual void Start(XEvent*) {
    start_pos_ = getMousePosition();
    LOGD(LScr::I->GetClient(window_))
        << "Window drag from " << start_pos_.x << ", " << start_pos_.y;
  }

  virtual bool Move(XEvent* ev) {
    Client* c = LScr::I->GetClient(window_);
    MousePos mp = getMousePosition();
    // Cancel everything if either the client has disappeared (window closed
    // while we were dragging it), or if we're somehow no longer holding the
    // mouse button down. This latter hack prevents randomly dragging around
    // windows due to a race condition in X.
    if (!c || !(mp.modMask & MOVING_BUTTON_MASK)) {
      // Either the client window closed underneath us, or we're somehow not
      // holding the mouse button down, despite not having seen the unclick
      // properly. In any case, cancel dragging.
      End(ev);
      return false;
    }
    moveImpl(c, mp.x - start_pos_.x, mp.y - start_pos_.y);
    return true;
  }

  virtual void moveImpl(Client* c, int dx, int dy) = 0;

  virtual void End(XEvent*) {
    MousePos mp = getMousePosition();
    LOGD(LScr::I->GetClient(window_))
        << "Window drag to " << mp.x << ", " << mp.y << " (moved "
        << (mp.x - start_pos_.x) << ", " << (mp.y - start_pos_.y) << ")";
    // Unmapping the popup only has an effect if it's open (so if this is a
    // resize instead of a move), but it doesn't hurt to always close it.
    xlib::XUnmapWindow(LScr::I->Popup());
  }

 private:
  // LWM's frame window.
  Window window_;
  MousePos start_pos_;
};

class WindowMover : public WindowDragger {
 public:
  WindowMover(Client* c)
      : WindowDragger(c),
        start_frame_rect_(c->FrameRect()),
        start_content_rect_(c->ContentRect()) {}

  virtual void moveImpl(Client* c, int dx, int dy) {
    Rect r = Rect::Translate(start_frame_rect_, Point{dx, dy});
    // Implement edge resistance for all of the visible areas. There can be
    // several if we're using multiple monitors with xrandr, and they can be
    // offset from each other. However, for each box, ensure that some part of
    // the window is interacting with an edge.
    // We're going to assume we always have to take struts into account, because
    // if a window has struts set, it should be movable anyway.
    // We only pick the first spotted edge correction factor. Otherwise if there
    // are two screens, say, next to each other, with the same bottom location,
    // a window that is dragged towards the bottom over the boundary will be
    // affected twice, resulting in weird effects (the window moving upwards as
    // it's dragged downwards).
    int orig_dx = dx;
    int orig_dy = dy;
    for (const auto& vis : LScr::I->VisibleAreas(true)) {
      // Check for top/bottom if the horizontal location of the window overlaps
      // with that of the screen area.
      if ((orig_dy == dy) && (r.xMin < vis.xMax) && (r.xMax > vis.xMin)) {
        dy += getResistanceOffset(vis.yMin - r.yMin);  // Top.
        dy -= getResistanceOffset(r.yMax - vis.yMax);  // Bottom.
      }
      // Check for left/right if the vertical location of the window overlaps
      // with that of the screen area.
      if ((orig_dx == dx) && (r.yMin < vis.yMax) && (r.yMax > vis.yMin)) {
        dx += getResistanceOffset(vis.xMin - r.xMin);  // Left.
        dx -= getResistanceOffset(r.xMax - vis.xMax);  // Right.
      }
    }
    c->MoveTo(Rect::Translate(start_content_rect_, Point{dx, dy}));
  }

 private:
  // The diff is expected to be the difference between a window position and
  // some barrier (eg edge of a screen). If that difference is within
  // 0..EDGE_RESIST, we return it; otherwise we return 0. This makes the code to
  // apply edge resistance a simple matter of subtracting or adding the returned
  // value.
  int getResistanceOffset(int diff) {
    if (diff <= 0 || diff > EDGE_RESIST) {
      return 0;
    }
    return diff;
  }

  const Rect start_frame_rect_;
  const Rect start_content_rect_;
};

class WindowResizer : public WindowDragger {
 public:
  WindowResizer(Client* c, Edge edge)
      : WindowDragger(c), edge_(edge), start_content_rect_(c->ContentRect()) {}

  virtual void moveImpl(Client* c, int dx, int dy) {
    Client_SizeFeedback();
    Rect ns = start_content_rect_;
    // Vertical.
    if (isTopEdge(edge_)) {
      ns.yMin += dy;
    }
    if (isBottomEdge(edge_)) {
      ns.yMax += dy;
    }

    // Horizontal.
    if (isLeftEdge(edge_)) {
      ns.xMin += dx;
    }
    if (isRightEdge(edge_)) {
      ns.xMax += dx;
    }
    // The client knows what the rules are regarding min/max size, and size
    // increments (eg. integer numbers of characters in xterm). Let it limit our
    // suggested size, so as to avoid unpleasantness.
    ns = c->LimitResize(ns);
    c->MoveResizeTo(ns);
  }

 private:
  const Edge edge_;
  const Rect start_content_rect_;
};

// Max distance between click and release, for closing, iconising etc.
#define MAX_CLICK_DISTANCE 4

// WindowClicker is a dragger that handles cases when we want to deal with
// simple clicks.
// Such actions include closing, hiding or lowering the window, but we only take
// action after the mouse is released, in case the user changes his or her mind.
// This class also checks the distance the mouse has moved, and only if it is
// released close to where it was pressed will it trigger the action.
class WindowClicker : public DragHandler {
 public:
  WindowClicker(Client* c) : window_(c->parent) {}
  virtual void Start(XEvent*) { start_pos_ = getMousePosition(); }
  virtual bool Move(XEvent*) { return true; }

  virtual void End(XEvent*) {
    MousePos mp = getMousePosition();
    const int dx = std::abs(start_pos_.x - mp.x);
    const int dy = std::abs(start_pos_.y - mp.y);
    if (std::max(dx, dy) > MAX_CLICK_DISTANCE) {
      // Cancelled by mouse pointer having moved too far away.
      return;
    }
    Client* c = LScr::I->GetClient(window_);
    // Check if client still exists.
    if (c) {
      act(c);
    }
  }

  virtual void act(Client* c) = 0;

 private:
  // LWM's frame window.
  Window window_;
  MousePos start_pos_;
};

class WindowCloser : public WindowClicker {
 public:
  WindowCloser(Client* c) : WindowClicker(c) {}
  virtual void act(Client* c) {
    LOGD(c) << "Closing (user action)";
    Client_Close(c);
  }
};

class WindowHider : public WindowClicker {
 public:
  WindowHider(Client* c) : WindowClicker(c) {}
  virtual void act(Client* c) {
    LOGD(c) << "Hiding (user action)";
    c->Hide();
  }
};

class WindowLowerer : public WindowClicker {
 public:
  WindowLowerer(Client* c) : WindowClicker(c) {}
  virtual void act(Client* c) {
    LOGD(c) << "Lowering (user action)";
    Client_Lower(c);
  }
};

class ShellRunner : public DragHandler {
 public:
  explicit ShellRunner(int button) : button_(button) {}
  virtual void Start(XEvent*) { shell(button_); }
  virtual bool Move(XEvent*) { return false; }
  virtual void End(XEvent*) {}

 private:
  int button_;
};

DragHandler* getDragHandlerForEvent(XEvent* ev) {
  XButtonEvent* e = &ev->xbutton;
  // Deal with root window button presses.
  if (e->window == e->root) {
    if (e->button == Button3) {
      return new MenuDragger;
    }
    return new ShellRunner(e->button);
  }

  Client* c = LScr::I->GetClient(e->window);
  if (c == nullptr) {
    return nullptr;
  }
  if (Resources::I->ClickToFocus()) {
    LScr::I->GetFocuser()->FocusClient(c);
  }

  // move this test up to disable scroll to focus
  if (e->button >= 4 && e->button <= 7) {
    return nullptr;
  }
  const Edge edge = c->EdgeAt(e->window, e->x, e->y);
  if (edge == EContents) {
    return nullptr;
  }
  if (edge == EClose) {
    return new WindowCloser(c);
  }

  // Somewhere in the rest of the frame.
  if (e->button == HIDE_BUTTON) {
    if (e->state & ShiftMask) {
      return new WindowLowerer(c);
    }
    return new WindowHider(c);
  }
  if (e->button == MOVE_BUTTON) {
    // If we're moving the window because the user has used the 'move' button
    // (generally middle), then force the mouse pointer to turn into the move
    // pointer, even if it's over an area of the window furniture which usually
    // has another pointer.
    XChangeActivePointerGrab(dpy,
                             ButtonMask | PointerMotionHintMask |
                                 ButtonMotionMask | OwnerGrabButtonMask,
                             LScr::I->Cursors()->ForEdge(ENone), CurrentTime);
    return new WindowMover(c);
  }
  if (e->button == RESHAPE_BUTTON) {
    Client_Raise(c);
    if (edge == ENone) {
      return new WindowMover(c);
    }
    return new WindowResizer(c, edge);
  }
  return nullptr;
}

void EvButtonPress(XEvent* ev) {
  if (current_dragger) {
    LOGI() << "Already doing something";
    return;  // Already doing something.
  }
  startDragging(getDragHandlerForEvent(ev), ev);
}

void EvButtonRelease(XEvent* ev) {
  stopDragging(ev);
}

void EvCirculateRequest(XEvent* ev) {
  XCirculateRequestEvent* e = &ev->xcirculaterequest;
  Client* c = LScr::I->GetClient(e->window);
  LOGD(c) << "CirculateRequest";
  if (c == 0) {
    if (e->place == PlaceOnTop) {
      xlib::XRaiseWindow(e->window);
    } else {
      xlib::XLowerWindow(e->window);
    }
  } else {
    if (e->place == PlaceOnTop) {
      Client_Raise(c);
    } else {
      Client_Lower(c);
    }
  }
}

void EvMapRequest(XEvent* ev) {
  XMapRequestEvent* e = &ev->xmaprequest;
  Client* c = LScr::I->GetOrAddClient(e->window, false);
  LOGD(c) << "MapRequest";
  if (c->hidden) {
    c->Unhide();
  }

  switch (c->State()) {
    case WithdrawnState:
      if (c->parent == LScr::I->Root()) {
        LOGD(c) << "(map) taking management of " << WinID(c->window);
        manage(c);
        LScr::I->GetFocuser()->FocusClient(c);
        break;
      }
      if (c->framed) {
        LOGD(c) << "(map) reparenting framed window " << WinID(c->parent);
        xlib::XReparentWindow(c->window, c->parent, borderWidth(),
                              borderWidth() + textHeight());
      } else {
        LOGD(c) << "(map) reparenting unframed window " << WinID(c->parent);
        const Point p = c->ContentRect().origin();
        xlib::XReparentWindow(c->window, c->parent, p.x, p.y);
      }
      XAddToSaveSet(dpy, c->window);
    // FALLTHROUGH
    case NormalState:
      LOGD(c) << "(map) NormalState " << WinID(c->window);
      xlib::XMapWindow(c->parent);
      xlib::XMapWindow(c->window);
      Client_Raise(c);
      c->SetState(NormalState);
      c->SendConfigureNotify();
      break;
  }
  ewmh_set_client_list();
}

char const* truth(bool yes) {
  return yes ? "true" : "false";
}

void EvUnmapNotify(XEvent* ev) {
  const XUnmapEvent& xe = ev->xunmap;
  Client* c = LScr::I->GetClient(xe.window);
  if (c == nullptr) {
    return;
  }
  // Be careful here. We only want to respond to unmaps on client windows that
  // we're managing. For example, if this isn't the direct client window,
  // then do nothing.
  if (c->window != xe.window) {
    return;
  }
  // Plus, when we reparent the client window to our frame, we'll receive an
  // unmap notification with window=child window, and parent=root. Check for
  // this, and ignore it.
  if (xe.event == LScr::I->Root()) {
    return;
  }
  // If we got here, then this is a client withdrawing its own window that we
  // have ourselves re-framed. We therefore withdraw ourselves.
  LOGD(c) << "Withdrawing unmapped window";
  withdraw(c);
}

std::ostream& operator<<(std::ostream& os, const XConfigureRequestEvent& e) {
  os << "ConfigRequestEvent " << WinID(e.window) << " (parent "
     << WinID(e.parent) << ")";
#define OUT(flag, var)     \
  if (e.value_mask & flag) \
    os << " " #var "->" << e.var;
  OUT(CWX, x);
  OUT(CWY, y);
  OUT(CWWidth, width);
  OUT(CWHeight, height);
  OUT(CWBorderWidth, border_width);
  OUT(CWSibling, above);
  OUT(CWStackMode, detail);
#undef OUT
  return os;
}

int absDist(int min1, int max1, int min2, int max2) {
  if (min1 > max2) {
    return min1 - max2;
  }
  if (min2 > max1) {
    return min2 - max1;
  }
  return 0;
}

Rect findBestScreenFor(const Rect& r, bool withStruts) {
  const std::vector<Rect> vis = LScr::I->VisibleAreas(withStruts);
  // First try to find the one with the largest overlap.
  Rect res{};
  int ra = 0;
  for (Rect v : vis) {
    const int area = Rect::Intersect(r, v).area().num_pixels();
    if (area > ra) {
      ra = area;
      res = v;
    }
  }
  // If we found an overlapping screen, return it.
  if (ra) {
    return res;
  }
  // Found no overlapping screens. Try again, this time picking the screen
  // closest to the query rectangle.
  int rd = INT_MAX;
  for (Rect v : vis) {
    // These will be absolute distances.
    const int xd = absDist(r.xMin, r.xMax, v.xMin, v.xMax);
    const int yd = absDist(r.yMin, r.yMax, v.yMin, v.yMax);
    // Just sum the distances; it'll do.
    const int d = xd + yd;
    if (d < rd) {
      rd = d;
      res = v;
    }
  }
  return res;
}

Rect makeVisible(Rect r, bool withStruts) {
  const Rect scr = findBestScreenFor(r, withStruts);
  Point translation{};
  if (r.width() >= scr.width()) {
    r.xMin = scr.xMin;
    r.xMax = scr.xMax;
  } else if (r.xMax > scr.xMax) {
    translation.x = scr.xMax - r.xMax;
  } else if (r.xMin < scr.xMin) {
    translation.x = scr.xMin - r.xMin;
  }
  if (r.height() >= scr.height()) {
    r.yMin = scr.yMin;
    r.yMax = scr.yMax;
  } else if (r.yMax > scr.yMax) {
    translation.y = scr.yMax - r.yMax;
  } else if (r.yMin < scr.yMin) {
    translation.y = scr.yMin - r.yMin;
  }
  return Rect::Translate(r, translation);
}

void EvConfigureRequest(XEvent* ev) {
  const XConfigureRequestEvent& e = ev->xconfigurerequest;
  // There are several situations in which we can receive a configure request.
  // Two of these are:
  //  1: The client is setting up the initial size, before mapping the window.
  //  2: The client has its own built-in window parts by which the user can
  //     drag the window around (eg the Nautilus file browser).
  //
  // The basic thing we have to do is to allow the configure request to go
  // through, and affect the client's window. So let's do that.
  // Now, let's check if we're already framed and visible. If so, we also move
  // around our frame. In this case we won't bother checking if the bounds are
  // sensible, as the client's making this request.
  Client* c = LScr::I->GetClient(e.window, false);
  if (c == nullptr || c->State() != NormalState || !c->framed) {
    XWindowChanges wc{};
    wc.x = e.x;
    wc.y = e.y;
    wc.width = e.width;
    wc.height = e.height;
    wc.border_width = e.border_width;
    wc.sibling = e.above;
    wc.stack_mode = e.detail;
    xlib::XConfigureWindow(e.window, e.value_mask, &wc);
    return;
  }
  LOGD(c) << "ConfigureRequest: " << e;
  if (!c->framed) {
    return;
  }
  // Current situation with Nautilus is:
  // When dragging to move, the *first* configure request has x,y = the relative
  // position of the content window with respect to the frame window. All later
  // requests are relative to the root. When dragging to resize (bottom edge,
  // just above LWM's boundary), the first request has x, y = the frame origin,
  // which makes the client window jump up and to the left, so its origin
  // becomes that of the frame origin. Bloody weird. My best guess is that
  // there's some client notify message that should be sent in order to let the
  // client know where it should be placing things. Changing the move handling
  // to add the frame rect's origin to the provided rect does *not* work - after
  // the initial positioning, the window quickly zooms miles off out of scope of
  // the screen.
  // Now intercept the reconfigure and turn it into a move under our system.
  // Only do anything if one of the size/position values changes.
  if ((e.value_mask & (CWX | CWY | CWWidth | CWHeight)) == 0) {
    return;
  }
  Rect new_rect = c->ContentRect();
  // ICCCM section 4.1.5 says that the x and y coordinates here
  // will have been "adjusted for the border width".
  // In the case of reparenting (which we have done), it seems that this means
  // the client provides the x and y coordinates of the top-left of the parent
  // frame window (ie. the one we created and stuck the client inside).
  // The ICCCM documentation doesn't make this clear, but testing with the
  // Nautilus file manager suggests that this is the correct interpretation.
  // It's damned weird is all I can say.
  // Anyway, if you ever feel like changing this behaviour, ensure you test with
  // Nautilus. Try dragging the window about by the thing's own internal top
  // bar, or try resizing at the pixel or two just on the inside of the client
  // window. With this offset-handling hack in place, the position of the
  // window should change in a manner one might expect; without it, any drags
  // inside the client window will cause the window (frame and all) to jump up
  // and to the left by an amount corresponding to the offset from frame to
  // client window.
  const Point offset = c->ContentRectRelative().origin();
  if (e.value_mask & CWX) {
    int diff = (e.x + offset.x) - new_rect.xMin;
    new_rect.xMin += diff;
    new_rect.xMax += diff;
  }
  if (e.value_mask & CWY) {
    int diff = (e.y + offset.y) - new_rect.yMin;
    new_rect.yMin += diff;
    new_rect.yMax += diff;
  }
  if (e.value_mask & CWWidth) {
    new_rect.xMax = new_rect.xMin + e.width;
  }
  if (e.value_mask & CWHeight) {
    new_rect.yMax = new_rect.yMin + e.height;
  }

  XWindowChanges wc{};
  c->FrameRect().To(wc);
  wc.border_width = 1;
  wc.sibling = e.above;
  wc.stack_mode = e.detail;
  xlib::XConfigureWindow(e.parent, e.value_mask, &wc);
  c->SendConfigureNotify();

  c->ContentRectRelative().To(wc);
  wc.border_width = 0;
  xlib::XConfigureWindow(e.window, e.value_mask, &wc);

  if (new_rect.area() == c->ContentRect().area()) {
    c->MoveTo(new_rect);
  } else {
    c->MoveResizeTo(new_rect);
  }
}

std::ostream& operator<<(std::ostream& os, const XConfigureEvent& e) {
  os << WinID(e.window) << " " << (e.send_event ? "S" : "s") << e.serial << " ";
  os << Rect::FromXYWH(e.x, e.y, e.width, e.height) << ", b=" << e.border_width;
  return os;
}

void EvConfigureNotify(XEvent*) {}

void EvDestroyNotify(XEvent* ev) {
  Window w = ev->xdestroywindow.window;
  // Request the client, but without scanning this window's parents for it.
  // The window is gone, so any attempt to scan the window tree will result in
  // errors.
  Client* c = LScr::I->GetClient(w, false);
  if (c == 0) {
    return;
  }

  ScopedIgnoreBadWindow ignorer;
  Client_Remove(c);
}

void EvClientMessage(XEvent* ev) {
  XClientMessageEvent* e = &ev->xclient;
  Client* c = LScr::I->GetClient(e->window);
  if (c == 0) {
    return;
  }
  if (e->message_type == wm_change_state) {
    if (e->format == 32 && e->data.l[0] == IconicState && c->IsNormal()) {
      LOGD(c) << "Client message: requested hide";
      c->Hide();
    }
    return;
  }
  if (e->message_type == ewmh_atom[_NET_WM_STATE] && e->format == 32) {
    LOGD(c) << "Client message: WM state change: " << e->data.l[0] << " -> "
            << e->data.l[1] << ", " << e->data.l[2];
    ewmh_change_state(c, e->data.l[0], e->data.l[1]);
    ewmh_change_state(c, e->data.l[0], e->data.l[2]);
    return;
  }
  if (e->message_type == ewmh_atom[_NET_ACTIVE_WINDOW] && e->format == 32) {
    LOGD(c) << "Client message: requested set active: unhiding";
    // An EWMH enabled application has asked for this client to be made the
    // active window. Unhide also raises and gives focus to the window.
    c->Unhide();
    return;
  }
  if (e->message_type == ewmh_atom[_NET_CLOSE_WINDOW] && e->format == 32) {
    LOGD(c) << "Client message: requested close";
    Client_Close(c);
    return;
  }
  if (e->message_type == ewmh_atom[_NET_MOVERESIZE_WINDOW] && e->format == 32) {
    XEvent ev;

    // FIXME: ok, so this is a bit of a hack
    ev.xconfigurerequest.window = e->window;
    ev.xconfigurerequest.x = e->data.l[1];
    ev.xconfigurerequest.y = e->data.l[2];
    ev.xconfigurerequest.width = e->data.l[3];
    ev.xconfigurerequest.height = e->data.l[4];
    ev.xconfigurerequest.value_mask = 0;
    if (e->data.l[0] & (1 << 8)) {
      ev.xconfigurerequest.value_mask |= CWX;
    }
    if (e->data.l[0] & (1 << 9)) {
      ev.xconfigurerequest.value_mask |= CWY;
    }
    if (e->data.l[0] & (1 << 10)) {
      ev.xconfigurerequest.value_mask |= CWWidth;
    }
    if (e->data.l[0] & (1 << 11)) {
      ev.xconfigurerequest.value_mask |= CWHeight;
    }
    LOGD(c) << "Client message: move/resize -> " << ev.xconfigurerequest
            << " (flags " << ev.xconfigurerequest.value_mask << ")";
    EvConfigureRequest(&ev);
    return;
  }
  if (e->message_type == ewmh_atom[_NET_WM_MOVERESIZE] && e->format == 32) {
    LOGD(c) << "Client message: requested _NET_WM_MOVERESIZE";
    Edge edge = E_LAST;
    EWMHDirection direction = (EWMHDirection)e->data.l[2];

    // before we can do any resizing, make the window visible
    if (c->IsHidden()) {
      c->Unhide();
    }
    xlib::XMapWindow(c->parent);
    Client_Raise(c);
    // FIXME: we're ignoring x_root, y_root and button!
    switch (direction) {
      case DSizeTopLeft:
        edge = ETopLeft;
        break;
      case DSizeTop:
        edge = ETop;
        break;
      case DSizeTopRight:
        edge = ETopRight;
        break;
      case DSizeRight:
        edge = ERight;
        break;
      case DSizeBottomRight:
        edge = EBottomRight;
        break;
      case DSizeBottom:
        edge = EBottom;
        break;
      case DSizeBottomLeft:
        edge = EBottomLeft;
        break;
      case DSizeLeft:
        edge = ELeft;
        break;
      case DMove:
        edge = ENone;
        break;
      case DSizeKeyboard:
        // FIXME: don't know how to deal with this
        edge = E_LAST;
        break;
      case DMoveKeyboard:
        edge = E_LAST;
        break;
      default:
        edge = E_LAST;
        fprintf(stderr,
                "%s: received _NET_WM_MOVERESIZE"
                " with bad direction",
                argv0);
        break;
    }
    switch (edge) {
      case E_LAST:
        break;
      case ENone:
        // Should do a move, but this currently can't work because we only allow
        // the move to continue while a mouse button is pressed. We should
        // consider adding back this functionality, but for now it's not used
        // and won't work.
        break;
      default:
        // Same here, this functionality can't work right now. Need to fix it.
        break;
    }
  }
}

struct diff {
  EWMHWindowState o;
  EWMHWindowState n;
};

std::ostream& operator<<(std::ostream& os, const diff& d) {
  bool changed = false;
#define D(x)                                            \
  do {                                                  \
    if (d.o.x != d.n.x) {                               \
      changed = true;                                   \
      os << " " #x << " " << (d.o.x ? "t->f" : "f->t"); \
    }                                                   \
  } while (false)
  D(skip_taskbar);
  D(skip_pager);
  D(fullscreen);
  D(above);
  D(below);
#undef D
  if (!changed) {
    os << " no changes (" << d.n << ")";
  }
  return os;
}

std::ostream& operator<<(std::ostream& os, const EWMHWindowState& s) {
#define D(x) os << " " #x << (s.x ? "=t" : "=f")
  D(skip_taskbar);
  D(skip_pager);
  D(fullscreen);
  D(above);
  D(below);
#undef D
  return os;
}

void EvPropertyNotify(XEvent* ev) {
  XPropertyEvent* e = &ev->xproperty;
  Client* c = LScr::I->GetClient(e->window);
  if (c == 0) {
    return;
  }
  // This function can be called for a window that has already been destroyed.
  // That's fine; we'll expect functions we call to handle errors properly, but
  // we'll stomp on the printing of error logs.
  ScopedIgnoreBadWindow ignorer;

  if (e->atom == _mozilla_url || e->atom == XA_WM_NAME) {
    LOGD(c) << "Property change: XA_WM_NAME";
    getWindowName(c);
  } else if (e->atom == XA_WM_TRANSIENT_FOR) {
    LOGD(c) << "Property change: XA_WM_TRANSIENT_FOR";
    getTransientFor(c);
  } else if (e->atom == XA_WM_NORMAL_HINTS) {
    LOGD(c) << "Property change: XA_WM_NORMAL_HINTS";
    // XXXXXXXXXXXXXXXXXX Reset the hints used for window sizing.
    // getNormalHints(c);
  } else if (e->atom == ewmh_atom[_NET_WM_STRUT]) {
    LOGD(c) << "Property change: _NET_WM_STRUT";
    ewmh_get_strut(c);
  } else if (e->atom == ewmh_atom[_NET_WM_STATE]) {
    const EWMHWindowState old = c->wstate;
    ewmh_get_state(c);
    LOGD(c) << "Property change: _NET_WM_STATE:" << diff{old, c->wstate};
    if (c->wstate.fullscreen && !old.fullscreen) {
      c->EnterFullScreen();
    } else if (!c->wstate.fullscreen && old.fullscreen) {
      c->ExitFullScreen();
    }
  }
}

void EvReparentNotify(XEvent* ev) {
  XReparentEvent* e = &ev->xreparent;
  if (e->event != LScr::I->Root() || e->override_redirect ||
      e->parent == LScr::I->Root()) {
    return;
  }

  Client* c = LScr::I->GetClient(e->window);
  LOGD(c) << "ReparentNotify to " << WinID(c->parent);
  if (c != 0 && (c->parent == LScr::I->Root() || c->IsWithdrawn())) {
    Client_Remove(c);
  }
}

std::string describeFocusMode(int mode) {
  switch (mode) {
#define CASE_RETURN(x) \
  case x:              \
    return #x
    CASE_RETURN(NotifyNormal);
    CASE_RETURN(NotifyGrab);
    CASE_RETURN(NotifyUngrab);
#undef CASE_RETURN
  }
  return "Unknown";
}

std::string describeFocusDetail(int detail) {
  switch (detail) {
#define CASE_RETURN(x) \
  case x:              \
    return #x
    CASE_RETURN(NotifyAncestor);
    CASE_RETURN(NotifyVirtual);
    CASE_RETURN(NotifyInferior);
    CASE_RETURN(NotifyNonlinear);
    CASE_RETURN(NotifyNonlinearVirtual);
    CASE_RETURN(NotifyPointer);
    CASE_RETURN(NotifyPointerRoot);
    CASE_RETURN(NotifyDetailNone);
#undef CASE_RETURN
  }
  return "Unknown";
}

std::ostream& operator<<(std::ostream& os, const XFocusChangeEvent& e) {
  os << WinID(e.window) << " (serial: " << e.serial
     << ") mode: " << describeFocusMode(e.mode)
     << ", detail: " << describeFocusDetail(e.detail);
  return os;
}

void EvFocusIn(XEvent* ev) {
  // In practice, XGetInputFocus returns the child window that actually has
  // focus (in Java apps, the 'FocusProxy' window), while the XEvent reports
  // the top-level window.
  Window focus_window;
  int revert_to;
  XGetInputFocus(dpy, &focus_window, &revert_to);
  // There seems to be a bug in the Xserver, whereupon for the first focus-in
  // event we receive, XGetInputFocus returns focus_window==1, which doesn't
  // correspond to any actual window. In this case, fall back to the window
  // which was specified in the event itself.
  // Without this hack, the first time we change focus after running LWM, we
  // get a spurious error due to trying to look up the parents of window 1.
  if (focus_window == 1) {
    focus_window = ev->xfocus.window;
  }
  Client* c = LScr::I->GetClient(focus_window);
  if (c) {
    LOGD(c) << "  focusing client; focus window = " << WinID(focus_window);
    LScr::I->GetFocuser()->FocusClient(c);
  }
}

void EvFocusOut(XEvent*) {}

void EvEnterNotify(XEvent* ev) {
  if (current_dragger) {
    return;
  }
  LScr::I->GetFocuser()->EnterWindow(ev->xcrossing.window, ev->xcrossing.time);
  // We receive enter events for our client windows too. When we do, we need
  // to switch the mouse pointer's shape to the default pointer.
  // If we don't do this, then for apps like Rhythmbox which don't
  // aggressively set the pointer to their preferred shape, we end up showing
  // silly icons, such as the 'resize corner' icon, while hovering over the
  // middle of the application window.
  Client* c = LScr::I->GetClient(ev->xcrossing.window);
  if (c == nullptr) {
    return;
  }
  if (ev->xcrossing.window != c->parent) {
    // TODO: add a SetCursor method to Client, so we don't have to keep
    // repeating this code everywhere.
    XSetWindowAttributes attr;
    attr.cursor = LScr::I->Cursors()->Root();
    xlib::XChangeWindowAttributes(c->parent, CWCursor, &attr);
    // Record that the current cursor is whatever the child window says it is.
    // This has to be different from any Edge we want to trigger when the mouse
    // crosses window furniture, otherwise we may fail to trigger a cursor
    // switch. For example, were we to set this to ENone, if the mouse were to
    // cross from the client window into the title bar, we'd fail to switch to
    // the 'move window' cursor.
    c->cursor = EContents;
  }
}

void EvMotionNotify(XEvent* ev) {
  if (current_dragger) {
    if (!current_dragger->Move(ev)) {
      current_dragger = nullptr;
    }
    return;
  }
  XMotionEvent* e = &ev->xmotion;
  Client* c = LScr::I->GetClient(e->window);
  if (c == nullptr) {
    return;
  }
  if ((e->window == c->parent) && (e->subwindow != c->window)) {
    Edge edge = c->EdgeAt(e->window, e->x, e->y);
    if (edge != EContents && c->cursor != edge) {
      XSetWindowAttributes attr;
      attr.cursor = LScr::I->Cursors()->ForEdge(edge);
      xlib::XChangeWindowAttributes(c->parent, CWCursor, &attr);
      c->cursor = edge;
    }
  }
}

extern void DispatchXEvent(XEvent* ev) {
  switch (ev->type) {
#define EV(x)  \
  case x:      \
    Ev##x(ev); \
    break

    EV(Expose);
    EV(MotionNotify);
    EV(ButtonPress);
    EV(ButtonRelease);
    EV(FocusIn);
    EV(FocusOut);
    EV(MapRequest);
    EV(ConfigureRequest);
    EV(UnmapNotify);
    EV(DestroyNotify);
    EV(ClientMessage);
    EV(PropertyNotify);
    EV(ReparentNotify);
    EV(EnterNotify);
    EV(CirculateRequest);
    EV(ConfigureNotify);
#undef EV

    case LeaveNotify:
    case CreateNotify:
    case GravityNotify:
    case MapNotify:
    case MappingNotify:
    case SelectionClear:
    case SelectionNotify:
    case SelectionRequest:
    case NoExpose:
      break;
    default:
      LOGI_IF(!shapeEvent(ev)) << "unknown event " << ev->type;
  }
}
