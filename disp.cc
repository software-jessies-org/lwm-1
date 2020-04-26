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
  WindowMover(Client* c) : WindowDragger(c) {
    window_start_ = {c->size.x, c->size.y};
  }

  virtual void moveImpl(Client* c, int dx, int dy) {
    const int new_x = window_start_.x + dx;
    const int new_y = window_start_.y + dy;
    Client_MakeSaneAndMove(c, ENone, new_x, new_y, 0, 0);
  }

 private:
  Point window_start_;
};

class WindowResizer : public WindowDragger {
 public:
  WindowResizer(Client* c, Edge edge) : WindowDragger(c), edge_(edge) {
    orig_size_ = c->size;
  }

  virtual void moveImpl(Client* c, int dx, int dy) {
    Client_SizeFeedback();
    XSizeHints ns = orig_size_;
    // Vertical.
    if (isTopEdge(edge_)) {
      ns.y += dy;
      ns.height -= dy;
    }
    if (isBottomEdge(edge_)) {
      ns.height += dy;
    }

    // Horizontal.
    if (isLeftEdge(edge_)) {
      ns.x += dx;
      ns.width -= dx;
    }
    if (isRightEdge(edge_)) {
      ns.width += dx;
    }
    Client_MakeSaneAndMove(c, edge_, ns.x, ns.y, ns.width, ns.height);
  }

 private:
  Edge edge_;
  XSizeHints orig_size_;
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
    xlib::XMapWindow(c->parent);
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
  Client* c = LScr::I->GetOrAddClient(e->window);
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
        xlib::XReparentWindow(c->window, c->parent, c->size.x, c->size.y);
      }
      XAddToSaveSet(dpy, c->window);
    // FALLTHROUGH
    case NormalState:
      LOGD(c) << "(map) NormalState " << WinID(c->window);
      xlib::XMapWindow(c->parent);
      xlib::XMapWindow(c->window);
      Client_Raise(c);
      c->SetState(NormalState);
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
  LOGD(c) << "UnmapNotify send_event=" << truth(xe.send_event)
          << " event=" << WinID(xe.event) << "; window=" << WinID(xe.window)
          << "; from_configure=" << truth(xe.from_configure);
  if (c == nullptr) {
    return;
  }

  // In the description of the ReparentWindow request we read: "If the window
  // is mapped, an UnmapWindow request is performed automatically first". This
  // might seem stupid, but it's the way it is. While a reparenting is pending
  // we ignore UnmapWindow requests.
  if (c->internal_state != IPendingReparenting) {
    LOGD(c) << "Internal state is normal; withdrawing";
    withdraw(c);
  }
  c->internal_state = INormal;
}

std::ostream& operator<<(std::ostream& os, const XConfigureRequestEvent& e) {
  os << (e.send_event ? "S" : "s") << e.serial << WinID(e.window) << " "
     << Rect::FromXYWH(e.x, e.y, e.width, e.height) << ", b=" << e.border_width;
  return os;
}

std::ostream& operator<<(std::ostream& os, const XWindowChanges& c) {
  os << Rect::FromXYWH(c.x, c.y, c.width, c.height) << ", b=" << c.border_width;
  return os;
}

struct XCfgValMask {
  explicit XCfgValMask(unsigned m) : m(m) {}
  unsigned long m;
};

char upper(char c, bool up) {
  return up ? toupper(c) : tolower(c);
}

std::ostream& operator<<(std::ostream& os, const XCfgValMask& m) {
#define OP(flag, ch) os << upper(ch, m.m & flag)
  OP(CWX, 'x');
  OP(CWY, 'y');
  OP(CWWidth, 'w');
  OP(CWHeight, 'h');
  OP(CWBorderWidth, 'b');
  OP(CWSibling, 'i');
  OP(CWStackMode, 't');
#undef OP
  return os;
}

void moveResize(Window w, const Rect& r) {
  xlib::XMoveResizeWindow(w, r.xMin, r.yMin, r.width(), r.height());
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
    const int area = Rect::Intersect(r, v).area();
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
  XWindowChanges wc{};
  XConfigureRequestEvent* e = &ev->xconfigurerequest;
  Client* c = LScr::I->GetClient(e->window);
  if (c == nullptr) {
    return;
  }
  LOGD(c) << "ConfigureRequest: " << *e;

  if (c->window == e->window) {
    // ICCCM section 4.1.5 says that the x and y coordinates here
    // will have been "adjusted for the border width".
    // NOTE: this may not be the only place to bear this in mind.
    if (e->value_mask & CWBorderWidth) {
      e->x -= e->border_width;
      e->y -= e->border_width;
    }

    if (e->value_mask & CWX) {
      c->size.x = e->x;
    }
    if (e->value_mask & CWY) {
      c->size.y = e->y;
      if (c->framed) {
        c->size.y += textHeight();
      }
    }
    if (e->value_mask & CWWidth) {
      c->size.width = e->width;
      if (c->framed) {
        c->size.width += 2 * borderWidth();
      }
    }
    if (e->value_mask & CWHeight) {
      c->size.height = e->height;
      if (c->framed) {
        c->size.height += 2 * borderWidth();
      }
    }
    if (e->value_mask & CWBorderWidth) {
      c->border = e->border_width;
    }

    if (c->parent != LScr::I->Root()) {
      wc.x = c->size.x;
      wc.y = c->size.y;
      if (c->framed) {
        wc.y -= textHeight();
      }
      wc.width = c->size.width;
      wc.height = c->size.height;
      if (c->framed) {
        wc.height += textHeight();
      }
      wc.border_width = 1;
      wc.sibling = e->above;
      wc.stack_mode = e->detail;

      LOGD(c) << "XConfigureWindow of " << WinID(e->parent)
              << "; mask=" << XCfgValMask(e->value_mask) << ": " << wc;
      xlib::XConfigureWindow(e->parent, e->value_mask, &wc);
      c->SendConfigureNotify();
    }
  }
  if ((c->internal_state == INormal) && c->framed) {
    // Offsets from outer frame window to inner window.
    wc.x = borderWidth();
    wc.y = titleBarHeight();
  } else {
    wc.x = e->x;
    wc.y = e->y;
  }

  wc.width = e->width;
  wc.height = e->height;
  wc.border_width = 0;
  wc.sibling = e->above;
  wc.stack_mode = e->detail;
  e->value_mask |= CWBorderWidth;

  LOGD(c) << "XConfigureWindow of " << WinID(e->window)
          << "; mask=" << XCfgValMask(e->value_mask) << ": " << wc;
  // xlib::XConfigureWindow(e->window, e->value_mask, &wc);

  if (c->window == e->window) {
    if (c->framed) {
      LOGD(c) << "framed - moving/resizing to " << c->size << "; state is "
              << (c->IsHidden()
                      ? "hidden"
                      : (c->IsWithdrawn()
                             ? "withdrawn"
                             : (c->IsNormal() ? "normal" : "unknown")));
      Rect r_orig =
          Rect::FromXYWH(c->size.x, c->size.y, c->size.width, c->size.height);
      Rect r = makeVisible(r_orig, !c->HasStruts());
      Client_MakeSaneAndMove(c, ENone, r.xMin, r.yMin, r.width(), r.height());
    } else {
      LOGD(c) << "unframed - moving/resizing to " << c->size;
      const Rect& r = makeVisible(c->RectNoBorder(), !c->HasStruts());
      moveResize(c->window, r);
      c->SetSize(r);
    }
  }
}

std::ostream& operator<<(std::ostream& os, const XConfigureEvent& e) {
  os << WinID(e.window) << " " << (e.send_event ? "S" : "s") << e.serial << " ";
  os << Rect::FromXYWH(e.x, e.y, e.width, e.height) << ", b=" << e.border_width;
  return os;
}

void EvConfigureNotify(XEvent* ev) {
  if (current_dragger) {
    // This is probably us moving the window around, so ignore it.
    // TODO: Check if the client is the one being molested, otherwise we'll miss
    // invalid openings if we're dragging.
    return;
  }
  const XConfigureEvent& xc = ev->xconfigure;
  Client* c = LScr::I->GetClient(xc.window);
  LOGD(c) << "ConfigureNotify: " << xc;
  if (!c || !c->framed || c->IsHidden()) {
    return;
  }
  if (c->parent != xc.window) {
    // Only force our own window to be on-screen, not any random
    // sub-window contained within it.
    return;
  }
}

void EvDestroyNotify(XEvent* ev) {
  Window w = ev->xdestroywindow.window;
  Client* c = LScr::I->GetClient(w);
  if (c == 0) {
    return;
  }

  ignore_badwindow = 1;
  Client_Remove(c);
  ignore_badwindow = 0;
}

void EvClientMessage(XEvent* ev) {
  XClientMessageEvent* e = &ev->xclient;
  Client* c = LScr::I->GetClient(e->window);
  if (c == 0) {
    return;
  }
  if (e->message_type == wm_change_state) {
    if (e->format == 32 && e->data.l[0] == IconicState && c->IsNormal()) {
      c->Hide();
    }
    return;
  }
  if (e->message_type == ewmh_atom[_NET_WM_STATE] && e->format == 32) {
    ewmh_change_state(c, e->data.l[0], e->data.l[1]);
    ewmh_change_state(c, e->data.l[0], e->data.l[2]);
    return;
  }
  if (e->message_type == ewmh_atom[_NET_ACTIVE_WINDOW] && e->format == 32) {
    // An EWMH enabled application has asked for this client to be made the
    // active window. Unhide also raises and gives focus to the window.
    c->Unhide();
    return;
  }
  if (e->message_type == ewmh_atom[_NET_CLOSE_WINDOW] && e->format == 32) {
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
    EvConfigureRequest(&ev);
    return;
  }
  if (e->message_type == ewmh_atom[_NET_WM_MOVERESIZE] && e->format == 32) {
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

  if (e->atom == _mozilla_url || e->atom == XA_WM_NAME) {
    LOGD(c) << "Property change: XA_WM_NAME";
    getWindowName(c);
  } else if (e->atom == XA_WM_TRANSIENT_FOR) {
    LOGD(c) << "Property change: XA_WM_TRANSIENT_FOR";
    getTransientFor(c);
  } else if (e->atom == XA_WM_NORMAL_HINTS) {
    LOGD(c) << "Property change: XA_WM_NORMAL_HINTS";
    getNormalHints(c);
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
    XChangeWindowAttributes(dpy, c->parent, CWCursor, &attr);
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
      XChangeWindowAttributes(dpy, c->parent, CWCursor, &attr);
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
