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

#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xos.h>
#include <X11/Xutil.h>

#include "ewmh.h"
#include "lwm.h"

static int current_item; /* Last known selected menu item. -1 if none. */

struct menuitem {
  Client *client;
  menuitem *next;
};

static menuitem *hidden_menu = 0;

// Left and right margins on the hidden window menu.
#define MENU_MARGIN 20
#define MENU_Y_PADDING 6

static void getMenuDimensions(int *width, int *height, int *length) {
  *length = 0;
  int w = 0; // Widest string so far.
  for (Client *c = client_head(); c; c = c->next) {
    if (!c->framed) {
      continue;
    }
    (*length)++;
    int tw = textWidth(c->MenuName()) + 2 * MENU_MARGIN;
    if (tw > w) {
      w = tw;
    }
  }
  *width = w + borderWidth();
  *height = textHeight() + MENU_Y_PADDING;
}

MousePos getMousePosition() {
  Window root, child;
  MousePos res;
  memset(&res, 0, sizeof(res));
  int t1, t2;
  XQueryPointer(dpy, screen->root, &root, &child, &res.x, &res.y, &t1, &t2,
                &res.modMask);
  return res;
}

int menu_whichitem(int x, int y) {
  int width;  /* Width of menu. */
  int height; /* Height of each menu item. */
  int length; /* Number of items on the menu. */

  getMenuDimensions(&width, &height, &length);

  /*
   * Translate to popup window coordinates. We do this ourselves to avoid
   * a round trip to the server.
   */
  x -= start_x;
  y -= start_y;

  /*
   * Are we outside the menu?
   */
  if (x < 0 || x > width || y < 0 || y >= length * height) {
    return -1;
  }
  return y / height;
}

void menuhit(XButtonEvent *e) {
  Client_ResetAllCursors();

  int width;  /* Width of menu. */
  int height; /* Height of each menu item. */
  int length; /* Number of menu items. */
  getMenuDimensions(&width, &height, &length);
  if (length == 0) {
    return;
  }

  /*
   * Arrange for centre of first menu item to be under pointer,
   * unless that would put the menu offscreen.
   */
  start_x = e->x - width / 2;
  start_y = e->y - height / 2;

  if (start_x + width > screen->display_width) {
    start_x = screen->display_width - width;
  }
  if (start_x < 0) {
    start_x = 0;
  }
  if (start_y + (height * length) > screen->display_height) {
    start_y = screen->display_height - (height * length);
  }
  if (start_y < 0) {
    start_y = 0;
  }

  current_item = menu_whichitem(e->x_root, e->y_root);
  XMoveResizeWindow(dpy, screen->popup, start_x, start_y, width,
                    length * height);
  XMapRaised(dpy, screen->popup);
  XChangeActivePointerGrab(dpy,
                           ButtonMask | ButtonMotionMask | OwnerGrabButtonMask,
                           None, CurrentTime);

  mode = wm_menu_up;
}

void hide(Client *c) {
  if (c == 0) {
    return;
  }

  /* Create new menu item, and thread it on the menu. */
  menuitem *newitem = (menuitem *)malloc(sizeof(menuitem));
  if (newitem == 0) {
    return;
  }
  newitem->client = c;
  newitem->next = hidden_menu;
  hidden_menu = newitem;

  /* Actually hide the window. */
  XUnmapWindow(dpy, c->parent);
  XUnmapWindow(dpy, c->window);

  c->hidden = true;

  /* If the window was the current window, it isn't any more... */
  if (c == current) {
    Client_Focus(NULL, CurrentTime);
  }
  Client_SetState(c, IconicState);
}

void unhide(int n, int map) {
  // Find the nth client, first by checking those which are hidden (they appear
  // at the top of the menu), and then checking the unhidden windows (which
  // appear below the dotted line).
  if (n < 0) {
    return;
  }

  menuitem *prev = 0;
  menuitem *m = hidden_menu;
  while (n > 0 && m != 0) {
    prev = m;
    m = m->next;
    n--;
  }
  Client *c = NULL;
  if (m != 0) {
    c = m->client;

    /* Remove the item from the menu, and dispose of it. */
    if (prev == 0) {
      hidden_menu = m->next;
    } else {
      prev->next = m->next;
    }
    free(m);

    c->hidden = false;
  } else {
    // It's not a hidden item, so try to find it in the list of non-hidden
    // clients.
    for (c = client_head(); c; c = c->next) {
      if (!c->framed || c->hidden) {
        continue;
      }
      if (n-- == 0) {
        break;
      }
    }
  }

  // If we found a client, unhide it.
  if (map && c) {
    XMapWindow(dpy, c->parent);
    XMapWindow(dpy, c->window);
    Client_Raise(c);
    Client_SetState(c, NormalState);
    // It feels right that the unhidden window gets focus always.
    Client_Focus(c, CurrentTime);
  }
}

void unhidec(Client *c, int map) {
  if (c == 0) {
    return;
  }

  /* My goodness, how the world sucks. */
  int i = 0;
  for (menuitem *m = hidden_menu; m != 0; m = m->next, i++) {
    if (m->client == c) {
      unhide(i, map);
      return;
    }
  }
}

static void draw_menu_item(Client *c, int i, int height) {
  int ty = i * height + g_font->ascent;
  drawString(screen->popup, MENU_MARGIN, ty + MENU_Y_PADDING / 2, c->MenuName(),
             &g_font_black);
}

void menu_expose() {
  int width;  /* Width of each item. */
  int height; /* Height of each item. */
  int length; /* Number of menu items. */
  getMenuDimensions(&width, &height, &length);

  /* Redraw the labels. */
  int i = 0;
  for (menuitem *m = hidden_menu; m != 0; m = m->next, i++) {
    draw_menu_item(m->client, i, height);
  }

  // Draw a dashed line between the hidden and non-hidden items.
  XSetLineAttributes(dpy, screen->menu_gc, 1, LineOnOffDash, CapButt,
                     JoinMiter);
  XDrawLine(dpy, screen->popup, screen->menu_gc, 0, height * i, width,
            height * i);

  // Draw the labels for non-hidden items.
  for (Client *c = client_head(); c; c = c->next) {
    if (!c->framed || c->hidden) {
      continue;
    }
    draw_menu_item(c, i++, height);
  }

  /* Highlight current item if there is one. */
  if (current_item >= 0 && current_item < length) {
    XFillRectangle(dpy, screen->popup, screen->menu_gc, 0,
                   current_item * height, width, height);
  }
}

void menu_motionnotify(XEvent *ev) {
  int width;  /* Width of menu. */
  int height; /* Height of each menu item. */
  int length; /* Number of menu items. */
  getMenuDimensions(&width, &height, &length);

  int old = current_item; // Old menu position.
  XButtonEvent *e = &ev->xbutton;
  current_item = menu_whichitem(e->x_root, e->y_root);

  if (current_item == old) {
    return;
  }

  /* Unhighlight the old position, if it was on the menu. */
  if (old >= 0 && old < length) {
    XFillRectangle(dpy, screen->popup, screen->menu_gc, 0, old * height, width,
                   height);
  }

  /* Highlight the new position, if it's on the menu. */
  if (current_item >= 0 && current_item < length) {
    XFillRectangle(dpy, screen->popup, screen->menu_gc, 0,
                   current_item * height, width, height);
  }
}

void menu_buttonrelease(XEvent *ev) {
  /*
   * Work out which menu item the button was released over.
   */
  int n = menu_whichitem(ev->xbutton.x_root, ev->xbutton.y_root);

  /* Hide the menu until it's needed again. */
  XUnmapWindow(dpy, screen->popup); /*BUG?*/

  /* Do the menu thing (of unhiding windows). */
  unhide(n, 1);

  if (current) {
    cmapfocus(current);
  }
}
