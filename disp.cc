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

#include <X11/X.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xos.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>

#include <stdio.h>
#include <stdlib.h>

#include "ewmh.h"
#include "lwm.h"

/*
 * Dispatcher for main event loop.
 */
struct Disp {
  int type;
  char const *const name;
  void (*handler)(XEvent *);
  void (*debug)(XEvent *, char const *);
};

static void expose(XEvent *);
static void buttonpress(XEvent *);
static void buttonrelease(XEvent *);
static void focuschange(XEvent *);
static void maprequest(XEvent *);
static void configurereq(XEvent *);
static void unmap(XEvent *);
static void destroy(XEvent *);
static void clientmessage(XEvent *);
static void colormap(XEvent *);
static void property(XEvent *);
static void reparent(XEvent *);
static void enter(XEvent *);
static void circulaterequest(XEvent *);
static void motionnotify(XEvent *);

void reshaping_motionnotify(XEvent *);

//
// Code for decoding events and printing them out in an understandable way.
//

// Helper functions for decoding specific types of X integers.
#define CASE_STR(x)                                                            \
  case x:                                                                      \
    return #x
#define WEIRD(x)                                                               \
  default:                                                                     \
    return "Weird" #x

static char const *debugFocusType(int v) {
  switch (v) {
    CASE_STR(FocusIn);
    CASE_STR(FocusOut);
    WEIRD(Focus);
  }
}

static char const *debugPropertyState(int v) {
  switch (v) {
    CASE_STR(PropertyNewValue);
    CASE_STR(PropertyDelete);
    WEIRD(PropertyState);
  }
}

static char const *debugFocusMode(int v) {
  switch (v) {
    CASE_STR(NotifyNormal);
    CASE_STR(NotifyGrab);
    CASE_STR(NotifyUngrab);
    WEIRD(FocusMode);
  }
}

static char const *debugFocusDetail(int v) {
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

static void debugGeneric(XEvent *ev, char const *evName) {
  if (debug_all_events) {
    DBG("%s: window 0x%lx", evName, ev->xany.window);
  }
}

static void debugConfigureNotify(XEvent *ev, char const *evName) {
  if (debug_all_events || debug_configure_notify) {
    XConfigureEvent *xc = &(ev->xconfigure);
    DBG("%s: ev window 0x%lx, window 0x%lx; pos %d, %d; size %d, %d",
        evName, xc->event, xc->window, xc->x, xc->y, xc->width, xc->height);
  }
}

static void debugPropertyNotify(XEvent *ev, char const *evName) {
  if (debug_all_events || debug_property_notify) {
    XPropertyEvent *xp = &(ev->xproperty);
    DBG("%s: window 0x%lx, atom %ld (%s); state %s", evName,
        xp->window, xp->atom, ewmh_atom_name(xp->atom),
        debugPropertyState(xp->state));
  }
}

static void debugFocusChange(XEvent *ev, char const *evName) {
  if (debug_all_events || debug_focus) {
    XFocusChangeEvent *xf = &(ev->xfocus);
    DBG("%s: %s, window 0x%lx, mode=%s, detail=%s", evName,
        debugFocusType(xf->type), xf->window, debugFocusMode(xf->mode),
        debugFocusDetail(xf->detail));
  }
}

static void debugMapRequest(XEvent *ev, char const *evName) {
  if (debug_all_events || debug_map) {
    XMapRequestEvent *e = &ev->xmaprequest;
    DBG("%s: window 0x%lx, parent 0x%lx, send=%d, serial=%lu", evName,
        e->window, e->parent, e->send_event, e->serial);
  }
}

//
// End of all the debugging support.
//

#define REG_DISP(ev, hand, dbg)                                                \
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
    REG_DISP(ConfigureNotify, 0, debugConfigureNotify),
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

/**
 * pending it the client in which an action has been started by a mouse press
 * and we are waiting for the button to be released before performing the action
 */
static Client *pending = NULL;

extern void dispatch(XEvent *ev) {
  for (Disp *p = disps; p < disps + sizeof(disps) / sizeof(disps[0]); p++) {
    if (p->type == ev->type) {
      p->debug(ev, p->name);
      if (p->handler) {
        p->handler(ev);
      }
      return;
    }
  }
  if (!shapeEvent(ev)) {
    DBG("%s: unknown event %d", argv0, ev->type);
  }
}

static void expose(XEvent *ev) {
  /* Only handle the last in a group of Expose events. */
  if (ev->xexpose.count != 0) {
    return;
  }

  Window w = ev->xexpose.window;

  /*
  * We don't draw on the root window so that people can have
  * their favourite Spice Girls backdrop...
  */
  if (getScreenFromRoot(w) != 0) {
    return;
  }

  /* Decide what needs redrawing: window frame or menu? */
  if (current_screen && w == current_screen->popup) {
    if (mode == wm_menu_up) {
      menu_expose();
    } else if (mode == wm_reshaping && current != 0) {
      size_expose();
    }
  } else {
    Client *c = Client_Get(w);
    if (c != 0) {
      Client_DrawBorder(c, c == current);
    }
  }
}

static void buttonpress(XEvent *ev) {
  /* If we're getting it already, we're not in the market for more. */
  if (mode != wm_idle) {
    /* but allow a button press to cancel a move/resize,
     * to satify the EWMH advisory to allow a second mechanism
     * of completing move/resize operations, due to a race.
     * (section 4.3) sucky!
     */
    if (mode == wm_reshaping) {
      mode = wm_idle;
    }
    return;
  }

  XButtonEvent *e = &ev->xbutton;
  Client *c = Client_Get(e->window);

  if (c && c != current && focus_mode == focus_click) {
    /* Click is not on current window,
     * and in click-to-focus mode, so change focus
     */
    Client_Focus(c, e->time);
  }

  /*move this test up to disable scroll to focus*/
  if (e->button >= 4 && e->button <= 7) {
    return;
  }

  if (c && c == current && (e->window == c->parent)) {
    /* Click went to our frame around a client. */

    /* The ``box''. */
    int quarter = (border + titleHeight()) / 4;
    if (e->x > (quarter + 2) && e->x < (3 + 3 * quarter) && e->y > quarter &&
        e->y <= 3 * quarter) {
      /*Client_Close(c);*/
      pending = c;
      mode = wm_closing_window;
      return;
    }

    /* Somewhere in the rest of the frame. */
    if (e->button == HIDE_BUTTON) {
      pending = c;
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

      /* Lasciate ogni speranza voi ch'entrate...  */

      if (e->x <= border && e->y <= border) {
        Client_ReshapeEdge(c, ETopLeft);
      } else if (e->x >= (c->size.width - border) && e->y <= border) {
        Client_ReshapeEdge(c, ETopRight);
      } else if (e->x >= (c->size.width - border) &&
                 e->y >= (c->size.height + titleHeight() - border)) {
        Client_ReshapeEdge(c, EBottomRight);
      } else if (e->x <= border &&
                 e->y >= (c->size.height + titleHeight() - border)) {
        Client_ReshapeEdge(c, EBottomLeft);
      } else if (e->x > border && e->x < (c->size.width - border) &&
                 e->y < border) {
        Client_ReshapeEdge(c, ETop);
      } else if (e->x > border && e->x < (c->size.width - border) &&
                 e->y >= border && e->y < (titleHeight() + border)) {
        Client_Move(c);
      } else if (e->x > (c->size.width - border) && e->y > border &&
                 e->y < (c->size.height + titleHeight() - border)) {
        Client_ReshapeEdge(c, ERight);
      } else if (e->x > border && e->x < (c->size.width - border) &&
                 e->y > (c->size.height - border)) {
        Client_ReshapeEdge(c, EBottom);
      } else if (e->x < border && e->y > border &&
                 e->y < (c->size.height + titleHeight() - border)) {
        Client_ReshapeEdge(c, ELeft);
      }
      return;
    }
    return;
  }

  /* Deal with root window button presses. */
  if (e->window == e->root) {
    if (e->button == Button3) {
      cmapfocus(0);
      menuhit(e);
    } else {
      shell(getScreenFromRoot(e->root), e->button, e->x, e->y);
    }
  }
}

static void buttonrelease(XEvent *ev) {
  XButtonEvent *e = &ev->xbutton;
  if (mode == wm_menu_up) {
    menu_buttonrelease(ev);
  } else if (mode == wm_reshaping) {
    XUnmapWindow(dpy, current_screen->popup);
  } else if (mode == wm_closing_window) {
    /* was the button released within the window's box?*/
    int quarter = (border + titleHeight()) / 4;
    if (pending != NULL && (e->window == pending->parent) &&
        (e->x > (quarter + 2) && e->x < (3 + 3 * quarter) && e->y > quarter &&
         e->y <= 3 * quarter)) {
      Client_Close(pending);
    }
    pending = NULL;
  } else if (mode == wm_hiding_window) {
    /* was the button release within the window's frame? */
    if (pending != NULL && (e->window == pending->parent) && (e->x >= 0) &&
        (e->y >= 0) && (e->x <= pending->size.width) &&
        (e->y <= (pending->size.height + titleHeight()))) {
      if (e->state & ShiftMask) {
        Client_Lower(pending);
      } else {
        hide(pending);
      }
    }
    pending = NULL;
  }
  mode = wm_idle;
}

static void circulaterequest(XEvent *ev) {
  XCirculateRequestEvent *e = &ev->xcirculaterequest;
  Client *c = Client_Get(e->window);
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

static void maprequest(XEvent *ev) {
  XMapRequestEvent *e = &ev->xmaprequest;
  Client *c = Client_Get(e->window);
  DBG_IF(debug_map, "in maprequest, client %p", c);
  
  if (c == 0 || c->window != e->window) {
    for (int screen = 0; screen < screen_count; screen++) {
      scanWindowTree(screen);
    }
    c = Client_Get(e->window);
    DBG_IF(debug_map, "in maprequest, after scan client is %p", c);
    if (c == 0 || c->window != e->window) {
      DBG("MapRequest for non-existent window!");
      return;
    }
  }

  unhidec(c, 1);

  switch (c->state) {
  case WithdrawnState:
    DBG_IF(debug_map, "in maprequest, WithdrawnState");
    if (getScreenFromRoot(c->parent) != 0) {
      DBG_IF(debug_map, "in maprequest, taking over management of window.");
      manage(c, 0);
      break;
    }
    if (c->framed) {
      XReparentWindow(dpy, c->window, c->parent, border,
                      border + titleHeight());
    } else {
      XReparentWindow(dpy, c->window, c->parent, c->size.x, c->size.y);
    }
    XAddToSaveSet(dpy, c->window);
  /*FALLTHROUGH*/
  case NormalState:
    DBG_IF(debug_map, "in maprequest, NormalState");
    XMapWindow(dpy, c->parent);
    XMapWindow(dpy, c->window);
    Client_Raise(c);
    Client_SetState(c, NormalState);
    break;
  }
  ewmh_set_client_list(c->screen);
}

static void unmap(XEvent *ev) {
  XUnmapEvent *e = &ev->xunmap;
  Client *c = Client_Get(e->window);
  if (c == 0) {
    return;
  }

  /*
   * In the description of the ReparentWindow request we read: "If the window
   * is mapped, an UnmapWindow request is performed automatically first". This
   * might seem stupid, but it's the way it is. While a reparenting is pending
   * we ignore UnmapWindow requests.
   */
  if (c->internal_state == IPendingReparenting) {
    c->internal_state = INormal;
    return;
  }

  /* "This time it's the real thing." */

  if (c->state == IconicState) {
    /*
     * Is this a hidden window disappearing? If not, then we
     * aren't interested because it's an unmap request caused
     * by our hiding a window.
     */
    if (e->send_event) {
      unhidec(c, 0); /* It's a hidden window disappearing. */
    }
  } else {
    /* This is a plain unmap, so withdraw the window. */
    withdraw(c);
  }
  c->internal_state = INormal;
}

static void configurereq(XEvent *ev) {
  XWindowChanges wc;
  XConfigureRequestEvent *e = &ev->xconfigurerequest;
  Client *c = Client_Get(e->window);

  if (c && c->window == e->window) {
    /*
    * ICCCM section 4.1.5 says that the x and y coordinates here
    * will have been "adjusted for the border width".
    * NOTE: this may not be the only place to bear this in mind.
    */
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
        c->size.y += titleHeight();
      }
    }
    if (e->value_mask & CWWidth) {
      c->size.width = e->width;
      if (c->framed) {
        c->size.width += 2 * border;
      }
    }
    if (e->value_mask & CWHeight) {
      c->size.height = e->height;
      if (c->framed) {
        c->size.height += 2 * border;
      }
    }
    if (e->value_mask & CWBorderWidth) {
      c->border = e->border_width;
    }

    if (getScreenFromRoot(c->parent) == 0) {
      wc.x = c->size.x;
      wc.y = c->size.y;
      if (c->framed) {
        wc.y -= titleHeight();
      }
      wc.width = c->size.width;
      wc.height = c->size.height;
      if (c->framed) {
        wc.height += titleHeight();
      }
      wc.border_width = 1;
      wc.sibling = e->above;
      wc.stack_mode = e->detail;

      XConfigureWindow(dpy, e->parent, e->value_mask, &wc);
      sendConfigureNotify(c);
    }
  }
  if (c && (c->internal_state == INormal) && c->framed) {
    wc.x = border;
    wc.y = border;
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
      XMoveResizeWindow(dpy, c->parent, c->size.x, c->size.y - titleHeight(),
                        c->size.width, c->size.height + titleHeight());
      XMoveWindow(dpy, c->window, border, border + titleHeight());
    } else {
      XMoveResizeWindow(dpy, c->window, c->size.x, c->size.y, c->size.width,
                        c->size.height);
    }
  }
}

static void destroy(XEvent *ev) {
  Window w = ev->xdestroywindow.window;
  Client *c = Client_Get(w);
  if (c == 0) {
    return;
  }

  ignore_badwindow = 1;
  Client_Remove(c);
  ignore_badwindow = 0;
}

static void clientmessage(XEvent *ev) {
  XClientMessageEvent *e = &ev->xclient;
  Client *c = Client_Get(e->window);
  if (c == 0) {
    return;
  }
  if (e->message_type == wm_change_state) {
    if (e->format == 32 && e->data.l[0] == IconicState && normal(c)) {
      hide(c);
    }
    return;
  }
  if (e->message_type == ewmh_atom[_NET_WM_STATE] && e->format == 32) {
    ewmh_change_state(c, e->data.l[0], e->data.l[1]);
    ewmh_change_state(c, e->data.l[0], e->data.l[2]);
    return;
  }
  if (e->message_type == ewmh_atom[_NET_ACTIVE_WINDOW] && e->format == 32) {
    /* An EWMH enabled application has asked for this client
     * to be made the active window. The window is raised, and
     * focus given if the focus mode is click (focusing on a
     * window other than the one the pointer is in makes no
     * sense when the focus mode is enter).
     */
    if (hidden(c)) {
      unhidec(c, 1);
    }
    XMapWindow(dpy, c->parent);
    Client_Raise(c);
    if (c != current && focus_mode == focus_click) {
      Client_Focus(c, CurrentTime);
    }
    return;
  }
  if (e->message_type == ewmh_atom[_NET_CLOSE_WINDOW] && e->format == 32) {
    Client_Close(c);
    return;
  }
  if (e->message_type == ewmh_atom[_NET_MOVERESIZE_WINDOW] && e->format == 32) {
    XEvent ev;

    /* FIXME: ok, so this is a bit of a hack */
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
    EWMHDirection direction = (EWMHDirection) e->data.l[2];

    /* before we can do any resizing, make the window visible */
    if (hidden(c)) {
      unhidec(c, 1);
    }
    XMapWindow(dpy, c->parent);
    Client_Raise(c);
    /* FIXME: we're ignoring x_root, y_root and button! */
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
      /* FIXME: don't know how to deal with this */
      edge = E_LAST;
      break;
    case DMoveKeyboard:
      edge = E_LAST;
      break;
    default:
      edge = E_LAST;
      fprintf(stderr, "%s: received _NET_WM_MOVERESIZE"
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

static void colormap(XEvent *ev) {
  XColormapEvent *e = &ev->xcolormap;
  if (e->c_new) {
    Client *c = Client_Get(e->window);
    if (c) {
      c->cmap = e->colormap;
      if (c == current) {
        cmapfocus(c);
      }
    } else {
      Client_ColourMap(ev);
    }
  }
}

static void property(XEvent *ev) {
  XPropertyEvent *e = &ev->xproperty;
  Client *c = Client_Get(e->window);
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
    if (c == current) {
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

static void reparent(XEvent *ev) {
  XReparentEvent *e = &ev->xreparent;
  if (getScreenFromRoot(e->event) == 0 || e->override_redirect ||
      getScreenFromRoot(e->parent) != 0) {
    return;
  }

  Client *c = Client_Get(e->window);
  if (c != 0 && (getScreenFromRoot(c->parent) != 0 || withdrawn(c))) {
    Client_Remove(c);
  }
}

static void focuschange(XEvent *ev) {
  if (ev->type == FocusOut) {
    return;
  }
  Window focus_window;
  int revert_to;
  XGetInputFocus(dpy, &focus_window, &revert_to);
  if (focus_window == PointerRoot || focus_window == None) {
    if (current) {
      Client_Focus(NULL, CurrentTime);
    }
    return;
  }
  Client *c = Client_Get(focus_window);
  if (c && c != current) {
    Client_Focus(c, CurrentTime);
  }
}

static void enter(XEvent *ev) {
  Client *c = Client_Get(ev->xcrossing.window);
  if (c == 0 || mode != wm_idle) {
    return;
  }

  if (c->framed) {
    XSetWindowAttributes attr;

    attr.cursor = c->screen->root_cursor;
    XChangeWindowAttributes(dpy, c->parent, CWCursor, &attr);
    c->cursor = ENone;
  }
  if (c != current && !c->hidden && focus_mode == focus_enter) {
    /* Entering a new window in enter focus mode, so take focus */
    Client_Focus(c, ev->xcrossing.time);
  }
}

static void motionnotify(XEvent *ev) {
  if (mode == wm_reshaping) {
    reshaping_motionnotify(ev);
  } else if (mode == wm_menu_up) {
    menu_motionnotify(ev);
  } else if (mode == wm_idle) {
    XMotionEvent *e = &ev->xmotion;
    Client *c = Client_Get(e->window);
    Edge edge = ENone;

    if (c && (e->window == c->parent) && (e->subwindow != c->window) &&
        mode == wm_idle) {
      /* mouse moved in a frame we manage - check cursor */
      int quarter = (border + titleHeight()) / 4;
      if (e->x > (quarter + 2) && e->x < (3 + 3 * quarter) && e->y > quarter &&
          e->y <= 3 * quarter) {
        edge = E_LAST;
      } else if (e->x <= border && e->y <= border) {
        edge = ETopLeft;
      } else if (e->x >= (c->size.width - border) && e->y <= border) {
        edge = ETopRight;
      } else if (e->x >= (c->size.width - border) &&
                 e->y >= (c->size.height + titleHeight() - border)) {
        edge = EBottomRight;
      } else if (e->x <= border &&
                 e->y >= (c->size.height + titleHeight() - border)) {
        edge = EBottomLeft;
      } else if (e->x > border && e->x < (c->size.width - border) &&
                 e->y < border) {
        edge = ETop;
      } else if (e->x > border && e->x < (c->size.width - border) &&
                 e->y >= border && e->y < (titleHeight() + border)) {
        edge = ENone;
      } else if (e->x > (c->size.width - border) && e->y > border &&
                 e->y < (c->size.height + titleHeight() - border)) {
        edge = ERight;
      } else if (e->x > border && e->x < (c->size.width - border) &&
                 e->y > (c->size.height - border)) {
        edge = EBottom;
      } else if (e->x < border && e->y > border &&
                 e->y < (c->size.height + titleHeight() - border)) {
        edge = ELeft;
      }
      if (c->cursor != edge) {
        XSetWindowAttributes attr;

        if (edge == ENone) {
          attr.cursor = c->screen->root_cursor;
        } else if (edge == E_LAST) {
          attr.cursor = c->screen->box_cursor;
        } else {
          attr.cursor = c->screen->cursor_map[edge];
        }
        XChangeWindowAttributes(dpy, c->parent, CWCursor, &attr);
        c->cursor = edge;
      }
    }
  }
}

/*ARGSUSED*/
void reshaping_motionnotify(XEvent *ev) {
  int nx;  /* New x. */
  int ny;  /* New y. */
  int ox;  /* Original x. */
  int oy;  /* Original y. */
  int ndx; /* New width. */
  int ndy; /* New height. */
  int odx; /* Original width. */
  int ody; /* Original height. */

  if (mode != wm_reshaping || !current) {
    return;
  }

  MousePos mp = getMousePosition();
  // We can sometimes get into a funny situation whereby we randomly start
  // dragging a window about. To avoid this, ensure that if we see the
  // mouse buttons aren't being held, we drop out of reshaping mode
  // immediately.
  if ((mp.modMask & MOVING_BUTTON_MASK) == 0) {
    mode = wm_idle;
    DBG("Flipped out of weird dragging mode.");
    return;
  }

  if (interacting_edge != ENone) {
    nx = ox = current->size.x;
    ny = oy = current->size.y;
    ndx = odx = current->size.width;
    ndy = ody = current->size.height;

    Client_SizeFeedback();

    /* Vertical. */
    if (isTopEdge(interacting_edge)) {
      mp.y += titleHeight();
      ndy += (current->size.y - mp.y);
      ny = mp.y;
    }
    if (isBottomEdge(interacting_edge)) {
      ndy = mp.y - current->size.y;
    }

    /* Horizontal. */
    if (isRightEdge(interacting_edge)) {
      ndx = mp.x - current->size.x;
    }
    if (isLeftEdge(interacting_edge)) {
      ndx += (current->size.x - mp.x);
      nx = mp.x;
    }

    Client_MakeSane(current, interacting_edge, &nx, &ny, &ndx, &ndy);
    XMoveResizeWindow(dpy, current->parent, current->size.x,
                      current->size.y - titleHeight(), current->size.width,
                      current->size.height + titleHeight());
    if (current->size.width == odx && current->size.height == ody) {
      if (current->size.x != ox || current->size.y != oy) {
        sendConfigureNotify(current);
      }
    } else {
      XMoveResizeWindow(dpy, current->window, border, border + titleHeight(),
                        current->size.width - 2 * border,
                        current->size.height - 2 * border);
    }
  } else {
    nx = mp.x + start_x;
    ny = mp.y + start_y;

    Client_MakeSane(current, interacting_edge, &nx, &ny, 0, 0);
    if (current->framed) {
      XMoveWindow(dpy, current->parent, current->size.x,
                  current->size.y - titleHeight());
    } else {
      XMoveWindow(dpy, current->parent, current->size.x, current->size.y);
    }
    sendConfigureNotify(current);
  }
}
