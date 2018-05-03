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
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ewmh.h"
#include "lwm.h"

// The following two arrays are co-indexed. The ewmh_atom_names is used to look
// up the name of a given Atom value, for debugging.
Atom ewmh_atom[EWMH_ATOM_LAST];
char const* ewmh_atom_names[EWMH_ATOM_LAST];
Atom utf8_string;

void ewmh_init(void) {
  // Build half a million EWMH atoms.
#define SET_ATOM(x)                                          \
    do {                                                     \
      ewmh_atom[x] = XInternAtom(dpy, #x, False);            \
      ewmh_atom_names[x] = #x;                               \
    } while (0)
  SET_ATOM(_NET_SUPPORTED);
  SET_ATOM(_NET_CLIENT_LIST);
  SET_ATOM(_NET_CLIENT_LIST_STACKING);
  SET_ATOM(_NET_NUMBER_OF_DESKTOPS);
  SET_ATOM(_NET_DESKTOP_GEOMETRY);
  SET_ATOM(_NET_DESKTOP_VIEWPORT);
  SET_ATOM(_NET_CURRENT_DESKTOP);
  SET_ATOM(_NET_DESKTOP_NAMES);
  SET_ATOM(_NET_ACTIVE_WINDOW);
  SET_ATOM(_NET_WORKAREA);
  SET_ATOM(_NET_SUPPORTING_WM_CHECK);
  SET_ATOM(_NET_VIRTUAL_ROOTS);
  SET_ATOM(_NET_DESKTOP_LAYOUT);
  SET_ATOM(_NET_SHOWING_DESKTOP);
  SET_ATOM(_NET_CLOSE_WINDOW);
  SET_ATOM(_NET_MOVERESIZE_WINDOW);
  SET_ATOM(_NET_WM_MOVERESIZE);
  SET_ATOM(_NET_WM_NAME);
  SET_ATOM(_NET_WM_VISIBLE_NAME);
  SET_ATOM(_NET_WM_ICON_NAME);
  SET_ATOM(_NET_WM_VISIBLE_ICON_NAME);
  SET_ATOM(_NET_WM_DESKTOP);
  SET_ATOM(_NET_WM_WINDOW_TYPE);
  SET_ATOM(_NET_WM_STATE);
  SET_ATOM(_NET_WM_ALLOWED_ACTIONS);
  SET_ATOM(_NET_WM_STRUT);
  SET_ATOM(_NET_WM_ICON_GEOMETRY);
  SET_ATOM(_NET_WM_ICON);
  SET_ATOM(_NET_WM_PID);
  SET_ATOM(_NET_WM_HANDLED_ICONS);
  SET_ATOM(_NET_WM_WINDOW_TYPE_DESKTOP);
  SET_ATOM(_NET_WM_WINDOW_TYPE_DOCK);
  SET_ATOM(_NET_WM_WINDOW_TYPE_TOOLBAR);
  SET_ATOM(_NET_WM_WINDOW_TYPE_MENU);
  SET_ATOM(_NET_WM_WINDOW_TYPE_UTILITY);
  SET_ATOM(_NET_WM_WINDOW_TYPE_SPLASH);
  SET_ATOM(_NET_WM_WINDOW_TYPE_DIALOG);
  SET_ATOM(_NET_WM_WINDOW_TYPE_NORMAL);
  SET_ATOM(_NET_WM_STATE_MODAL);
  SET_ATOM(_NET_WM_STATE_STICKY);
  SET_ATOM(_NET_WM_STATE_MAXIMISED_VERT);
  SET_ATOM(_NET_WM_STATE_MAXIMISED_HORZ);
  SET_ATOM(_NET_WM_STATE_SHADED);
  SET_ATOM(_NET_WM_STATE_SKIP_TASKBAR);
  SET_ATOM(_NET_WM_STATE_SKIP_PAGER);
  SET_ATOM(_NET_WM_STATE_HIDDEN);
  SET_ATOM(_NET_WM_STATE_FULLSCREEN);
  SET_ATOM(_NET_WM_STATE_ABOVE);
  SET_ATOM(_NET_WM_STATE_BELOW);
  SET_ATOM(_NET_WM_ACTION_MOVE);
  SET_ATOM(_NET_WM_ACTION_RESIZE);
  SET_ATOM(_NET_WM_ACTION_MINIMIZE);
  SET_ATOM(_NET_WM_ACTION_SHADE);
  SET_ATOM(_NET_WM_ACTION_STICK);
  SET_ATOM(_NET_WM_ACTION_MAXIMIZE_HORIZ);
  SET_ATOM(_NET_WM_ACTION_MAXIMIZE_VERT);
  SET_ATOM(_NET_WM_ACTION_FULLSCREEN);
  SET_ATOM(_NET_WM_ACTION_CHANGE_DESKTOP);
  SET_ATOM(_NET_WM_ACTION_CLOSE);
#undef SET_ATOM
  utf8_string = XInternAtom(dpy, "UTF8_STRING", False);
}

char const *ewmh_atom_name(Atom at) {
  for (int i = 0; i < EWMH_ATOM_LAST; i++) {
    if (at == ewmh_atom[i]) {
      return ewmh_atom_names[i];
    }
  }
  return "unknown atom";
}

void ewmh_init_screens(void) {
  // Announce EWMH compatibility on all screens.
  for (int i = 0; i < screen_count; i++) {
    screens[i].ewmh_set_client_list = False;
    screens[i].ewmh_compat =
        XCreateSimpleWindow(dpy, screens[i].root, -200, -200, 1, 1, 0, 0, 0);
    XChangeProperty(dpy, screens[i].ewmh_compat, ewmh_atom[_NET_WM_NAME],
                    utf8_string, 8, PropModeReplace, "lwm", 3);

    /* set root window properties */
    XChangeProperty(dpy, screens[i].root, ewmh_atom[_NET_SUPPORTED], XA_ATOM,
                    32, PropModeReplace, (unsigned char *)ewmh_atom,
                    EWMH_ATOM_LAST);

    XChangeProperty(dpy, screens[i].root, ewmh_atom[_NET_SUPPORTING_WM_CHECK],
                    XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)&screens[i].ewmh_compat, 1);

    unsigned long data[4];
    data[0] = 1;
    XChangeProperty(dpy, screens[i].root, ewmh_atom[_NET_NUMBER_OF_DESKTOPS],
                    XA_CARDINAL, 32, PropModeReplace, (unsigned char *)data, 1);

    data[0] = screens[i].display_width;
    data[1] = screens[i].display_height;
    XChangeProperty(dpy, screens[i].root, ewmh_atom[_NET_DESKTOP_GEOMETRY],
                    XA_CARDINAL, 32, PropModeReplace, (unsigned char *)data, 2);

    data[0] = 0;
    data[1] = 0;
    XChangeProperty(dpy, screens[i].root, ewmh_atom[_NET_DESKTOP_VIEWPORT],
                    XA_CARDINAL, 32, PropModeReplace, (unsigned char *)data, 2);

    data[0] = 0;
    XChangeProperty(dpy, screens[i].root, ewmh_atom[_NET_CURRENT_DESKTOP],
                    XA_CARDINAL, 32, PropModeReplace, (unsigned char *)data, 1);

    ewmh_set_strut(&screens[i]);
    ewmh_set_client_list(&screens[i]);
  }
}

EWMHWindowType ewmh_get_window_type(Window w) {
  Atom rt;
  Atom *type;
  int fmt;
  unsigned long n;
  unsigned long extra;
  int i = XGetWindowProperty(dpy, w, ewmh_atom[_NET_WM_WINDOW_TYPE], 0, 100,
                             False, XA_ATOM, &rt, &fmt, &n, &extra,
                             (unsigned char **)&type);
  if (i != Success || type == NULL) {
    return WTypeNone;
  }
  EWMHWindowType ret = WTypeNone;
  for (; n; n--) {
    if (type[n - 1] == ewmh_atom[_NET_WM_WINDOW_TYPE_DESKTOP]) {
      ret = WTypeDesktop;
      break;
    }
    if (type[n - 1] == ewmh_atom[_NET_WM_WINDOW_TYPE_DOCK]) {
      ret = WTypeDock;
      break;
    }
    if (type[n - 1] == ewmh_atom[_NET_WM_WINDOW_TYPE_TOOLBAR]) {
      ret = WTypeToolbar;
      break;
    }
    if (type[n - 1] == ewmh_atom[_NET_WM_WINDOW_TYPE_MENU]) {
      ret = WTypeMenu;
      break;
    }
    if (type[n - 1] == ewmh_atom[_NET_WM_WINDOW_TYPE_UTILITY]) {
      ret = WTypeUtility;
      break;
    }
    if (type[n - 1] == ewmh_atom[_NET_WM_WINDOW_TYPE_SPLASH]) {
      ret = WTypeSplash;
      break;
    }
    if (type[n - 1] == ewmh_atom[_NET_WM_WINDOW_TYPE_DIALOG]) {
      ret = WTypeDialog;
      break;
    }
    if (type[n - 1] == ewmh_atom[_NET_WM_WINDOW_TYPE_NORMAL]) {
      ret = WTypeNormal;
      break;
    }
  }
  XFree(type);
  return ret;
}

Bool ewmh_get_window_name(Client *c) {
#ifdef X_HAVE_UTF8_STRING
  Atom rt;
  char *name;
  int fmt;
  unsigned long n;
  unsigned long extra;
  int i = XGetWindowProperty(dpy, c->window, ewmh_atom[_NET_WM_NAME], 0, 100,
                             False, utf8_string, &rt, &fmt, &n, &extra,
                             (unsigned char **)&name);
  if (i != Success || name == NULL) {
    return False;
  }
  Client_Name(c, name, True);
  XFree(name);
  return True;
#else
  return False;
#endif
}

Bool ewmh_hasframe(Client *c) {
  switch (c->wtype) {
  case WTypeDesktop:
  case WTypeDock:
  case WTypeMenu:
  case WTypeSplash:
    return False;
  default:
    return True;
  }
}

void ewmh_get_state(Client *c) {
  if (c == NULL) {
    return;
  }
  Atom rt;
  Atom *state;
  int fmt;
  unsigned long n;
  unsigned long extra;
  int i = XGetWindowProperty(dpy, c->window, ewmh_atom[_NET_WM_STATE], 0, 100,
                             False, XA_ATOM, &rt, &fmt, &n, &extra,
                             (unsigned char **)&state);
  if (i != Success || state == NULL) {
    return;
  }
  c->wstate.skip_taskbar = False;
  c->wstate.skip_pager = False;
  c->wstate.fullscreen = False;
  c->wstate.above = False;
  c->wstate.below = False;
  for (; n; n--) {
    if (state[n - 1] == ewmh_atom[_NET_WM_STATE_SKIP_TASKBAR]) {
      c->wstate.skip_taskbar = True;
    }
    if (state[n - 1] == ewmh_atom[_NET_WM_STATE_SKIP_PAGER]) {
      c->wstate.skip_pager = True;
    }
    if (state[n - 1] == ewmh_atom[_NET_WM_STATE_FULLSCREEN]) {
      c->wstate.fullscreen = True;
    }
    if (state[n - 1] == ewmh_atom[_NET_WM_STATE_ABOVE]) {
      c->wstate.above = True;
    }
    if (state[n - 1] == ewmh_atom[_NET_WM_STATE_BELOW]) {
      c->wstate.below = True;
    }
  }
  XFree(state);
}

static Bool new_state(unsigned long action, Bool current) {
  enum Action { remove, add, toggle };
  switch (action) {
  case remove:
    return False;
  case add:
    return True;
  case toggle:
    return (current == True) ? False : True;
  }
  fprintf(stderr, "%s: bad action in _NET_WM_STATE (%d)\n", argv0, (int)action);
  return current;
}

void ewmh_change_state(Client *c, unsigned long action, unsigned long atom) {
  Atom *a = (Atom *)&atom;

  if (atom == 0) {
    return;
  }
  if (*a == ewmh_atom[_NET_WM_STATE_SKIP_TASKBAR]) {
    c->wstate.skip_taskbar = new_state(action, c->wstate.skip_taskbar);
  }
  if (*a == ewmh_atom[_NET_WM_STATE_SKIP_PAGER]) {
    c->wstate.skip_pager = new_state(action, c->wstate.skip_pager);
  }
  if (*a == ewmh_atom[_NET_WM_STATE_FULLSCREEN]) {
    Bool was_fullscreen = c->wstate.fullscreen;

    c->wstate.fullscreen = new_state(action, c->wstate.fullscreen);
    if (was_fullscreen == False && c->wstate.fullscreen == True) {
      Client_EnterFullScreen(c);
    }
    if (was_fullscreen == True && c->wstate.fullscreen == False) {
      Client_ExitFullScreen(c);
    }
  }
  if (*a == ewmh_atom[_NET_WM_STATE_ABOVE]) {
    c->wstate.above = new_state(action, c->wstate.above);
  }
  if (*a == ewmh_atom[_NET_WM_STATE_BELOW]) {
    c->wstate.below = new_state(action, c->wstate.below);
  }
  ewmh_set_state(c);

  /* may have to shuffle windows in the stack after a change of state */
  ewmh_set_client_list(c->screen);
}

void ewmh_set_state(Client *c) {
  if (c == NULL) {
    return;
  }
#define MAX_ATOMS 6
  Atom a[MAX_ATOMS];
  int atoms = 0;
  if (c->state != WithdrawnState) {
    if (c->hidden == True) {
      a[atoms++] = ewmh_atom[_NET_WM_STATE_HIDDEN];
    }
    if (c->wstate.skip_taskbar == True) {
      a[atoms++] = ewmh_atom[_NET_WM_STATE_SKIP_TASKBAR];
    }
    if (c->wstate.skip_pager == True) {
      a[atoms++] = ewmh_atom[_NET_WM_STATE_SKIP_PAGER];
    }
    if (c->wstate.fullscreen == True) {
      a[atoms++] = ewmh_atom[_NET_WM_STATE_FULLSCREEN];
    }
    if (c->wstate.above == True) {
      a[atoms++] = ewmh_atom[_NET_WM_STATE_ABOVE];
    }
    if (c->wstate.below == True) {
      a[atoms++] = ewmh_atom[_NET_WM_STATE_BELOW];
    }
  }
  if (atoms > MAX_ATOMS) {
    panic("too many atoms! Change MAX_ATOMS in ewmh_set_state");
  }
  XChangeProperty(dpy, c->window, ewmh_atom[_NET_WM_STATE], XA_ATOM, 32,
                  PropModeReplace, (unsigned char *)a, atoms);
#undef MAX_ATOMS
}

void ewmh_set_allowed(Client *c) {
  /* FIXME: this is dumb - the allowed actions should be calculated
   * but for now, anything goes.
   */
  Atom action[4];

  action[0] = ewmh_atom[_NET_WM_ACTION_MOVE];
  action[1] = ewmh_atom[_NET_WM_ACTION_RESIZE];
  action[2] = ewmh_atom[_NET_WM_ACTION_FULLSCREEN];
  action[3] = ewmh_atom[_NET_WM_ACTION_CLOSE];
  XChangeProperty(dpy, c->window, ewmh_atom[_NET_WM_ALLOWED_ACTIONS], XA_ATOM,
                  32, PropModeReplace, (unsigned char *)action, 4);
}

void ewmh_set_strut(ScreenInfo *screen) {
  /* find largest reserved areas */
  EWMHStrut strut;
  strut.left = 0;
  strut.right = 0;
  strut.top = 0;
  strut.bottom = 0;
  for (Client *c = client_head(); c; c = c->next) {
    if (c->screen != screen) {
      continue;
    }
    if (c->strut.left > strut.left) {
      strut.left = c->strut.left;
    }
    if (c->strut.right > strut.right) {
      strut.right = c->strut.right;
    }
    if (c->strut.top > strut.top) {
      strut.top = c->strut.top;
    }
    if (c->strut.bottom > strut.bottom) {
      strut.bottom = c->strut.bottom;
    }
  }

  /* if the reserved areas have not changed then we're done */
  if (screen->strut.left == strut.left && screen->strut.right == strut.right &&
      screen->strut.top == strut.top && screen->strut.bottom == strut.bottom) {
    return;
  }

  /* apply the new strut */
  screen->strut.left = strut.left;
  screen->strut.right = strut.right;
  screen->strut.top = strut.top;
  screen->strut.bottom = strut.bottom;

  /* set the new workarea */
  unsigned long data[4];
  data[0] = strut.left;
  data[1] = strut.top;
  data[2] = screen->display_width - (strut.left + strut.right);
  data[3] = screen->display_height - (strut.top + strut.bottom);
  XChangeProperty(dpy, screen->root, ewmh_atom[_NET_WORKAREA], XA_CARDINAL, 32,
                  PropModeReplace, (unsigned char *)data, 4);

  /* ensure no window fully occupy reserved areas */
  for (Client *c = client_head(); c; c = c->next) {
    int x = c->size.x;
    int y = c->size.y;

    if (c->wstate.fullscreen == True) {
      continue;
    }
    Edge backup = interacting_edge;
    interacting_edge = ENone;
    // Note: the only reason this doesn't crash (due to the last two args being
    // 0) is that dx and dy are only used when edge != ENone.
    // You have been warned.
    Client_MakeSane(c, ENone, &x, &y, 0, 0);
    interacting_edge = backup;
    if (c->framed == True) {
      XMoveWindow(dpy, c->parent, c->size.x, c->size.y - titleHeight());
    } else {
      XMoveWindow(dpy, c->parent, c->size.x, c->size.y);
    }
    sendConfigureNotify(c);
  }
}

/*
 * get _NET_WM_STRUT and if it is available recalculate the screens
 * reserved areas. the EWMH spec isn't clear about what we should do
 * about hidden windows. It seems silly to reserve space for an invisible
 * window, but the spec allows it. Ho Hum...		jfc
 */
void ewmh_get_strut(Client *c) {
  if (c == NULL) {
    return;
  }
  Atom rt;
  unsigned long *strut;
  int fmt;
  unsigned long n;
  unsigned long extra;
  int i = XGetWindowProperty(dpy, c->window, ewmh_atom[_NET_WM_STRUT], 0, 5,
                             False, XA_CARDINAL, &rt, &fmt, &n, &extra,
                             (unsigned char **)&strut);
  if (i != Success || strut == NULL || n < 4) {
    return;
  }
  c->strut.left = (unsigned int)strut[0];
  c->strut.right = (unsigned int)strut[1];
  c->strut.top = (unsigned int)strut[2];
  c->strut.bottom = (unsigned int)strut[3];
  ewmh_set_strut(c->screen);
}

/* fix stack forces each window on the screen to be in the right place in
 * the window stack as indicated in the EWMH spec version 1.2 (section 7.10).
 */
static void fix_stack(ScreenInfo *screen) {
  /* this is pretty dumb. we should query the tree and only move
   * those windows that require it. doing it regardless like this
   * causes the desktop to flicker
   */

  /* first lower clients with _NET_WM_STATE_BELOW */
  for (Client *c = client_head(); c; c = c->next) {
    if (c->wstate.below == False) {
      continue;
    }
    Client_Lower(c);
  }

  /* lower desktops - they are always the lowest */
  for (Client *c = client_head(); c; c = c->next) {
    if (c->wtype != WTypeDesktop) {
      continue;
    }
    Client_Lower(c);
    break; /* only one desktop, surely */
  }

  /* raise clients with _NET_WM_STATE_ABOVE and docks
   * (unless marked with _NET_WM_STATE_BELOW)
   */
  for (Client *c = client_head(); c; c = c->next) {
    if (!(c->wstate.above == True ||
          (c->wtype == WTypeDock && c->wstate.below == False))) {
      continue;
    }
    Client_Raise(c);
  }

  /* raise fullscreens - they're always on top */
  /* Misam Saki reports problems with this and believes fullscreens
   * should not be automatically raised.
   *
   * However if the code below is removed then the panel is raised above
   * fullscreens, which is not desirable.
   */
  for (Client *c = client_head(); c; c = c->next) {
    if (c->wstate.fullscreen == False) {
      continue;
    }
    Client_Raise(c);
  }
}

static Bool valid_for_client_list(ScreenInfo *screen, Client *c) {
  if (c->screen != screen) {
    return False;
  }
  if (c->state == WithdrawnState) {
    return False;
  }
  return True;
}

/*
* update_client_list updates the properties on the root window used by
* task lists and pagers.
*
* it should be called whenever the window stack is modified, or when clients
* are hidden or unhidden.
*/
void ewmh_set_client_list(ScreenInfo *screen) {
  if (screen == NULL || screen->ewmh_set_client_list == True) {
    return;
  }
  screen->ewmh_set_client_list = True;
  fix_stack(screen);
  int no_clients = 0;
  for (Client *c = client_head(); c; c = c->next) {
    if (valid_for_client_list(screen, c) == True) {
      no_clients++;
    }
  }
  Window *client_list = NULL;
  Window *stacked_client_list = NULL;
  if (no_clients > 0) {
    Window dw1;
    Window dw2;
    Window *wins = 0;
    unsigned int win;
    unsigned int nwins = 0;

    client_list = malloc(sizeof(Window) * no_clients);
    int i = no_clients - 1; /* array starts with oldest */
    for (Client *c = client_head(); c; c = c->next) {
      if (valid_for_client_list(screen, c) == True) {
        client_list[i] = c->window;
        i--;
        if (i < 0) {
          break;
        }
      }
    }

    stacked_client_list = malloc(sizeof(Window) * no_clients);
    i = 0;
    XQueryTree(dpy, screen->root, &dw1, &dw2, &wins, &nwins);
    for (win = 0; win < nwins; win++) {
      Client *c = Client_Get(wins[win]);
      if (!c) {
        continue;
      }
      if (valid_for_client_list(screen, c) == True) {
        stacked_client_list[i] = c->window;
        i++;
        if (i >= no_clients) {
          break;
        }
      }
    }
    if (nwins > 0) {
      XFree(wins);
    }
  }
  XChangeProperty(dpy, screen->root, ewmh_atom[_NET_CLIENT_LIST], XA_WINDOW, 32,
                  PropModeReplace, (unsigned char *)client_list, no_clients);
  XChangeProperty(dpy, screen->root, ewmh_atom[_NET_CLIENT_LIST_STACKING],
                  XA_WINDOW, 32, PropModeReplace,
                  (unsigned char *)stacked_client_list, no_clients);
  free(client_list);
  free(stacked_client_list);
  screen->ewmh_set_client_list = False;
}
