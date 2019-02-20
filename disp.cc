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

// Dispatcher for main event loop.
struct Disp {
  int type;
  char const* const name;
  void (*handler)(XEvent*);
  void (*debug)(XEvent*, char const*);
};

static void expose(XEvent*);
static void buttonpress(XEvent*);
static void buttonrelease(XEvent*);
static void focuschange(XEvent*);
static void maprequest(XEvent*);
static void configurereq(XEvent*);
static void configurenotify(XEvent*);
static void unmap(XEvent*);
static void destroy(XEvent*);
static void clientmessage(XEvent*);
static void colormap(XEvent*);
static void property(XEvent*);
static void reparent(XEvent*);
static void enter(XEvent*);
static void circulaterequest(XEvent*);
static void motionnotify(XEvent*);

void reshaping_motionnotify(XEvent*);

//
// Code for decoding events and printing them out in an understandable way.
//

// Helper functions for decoding specific types of X integers.
#define CASE_STR(x) \
  case x:           \
    return #x
#define WEIRD(x) \
  default:       \
    return "Weird" #x

static char const* debugFocusType(int v) {
  switch (v) {
    CASE_STR(FocusIn);
    CASE_STR(FocusOut);
    WEIRD(Focus);
  }
}

static char const* debugPropertyState(int v) {
  switch (v) {
    CASE_STR(PropertyNewValue);
    CASE_STR(PropertyDelete);
    WEIRD(PropertyState);
  }
}

static char const* debugFocusMode(int v) {
  switch (v) {
    CASE_STR(NotifyNormal);
    CASE_STR(NotifyGrab);
    CASE_STR(NotifyUngrab);
    WEIRD(FocusMode);
  }
}

static char const* debugFocusDetail(int v) {
  switch (v) {
    CASE_STR(NotifyAncestor);
    CASE_STR(NotifyVirtual);
    CASE_STR(NotifyInferior);
    CASE_STR(NotifyNonlinear);
    CASE_STR(NotifyNonlinearVirtual);
    CASE_STR(NotifyPointer);
    CASE_STR(NotifyPointerRoot);
    CASE_STR(NotifyDetailNone);
    WEIRD(FocusDetail);
  }
}

// Undefine our helper macros, so they don't pollute anyone else.
#undef CASE_STR
#undef WEIRD

static void debugGeneric(XEvent* ev, char const* evName) {
  if (debug_all_events) {
    DBGF("%s: window 0x%lx", evName, ev->xany.window);
  }
}

static void debugConfigureNotify(XEvent* ev, char const* evName) {
  if (debug_all_events || debug_configure_notify) {
    XConfigureEvent* xc = &(ev->xconfigure);
    DBGF("%s: ev window 0x%lx, window 0x%lx; pos %d, %d; size %d, %d", evName,
         xc->event, xc->window, xc->x, xc->y, xc->width, xc->height);
  }
}

static void debugPropertyNotify(XEvent* ev, char const* evName) {
  if (debug_all_events || debug_property_notify) {
    XPropertyEvent* xp = &(ev->xproperty);
    DBGF("%s: window 0x%lx, atom %ld (%s); state %s", evName, xp->window,
         xp->atom, ewmh_atom_name(xp->atom), debugPropertyState(xp->state));
  }
}

static void debugFocusChange(XEvent* ev, char const* evName) {
  if (debug_all_events || debug_focus) {
    XFocusChangeEvent* xf = &(ev->xfocus);
    DBGF("%s: %s, window 0x%lx, mode=%s, detail=%s", evName,
         debugFocusType(xf->type), xf->window, debugFocusMode(xf->mode),
         debugFocusDetail(xf->detail));
  }
}

static void debugMapRequest(XEvent* ev, char const* evName) {
  if (debug_all_events || debug_map) {
    XMapRequestEvent* e = &ev->xmaprequest;
    DBGF("%s: window 0x%lx, parent 0x%lx, send=%d, serial=%lu", evName,
         e->window, e->parent, e->send_event, e->serial);
  }
}

//
// End of all the debugging support.
//

#define REG_DISP(ev, hand, dbg) \
  { ev, #ev, hand, dbg }
static Disp disps[] = {
    REG_DISP(Expose, expose, debugGeneric),
    REG_DISP(MotionNotify, motionnotify, debugGeneric),
    REG_DISP(ButtonPress, buttonpress, debugGeneric),
    REG_DISP(ButtonRelease, buttonrelease, debugGeneric),
    REG_DISP(FocusIn, focuschange, debugFocusChange),
    REG_DISP(FocusOut, focuschange, debugFocusChange),
    REG_DISP(MapRequest, maprequest, debugMapRequest),
    REG_DISP(ConfigureRequest, configurereq, debugGeneric),
    REG_DISP(UnmapNotify, unmap, debugGeneric),
    REG_DISP(DestroyNotify, destroy, debugGeneric),
    REG_DISP(ClientMessage, clientmessage, debugGeneric),
    REG_DISP(ColormapNotify, colormap, debugGeneric),
    REG_DISP(PropertyNotify, property, debugPropertyNotify),
    REG_DISP(ReparentNotify, reparent, debugGeneric),
    REG_DISP(EnterNotify, enter, debugGeneric),
    REG_DISP(CirculateRequest, circulaterequest, debugGeneric),
    REG_DISP(LeaveNotify, 0, debugGeneric),
    REG_DISP(ConfigureNotify, configurenotify, debugConfigureNotify),
    REG_DISP(CreateNotify, 0, debugGeneric),
    REG_DISP(GravityNotify, 0, debugGeneric),
    REG_DISP(MapNotify, 0, debugGeneric),
    REG_DISP(MappingNotify, 0, debugGeneric),
    REG_DISP(SelectionClear, 0, debugGeneric),
    REG_DISP(SelectionNotify, 0, debugGeneric),
    REG_DISP(SelectionRequest, 0, debugGeneric),
    REG_DISP(NoExpose, 0, debugGeneric),
};
#undef REG_DISP

// pendingFrame is the LWM frame window (aka parent) of the client in which an
// action has been started by a mouse press and we are waiting for the button
// to be released before performing the action.
// It may refer to a disappeared window if something closes.
static Window pendingFrame;

extern void dispatch(XEvent* ev) {
  for (Disp* p = disps; p < disps + sizeof(disps) / sizeof(disps[0]); p++) {
    if (p->type == ev->type) {
      p->debug(ev, p->name);
      if (p->handler) {
        p->handler(ev);
      }
      return;
    }
  }
  if (!shapeEvent(ev)) {
    DBGF("%s: unknown event %d", argv0, ev->type);
  }
}

static void expose(XEvent* ev) {
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
    if (mode == wm_menu_up) {
      LScr::I->GetHider()->Paint();
    } else if (mode == wm_reshaping) {
      size_expose();
    }
  } else {
    Client* c = LScr::I->GetClient(w);
    if (c != 0) {
      c->DrawBorder();
    }
  }
}

static void buttonpress(XEvent* ev) {
  if (mode != wm_idle) {
    return;  // We're already doing something, so ignore extra presses.
  }
  XButtonEvent* e = &ev->xbutton;

  // Deal with root window button presses.
  if (e->window == e->root) {
    if (e->button == Button3) {
      cmapfocus(0);
      LScr::I->GetHider()->OpenMenu(e);
    } else {
      shell(e->button);
    }
    return;
  }

  Client* c = LScr::I->GetClient(e->window);
  if (c == nullptr) {
    return;
  }
  if (Resources::I->ClickToFocus()) {
    LScr::I->GetFocuser()->FocusClient(c);
  }

  // move this test up to disable scroll to focus
  if (e->button >= 4 && e->button <= 7) {
    return;
  }
  const Edge edge = c->EdgeAt(e->window, e->x, e->y);
  if (edge == EContents) {
    return;
  }
  if (edge == EClose) {
    pendingFrame = c->parent;
    mode = wm_closing_window;
    return;
  }

  // Somewhere in the rest of the frame.
  if (e->button == HIDE_BUTTON) {
    pendingFrame = c->parent;
    mode = wm_hiding_window;
    return;
  }
  if (e->button == MOVE_BUTTON) {
    Client_Move(c);
    return;
  }
  if (e->button == RESHAPE_BUTTON) {
    XMapWindow(dpy, c->parent);
    Client_Raise(c);
    Client_ReshapeEdge(c, edge);
  }
}

static void buttonrelease(XEvent* ev) {
  XButtonEvent* e = &ev->xbutton;
  Client* pendingClient = LScr::I->GetClient(pendingFrame);
  if (mode == wm_menu_up) {
    LScr::I->GetHider()->MouseRelease(ev);
  } else if (mode == wm_reshaping) {
    XUnmapWindow(dpy, LScr::I->Popup());
  } else if (mode == wm_closing_window && pendingClient) {
    if (pendingClient->EdgeAt(e->window, e->x, e->y) == EClose) {
      Client_Close(pendingClient);
    }
  } else if (mode == wm_hiding_window && pendingClient) {
    // Was the button release within the window's frame?
    // Note that x11 sends is buttonrelease events which match the window the
    // mousedown event went to, even if we let go of the mouse while hovering
    // over the background.
    if ((e->window == pendingClient->parent) && (e->x >= 0) && (e->y >= 0) &&
        (e->x <= pendingClient->size.width) &&
        (e->y <= (pendingClient->size.height + textHeight()))) {
      if (e->state & ShiftMask) {
        Client_Lower(pendingClient);
      } else {
        pendingClient->Hide();
      }
    }
  }
  pendingFrame = 0;
  mode = wm_idle;
}

static void circulaterequest(XEvent* ev) {
  XCirculateRequestEvent* e = &ev->xcirculaterequest;
  Client* c = LScr::I->GetClient(e->window);
  if (c == 0) {
    if (e->place == PlaceOnTop) {
      XRaiseWindow(e->display, e->window);
    } else {
      XLowerWindow(e->display, e->window);
    }
  } else {
    if (e->place == PlaceOnTop) {
      Client_Raise(c);
    } else {
      Client_Lower(c);
    }
  }
}

static void maprequest(XEvent* ev) {
  XMapRequestEvent* e = &ev->xmaprequest;
  Client* c = LScr::I->GetOrAddClient(e->window);
  DBGF_IF(debug_map, "in maprequest, client %p", (void*)c);
  if (!c) {
    DBGF("MapRequest for non-existent window: %lx!", c->window);
    return;
  }

  if (c->hidden) {
    c->Unhide();
  }

  switch (c->State()) {
    case WithdrawnState:
      if (c->parent == LScr::I->Root()) {
        DBGF_IF(debug_map,
                "in maprequest, taking over management of window %lx.",
                c->window);
        manage(c);
        LScr::I->GetFocuser()->FocusClient(c);
        break;
      }
      if (c->framed) {
        DBGF_IF(debug_map, "in maprequest, reparenting window %lx.", c->parent);
        XReparentWindow(dpy, c->window, c->parent, borderWidth(),
                        borderWidth() + textHeight());
      } else {
        DBGF_IF(debug_map, "in maprequest, reparenting (2) window %lx.",
                c->parent);
        XReparentWindow(dpy, c->window, c->parent, c->size.x, c->size.y);
      }
      XAddToSaveSet(dpy, c->window);
      // FALLTHROUGH
    case NormalState:
      DBG_IF(debug_map, "in maprequest, NormalState");
      XMapWindow(dpy, c->parent);
      XMapWindow(dpy, c->window);
      Client_Raise(c);
      c->SetState(NormalState);
      break;
  }
  ewmh_set_client_list();
}

static void unmap(XEvent* ev) {
  Client* c = LScr::I->GetClient(ev->xunmap.window);
  if (c == nullptr) {
    return;
  }

  // In the description of the ReparentWindow request we read: "If the window
  // is mapped, an UnmapWindow request is performed automatically first". This
  // might seem stupid, but it's the way it is. While a reparenting is pending
  // we ignore UnmapWindow requests.
  if (c->internal_state != IPendingReparenting) {
    withdraw(c);
  }
  c->internal_state = INormal;
}

static void configurereq(XEvent* ev) {
  XWindowChanges wc;
  XConfigureRequestEvent* e = &ev->xconfigurerequest;
  Client* c = LScr::I->GetClient(e->window);

  if (c && c->window == e->window) {
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

      XConfigureWindow(dpy, e->parent, e->value_mask, &wc);
      sendConfigureNotify(c);
    }
  }
  if (c && (c->internal_state == INormal) && c->framed) {
    wc.x = borderWidth();
    wc.y = borderWidth();
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

  XConfigureWindow(dpy, e->window, e->value_mask, &wc);

  if (c) {
    if (c->framed) {
      XMoveResizeWindow(dpy, c->parent, c->size.x, c->size.y - textHeight(),
                        c->size.width, c->size.height + textHeight());
      XMoveWindow(dpy, c->window, borderWidth(), borderWidth() + textHeight());
    } else {
      XMoveResizeWindow(dpy, c->window, c->size.x, c->size.y, c->size.width,
                        c->size.height);
    }
  }
}

static void configurenotify(XEvent* ev) {
  if (mode != wm_idle) {
    // This is probably us moving the window around, so ignore it.
    // TODO: Check if the client is the one being molested, otherwise we'll miss
    // invalid openings if we're dragging.
    return;
  }
  const XConfigureEvent& xc = ev->xconfigure;
  Client* c = LScr::I->GetClient(xc.window);
  if (!c || !c->framed || c->IsHidden()) {
    return;
  }
  if (c->parent != xc.window) {
    // Only force our own window to be on-screen, not any random
    // sub-window contained within it.
    return;
  }
  const int bw = borderWidth();
  const int th = textHeight();
  const int x = xc.x + bw;
  const int y = xc.y + th;
  const int w = xc.width - 2 * bw;
  const int h = xc.height - (bw + th);
  if (Client_MakeSane(c, ENone, x, y, w, h)) {
    XMoveResizeWindow(dpy, c->parent, c->size.x, c->size.y - textHeight(),
                      c->size.width, c->size.height + textHeight());
    LOGW() << "Forcing sanity upon " << c->Name() << ", at " << c->size.x
           << ", " << c->size.y;
  }
}

static void destroy(XEvent* ev) {
  Window w = ev->xdestroywindow.window;
  Client* c = LScr::I->GetClient(w);
  if (c == 0) {
    return;
  }

  ignore_badwindow = 1;
  Client_Remove(c);
  ignore_badwindow = 0;
}

static void clientmessage(XEvent* ev) {
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
    configurereq(&ev);
    return;
  }
  if (e->message_type == ewmh_atom[_NET_WM_MOVERESIZE] && e->format == 32) {
    Edge edge = E_LAST;
    EWMHDirection direction = (EWMHDirection)e->data.l[2];

    // before we can do any resizing, make the window visible
    if (c->IsHidden()) {
      c->Unhide();
    }
    XMapWindow(dpy, c->parent);
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
        Client_Move(c);
        break;
      default:
        Client_ReshapeEdge(c, edge);
        break;
    }
  }
}

static void colormap(XEvent* ev) {
  XColormapEvent* e = &ev->xcolormap;
  if (e->c_new) {
    Client* c = LScr::I->GetClient(e->window);
    if (c) {
      c->cmap = e->colormap;
      if (c->HasFocus()) {
        cmapfocus(c);
      }
    } else {
      Client_ColourMap(ev);
    }
  }
}

static void property(XEvent* ev) {
  XPropertyEvent* e = &ev->xproperty;
  Client* c = LScr::I->GetClient(e->window);
  if (c == 0) {
    return;
  }

  if (e->atom == _mozilla_url || e->atom == XA_WM_NAME) {
    getWindowName(c);
  } else if (e->atom == XA_WM_TRANSIENT_FOR) {
    getTransientFor(c);
  } else if (e->atom == XA_WM_NORMAL_HINTS) {
    getNormalHints(c);
  } else if (e->atom == wm_colormaps) {
    getColourmaps(c);
    if (c->HasFocus()) {
      cmapfocus(c);
    }
  } else if (e->atom == ewmh_atom[_NET_WM_STRUT]) {
    ewmh_get_strut(c);
  } else if (e->atom == ewmh_atom[_NET_WM_STATE]) {
    // Received notice that client wants to change its state
    //  update internal wstate tracking
    bool wasFullscreen = c->wstate.fullscreen;
    ewmh_get_state(c);
    // make any changes requested
    if (c->wstate.fullscreen && !wasFullscreen) {
      Client_EnterFullScreen(c);
    } else if (!c->wstate.fullscreen && wasFullscreen) {
      Client_ExitFullScreen(c);
    }
  }
}

static void reparent(XEvent* ev) {
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

static void focuschange(XEvent* ev) {
  if (ev->type == FocusOut) {
    return;
  }
  Window focus_window;
  int revert_to;
  XGetInputFocus(dpy, &focus_window, &revert_to);
  Client* c = LScr::I->GetClient(focus_window);
  if (c) {
    LScr::I->GetFocuser()->FocusClient(c);
  }
}

static void enter(XEvent* ev) {
  if (mode == wm_idle) {
    LScr::I->GetFocuser()->EnterWindow(ev->xcrossing.window,
                                       ev->xcrossing.time);
    // We receive enter events for our client windows too. When we do, we need
    // to switch the mouse pointer's shape to the default pointer.
    // If we don't do this, then for apps like Rhythmbox which don't
    // aggressively set the pointer to their preferred shape, we end up showing
    // silly icons, such as the 'resize corner' icon, while hovering over the
    // middle of the application window.
    Client* c = LScr::I->GetClient(ev->xcrossing.window);
    if (c && ev->xcrossing.window != c->parent) {
      // TODO: add a SetCursor method to Client, so we don't have to keep
      // repeating this code everywhere.
      XSetWindowAttributes attr;
      attr.cursor = LScr::I->Cursors()->Root();
      XChangeWindowAttributes(dpy, c->parent, CWCursor, &attr);
      c->cursor = ENone;
    }
  }
}

static void motionnotify(XEvent* ev) {
  if (mode == wm_reshaping) {
    reshaping_motionnotify(ev);
  } else if (mode == wm_menu_up) {
    LScr::I->GetHider()->MouseMotion(ev);
  } else if (mode == wm_idle) {
    XMotionEvent* e = &ev->xmotion;
    Client* c = LScr::I->GetClient(e->window);
    if (c && (e->window == c->parent) && (e->subwindow != c->window)) {
      Edge edge = c->EdgeAt(e->window, e->x, e->y);
      if (edge != EContents && c->cursor != edge) {
        XSetWindowAttributes attr;
        attr.cursor = LScr::I->Cursors()->ForEdge(edge);
        XChangeWindowAttributes(dpy, c->parent, CWCursor, &attr);
        c->cursor = edge;
      }
    }
  }
}

/*ARGSUSED*/
void reshaping_motionnotify(XEvent* ev) {
  ev = ev;
  int nx;   // New x.
  int ny;   // New y.
  int ox;   // Original x.
  int oy;   // Original y.
  int ndx;  // New width.
  int ndy;  // New height.
  int odx;  // Original width.
  int ody;  // Original height.

  Client* c = Client::FocusedClient();
  if (mode != wm_reshaping || !c) {
    return;
  }

  MousePos mp = getMousePosition();
  // We can sometimes get into a funny situation whereby we randomly start
  // dragging a window about. To avoid this, ensure that if we see the
  // mouse buttons aren't being held, we drop out of reshaping mode
  // immediately.
  if ((mp.modMask & MOVING_BUTTON_MASK) == 0) {
    mode = wm_idle;
    // If we escape from the weird dragging mode and we were resizing, we should
    // ensure the size popup is closed.
    XUnmapWindow(dpy, LScr::I->Popup());
    DBG("Flipped out of weird dragging mode.");
    return;
  }

  if (interacting_edge != ENone) {
    nx = ox = c->size.x;
    ny = oy = c->size.y;
    ndx = odx = c->size.width;
    ndy = ody = c->size.height;

    Client_SizeFeedback();

    // Vertical.
    if (isTopEdge(interacting_edge)) {
      mp.y += textHeight();
      ndy += (c->size.y - mp.y);
      ny = mp.y;
    }
    if (isBottomEdge(interacting_edge)) {
      ndy = mp.y - c->size.y;
    }

    // Horizontal.
    if (isRightEdge(interacting_edge)) {
      ndx = mp.x - c->size.x;
    }
    if (isLeftEdge(interacting_edge)) {
      ndx += (c->size.x - mp.x);
      nx = mp.x;
    }

    Client_MakeSane(c, interacting_edge, nx, ny, ndx, ndy);
    XMoveResizeWindow(dpy, c->parent, c->size.x, c->size.y - textHeight(),
                      c->size.width, c->size.height + textHeight());
    if (c->size.width == odx && c->size.height == ody) {
      if (c->size.x != ox || c->size.y != oy) {
        sendConfigureNotify(c);
      }
    } else {
      const int border = borderWidth();
      XMoveResizeWindow(dpy, c->window, border, border + textHeight(),
                        c->size.width - 2 * border,
                        c->size.height - 2 * border);
    }
  } else {
    nx = mp.x + start_x;
    ny = mp.y + start_y;

    Client_MakeSane(c, interacting_edge, nx, ny, 0, 0);
    if (c->framed) {
      XMoveWindow(dpy, c->parent, c->size.x, c->size.y - textHeight());
    } else {
      XMoveWindow(dpy, c->parent, c->size.x, c->size.y);
    }
    sendConfigureNotify(c);
  }
}
