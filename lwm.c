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

#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <signal.h>

#include <X11/X.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xos.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>

#include "lwm.h"

Mode mode;   /* The window manager's mode. (See "lwm.h".) */
int start_x; /* The X position where the mode changed. */
int start_y; /* The Y position where the mode changed. */

Display *dpy;        /* The connection to the X server. */
int screen_count;    /* The number of screens. */
ScreenInfo *screens; /* Information about these screens. */
ScreenInfo *current_screen;

XFontSet font_set = NULL; /* Font set for title var */
XFontSetExtents *font_set_ext = NULL;
XFontSet popup_font_set = NULL; /* Font set for popups */
XFontSetExtents *popup_font_set_ext = NULL;

Bool shape;      /* Does server have Shape Window extension? */
int shape_event; /* ShapeEvent event type. */

/* Atoms we're interested in. See the ICCCM for more information. */
Atom wm_state;
Atom wm_change_state;
Atom wm_protocols;
Atom wm_delete;
Atom wm_take_focus;
Atom wm_colormaps;
Atom compound_text;

/** Netscape uses this to give information about the URL it's displaying. */
Atom _mozilla_url;


// Debugging support.
static void setDebugArg(char ch) {
  switch (ch) {
  case 'c':
    debug_configure_notify = True;
    break;
  case 'e':
    debug_all_events = True;
    break;
  case 'f':
    debug_focus = True;
    break;
  case 'm':
    debug_map = True;
    break;
  case 'p':
    debug_property_notify = True;
    break;
  default:
    fprintf(stderr, "Unrecognised debug option: '%c'\n", ch);
  }
}

Bool debug_configure_notify;  // -d=c
Bool debug_all_events;        // -d=e
Bool debug_focus;             // -d=f
Bool debug_map;               // -d=m
Bool debug_property_notify;   // -d=p

Bool printDebugPrefix(char const* file, int line) {
  char buf[16];
  time_t t;
  time(&t);
  struct tm tm = *localtime(&t);
  strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
  fprintf(stderr, "%s %s:%d : ", buf, file, line);
  return True;
}

/*
 * if we're really short of a clue we might look at motif hints, and
 * we're not going to link with motif, so we'll have to do it by hand
 */
Atom motif_wm_hints;

Bool forceRestart;
char *argv0;

static void initScreens(void);
static void initScreen(int);

static void rrScreenChangeNotify(XEvent *ev);
static void rrNotify(XEvent *ev);

/*ARGSUSED*/
extern int main(int argc, char *argv[]) {
  argv0 = argv[0];
  for (int i = 1; i < argc; i++) {
    if (!strncmp(argv[i], "-d=", 3)) {
      for (int j = 3; argv[i][j]; j++) {
        setDebugArg(argv[i][j]);
      }
    }
  }

  mode = wm_initialising;

  setlocale(LC_ALL, "");

  /* Open a connection to the X server. */
  dpy = XOpenDisplay(NULL);
  if (dpy == 0) {
    panic("can't open display.");
  }

  parseResources();

  /* Set up an error handler. */
  XSetErrorHandler(errorHandler);

  /* Set up signal handlers. */
  signal(SIGTERM, Terminate);
  signal(SIGINT, Terminate);
  signal(SIGHUP, Terminate);

  /* Ignore SIGCHLD. */
  struct sigaction sa;
  sa.sa_handler = SIG_IGN;
#ifdef SA_NOCLDWAIT
  sa.sa_flags = SA_NOCLDWAIT;
#else
  sa.sa_flags = 0;
#endif
  sigemptyset(&sa.sa_mask);
  sigaction(SIGCHLD, &sa, 0);

  /* Internalize useful atoms. */
  wm_state = XInternAtom(dpy, "WM_STATE", False);
  wm_change_state = XInternAtom(dpy, "WM_CHANGE_STATE", False);
  wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
  wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
  wm_take_focus = XInternAtom(dpy, "WM_TAKE_FOCUS", False);
  wm_colormaps = XInternAtom(dpy, "WM_COLORMAP_WINDOWS", False);
  compound_text = XInternAtom(dpy, "COMPOUND_TEXT", False);

  _mozilla_url = XInternAtom(dpy, "_MOZILLA_URL", False);

  motif_wm_hints = XInternAtom(dpy, "_MOTIF_WM_HINTS", False);
  
  ewmh_init();

  /*
   * Get fonts for our titlebar and our popup window. We try to
   * get Lucida, but if we can't we make do with fixed because everyone
   * has that.
   */
  /* FIXME: do these need to be freed? */
  char **missing;
  char *def;
  int missing_count;

  font_set = XCreateFontSet(dpy, font_name, &missing, &missing_count, &def);
  if (font_set == NULL) {
    font_set = XCreateFontSet(dpy, "fixed", &missing, &missing_count, &def);
  }
  if (font_set == NULL) {
    panic("unable to create font set for title font");
  }
  if (missing_count > 0) {
    fprintf(stderr, "%s: warning: missing %d charset"
                    "%s for title font\n",
            argv0, missing_count, (missing_count == 1) ? "" : "s");
  }
  font_set_ext = XExtentsOfFontSet(font_set);

  popup_font_set =
      XCreateFontSet(dpy, popup_font_name, &missing, &missing_count, &def);
  if (popup_font_set == NULL) {
    popup_font_set =
        XCreateFontSet(dpy, "fixed", &missing, &missing_count, &def);
  }
  if (popup_font_set == NULL) {
    panic("unable to create font set for popup font");
  }
  if (missing_count > 0) {
    fprintf(stderr, "%s: warning: missing %d charset"
                    "%s for popup font\n",
            argv0, missing_count, (missing_count == 1) ? "" : "s");
  }
  popup_font_set_ext = XExtentsOfFontSet(popup_font_set);

  initScreens();
  ewmh_init_screens();
  session_init(argc, argv);
  
  // Do we need to support XRandR?
  int rr_event_base, rr_error_base;
  Bool have_rr = XRRQueryExtension(dpy, &rr_event_base, &rr_error_base);
  if (have_rr) {
    for (int i = 0; i < screen_count; i++) {
      XRRSelectInput(dpy, screens[i].root, RRScreenChangeNotifyMask);
    }
  }
  
  /* See if the server has the Shape Window extension. */
  shape = serverSupportsShapes();

  /*
   * Initialisation is finished, but we start off not interacting with the
   * user.
   */
  mode = wm_idle;

  /*
   * The main event loop.
   */
  int dpy_fd = ConnectionNumber(dpy);
  int max_fd = dpy_fd + 1;
  if (ice_fd > dpy_fd) {
    max_fd = ice_fd + 1;
  }
  while (!forceRestart) {
    fd_set readfds;

    FD_ZERO(&readfds);
    FD_SET(dpy_fd, &readfds);
    if (ice_fd > 0) {
      FD_SET(ice_fd, &readfds);
    }
    if (select(max_fd, &readfds, NULL, NULL, NULL) > -1) {
      if (FD_ISSET(dpy_fd, &readfds)) {
        while (XPending(dpy)) {
          XEvent ev;
          XNextEvent(dpy, &ev);
          // xrandr notifications have arbitrary numbers, so check for them
          // before trying the static selection.
          if (ev.type == rr_event_base + RRScreenChangeNotify) {
            rrScreenChangeNotify(&ev);
          } else {
            dispatch(&ev);
          }
        }
      }
      if (ice_fd > 0 && FD_ISSET(ice_fd, &readfds)) {
        session_process();
      }
    }
  }
  // Someone hit us with a SIGHUP: better exec ourselves to force a config
  // reload and cope with changing screen sizes.
  execvp(argv0, argv);
}

// scaleSize scales *size so that it occupies the same proportion of 'news' as
// it did 'olds' (these are new and old screen sizes in the same dimension),
// with the caveat that any change will be made in multiples of inc.
static int scaleSize(int olds, int news, int size, int inc) {
  int add = ((size * news) / olds) - size;
  // 'add' can, of course, be negative. That's fine.
  add -= (add % inc);
  return size + add;
}

// moveOrChangeSize takes in an old and new size (olds, news) describing the
// change in one screen dimension (width or height). It takes pos, which is the
// corresponding left or top location on that dimension, and size which is the
// extent of the window in that dimension. Additionally inc describes the
// increment in pixels of window size.
// On return, *pos and *size are updated to represent a 'nice' positioning in
// the new size.
// In general, we try not to change the size, but rather shift the window
// towards its closest screen edge proportionally to the size of the screen.
// Thus, if a window started off 5% of the way across the screen from the left,
// it will still be 5% across from the left on return.
// On the other hand, if we detect that the window is trying to take up all the
// screen space in some dimension (eg top and bottom of the window are within
// 5% of the screen height of the top and bottom edges of the screen), we grow
// the window in that dimension to ensure it still takes up the same proportion
// of the screen size.
static void moveOrChangeSize(int olds, int news, int *pos, int *size, int inc) {
  if (inc < 1) {
    inc = 1;
  }
  int opos = *pos;
  int osize = *size;
  // For clarity, comments assume we're talking about the X dimension. This is
  // not necessarily the case, but it's easier to visualise if we pick one
  // dimension.
  int nearDist = *pos;                 // Distance from left edge of screen.
  int farDist = olds - (*pos + *size); // Distance from right edge of screen.
  Bool nearClose = (nearDist * 20 < olds); // Very close to left edge?
  Bool farClose = (farDist * 20 < olds);   // Very close to right edge?

  if (nearClose && farClose) {
    // Window is full width; scale it up or down keeping the left and right
    // edges the same distance from the screen edge.
    *size += ((news - olds) / inc) * inc;
  } else {
    // If we're not scaling the window because it's full-screen, then we need to
    // check its size to ensure it doesn't exceed the new screen size, and scale
    // it down appropriately if it does. The general rule we'll apply is that if
    // the screen is shrinking, and the new window size is 90% of the screen
    // size
    // or larger, then we'll scale it proportionally.
    if (news < olds && (*size * 10 / 9) > news) {
      // Too big; scale down.
      *size = scaleSize(olds, news, *size, inc);
    }
    // The window size is going to be alright now, but how we move the window
    // depends on its original position. If it was close to the left or right
    // edges, it will follow them; if it was somewhere floating in the middle
    // ground, we will move the window such that its central point will occupy
    // the same proportional location of the screen, but adjusting to stop the
    // window edge going off the side.
    if (nearClose) {
      *pos = (*pos * news) / olds;
    } else if (farClose) {
      farDist = (farDist * news) / olds;
      *pos = news - farDist - *size;
    } else {
      *pos = (*pos * (news - *size)) / (olds - *size);
    }
  }
}

static void rrScreenChangeNotify(XEvent *ev) {
  XRRScreenChangeNotifyEvent *rrev = (XRRScreenChangeNotifyEvent *)ev;
  for (int i = 0; i < screen_count; i++) {
    if (screens[i].root != rrev->root) {
      continue;
    }
    const int oScrWidth = screens[i].display_width;
    const int oScrHeight = screens[i].display_height;
    const int nScrWidth = rrev->width;
    const int nScrHeight = rrev->height;
    // We've found the right screen. Refresh our idea of its size.
    // Note that we don't call DisplayWidth() and DisplayHeight() because they
    // seem to always return the original size, while the change notify event
    // has the updated size.
    if (oScrWidth == nScrWidth && oScrHeight == nScrHeight) {
      return;  // Don't process the same event multiple times.
    }
    screens[i].display_width = nScrWidth;
    screens[i].display_height = nScrHeight;
    // Now, go through the windows and adjust their sizes and locations to conform to the new screen layout.
    for (Client *c = client_head(); c; c = c->next) {
      int x = c->size.x;
      int y = c->size.y;
      const int oldx = x;
      const int oldy = y;
      const int oldw = c->size.width;
      const int oldh = c->size.height;

      moveOrChangeSize(oScrWidth, nScrWidth, &x, &(c->size.width),
                       c->size.width_inc);
      moveOrChangeSize(oScrHeight, nScrHeight, &y, &(c->size.height),
                       c->size.height_inc);
      Edge backup = interacting_edge;
      interacting_edge = ENone;
      // Note: the only reason this doesn't crash (due to the last two args
      // being 0) is that dx and dy are only used when edge != ENone.
      // You have been warned.
      Client_MakeSane(c, ENone, &x, &y, 0, 0);
      interacting_edge = backup;
      XMoveResizeWindow(dpy, c->parent, c->size.x, c->size.y - titleHeight(),
                        c->size.width, c->size.height + titleHeight());
      if (c->size.width == oldw && c->size.height == oldh) {
        if (c->size.x != oldx || c->size.y != oldy) {
          sendConfigureNotify(c);
        }
      } else {
        XMoveResizeWindow(dpy, c->window, border, border + titleHeight(),
                          c->size.width - 2 * border,
                          c->size.height - 2 * border);
      }
    }
  }
}

void sendConfigureNotify(Client *c) {
  XConfigureEvent ce;
  ce.type = ConfigureNotify;
  ce.event = c->window;
  ce.window = c->window;
  if (c->framed == True) {
    ce.x = c->size.x + border;
    ce.y = c->size.y + border;
    ce.width = c->size.width - 2 * border;
    ce.height = c->size.height - 2 * border;
    ce.border_width = c->border;
  } else {
    ce.x = c->size.x;
    ce.y = c->size.y;
    ce.width = c->size.width;
    ce.height = c->size.height;
    ce.border_width = c->border;
  }
  ce.above = None;
  ce.override_redirect = 0;
  XSendEvent(dpy, c->window, False, StructureNotifyMask, (XEvent *)&ce);
}

extern void scanWindowTree(int screen) {
  unsigned int nwins = 0;
  Window dw1;
  Window dw2;
  Window *wins = 0;
  XWindowAttributes attr;

  XQueryTree(dpy, screens[screen].root, &dw1, &dw2, &wins, &nwins);
  for (int i = 0; i < nwins; i++) {
    XGetWindowAttributes(dpy, wins[i], &attr);
    if (attr.override_redirect || wins[i] == screens[screen].popup) {
      continue;
    }
    Client *c = Client_Add(wins[i], screens[screen].root);
    if (c != 0 && c->window == wins[i]) {
      c->screen = &screens[screen];
      c->size.x = attr.x;
      c->size.y = attr.y;
      c->size.width = attr.width;
      c->size.height = attr.height;
      c->border = attr.border_width;
      if (attr.map_state == IsViewable) {
        c->internal_state = IPendingReparenting;
        manage(c, 1);
      }
    }
  }
  if (wins) {
    XFree(wins); /* wins==0 should be impossible; paranoia. */
  }
}

/*ARGSUSED*/
extern void shell(ScreenInfo *screen, int button, int x, int y) {
  /* Get the command we're to execute. Give up if there isn't one. */
  const char *command = NULL;
  if (button == Button1) {
    command = btn1_command;
  } else if (button == Button2) {
    command = btn2_command;
  }
  if (!command) {
    return;
  }

  const char *sh = getenv("SHELL");
  if (!sh) {
    sh = "/bin/sh";
  }

  switch (fork()) {
  case 0: /* Child. */
    close(ConnectionNumber(dpy));
    if (screen && screen->display_spec != 0) {
      putenv(screen->display_spec);
    }
    execl(sh, sh, "-c", command, NULL);
    fprintf(stderr, "%s: can't exec \"%s -c %s\"\n", argv0, sh, command);
    execlp("xterm", "xterm", NULL);
    exit(EXIT_FAILURE);
  case -1: /* Error. */
    fprintf(stderr, "%s: couldn't fork\n", argv0);
    break;
  }
}

extern int titleHeight(void) { return font_set_ext->max_logical_extent.height; }

extern int ascent(XFontSetExtents *font_set_ext) {
  return abs(font_set_ext->max_logical_extent.y);
}

extern int popupHeight(void) {
  return popup_font_set_ext->max_logical_extent.height;
}

extern int titleWidth(XFontSet font_set, Client *c) {
  if (c == NULL) {
    return 0;
  }
  char *name;
  int namelen;

  if (c->menu_name == NULL) {
    name = c->name;
    namelen = c->namelen;
  } else {
    name = c->menu_name;
    namelen = c->menu_namelen;
  }
  if (name == NULL) {
    return 0;
  }
  XRectangle ink;
  XRectangle logical;
#ifdef X_HAVE_UTF8_STRING
  if (c->name_utf8 == True)
    Xutf8TextExtents(font_set, name, namelen, &ink, &logical);
  else
#endif
    XmbTextExtents(font_set, name, namelen, &ink, &logical);

  return logical.width;
}

extern int popupWidth(char *string, int string_length) {
  XRectangle ink;
  XRectangle logical;

  XmbTextExtents(popup_font_set, string, string_length, &ink, &logical);

  return logical.width;
}

static void initScreens(void) {
  /* Find out how many screens we've got, and allocate space for their info. */
  screen_count = ScreenCount(dpy);
  screens = (ScreenInfo *)malloc(screen_count * sizeof(ScreenInfo));

  /* Go through the screens one-by-one, initialising them. */
  for (int screen = 0; screen < screen_count; screen++) {
    initialiseCursors(screen);
    initScreen(screen);
    scanWindowTree(screen);
  }
}

static void initScreen(int screen) {
  /* Set the DISPLAY specification. */
  char *display_string = DisplayString(dpy);
  char *colon = strrchr(display_string, ':');
  if (colon) {
    char *dot = strrchr(colon, '.');
    const int len = 9 + strlen(display_string) + ((dot == 0) ? 2 : 0) + 10;
    screens[screen].display_spec = (char *)malloc(len);
    sprintf(screens[screen].display_spec, "DISPLAY=%s", display_string);
    if (!dot) {
      dot = screens[screen].display_spec + len - 3;
    } else {
      dot = strrchr(screens[screen].display_spec, '.');
    }
    sprintf(dot, ".%i", screen);
  } else {
    screens[screen].display_spec = 0;
  }

  /* Find the root window. */
  screens[screen].root = RootWindow(dpy, screen);
  screens[screen].display_width = DisplayWidth(dpy, screen);
  screens[screen].display_height = DisplayHeight(dpy, screen);
  screens[screen].strut.left = 0;
  screens[screen].strut.right = 0;
  screens[screen].strut.top = 0;
  screens[screen].strut.bottom = 0;

  /* Get the pixel values of the only two colours we use. */
  screens[screen].black = BlackPixel(dpy, screen);
  screens[screen].white = WhitePixel(dpy, screen);
  XColor colour, exact;
  XAllocNamedColor(dpy, DefaultColormap(dpy, screen), "DimGray", &colour,
                   &exact);
  screens[screen].gray = colour.pixel;

  /* Set up root (frame) GC's. */
  XGCValues gv;
  gv.foreground = screens[screen].black ^ screens[screen].white;
  gv.background = screens[screen].white;
  gv.function = GXxor;
  gv.line_width = 1;
  gv.subwindow_mode = IncludeInferiors;
  screens[screen].gc_thin = XCreateGC(dpy, screens[screen].root,
                                      GCForeground | GCBackground | GCFunction |
                                          GCLineWidth | GCSubwindowMode,
                                      &gv);

  gv.line_width = 2;
  screens[screen].gc = XCreateGC(dpy, screens[screen].root,
                                 GCForeground | GCBackground | GCFunction |
                                     GCLineWidth | GCSubwindowMode,
                                 &gv);

  /* Create a window for our popup. */
  screens[screen].popup =
      XCreateSimpleWindow(dpy, screens[screen].root, 0, 0, 1, 1, 1,
                          screens[screen].black, screens[screen].white);
  XSetWindowAttributes attr;
  attr.event_mask = ButtonMask | ButtonMotionMask | ExposureMask;
  XChangeWindowAttributes(dpy, screens[screen].popup, CWEventMask, &attr);

  /* Create menu GC. */
  gv.line_width = 1;
  screens[screen].menu_gc = XCreateGC(dpy, screens[screen].popup,
                                      GCForeground | GCBackground | GCFunction |
                                          GCLineWidth | GCSubwindowMode,
                                      &gv);

  /* Create size indicator GC. */
  gv.foreground = screens[screen].black;
  gv.function = GXcopy;
  screens[screen].size_gc = XCreateGC(dpy, screens[screen].popup,
                                      GCForeground | GCBackground | GCFunction |
                                          GCLineWidth | GCSubwindowMode,
                                      &gv);

  /* Announce our interest in the root window. */
  attr.cursor = screens[screen].root_cursor;
  attr.event_mask = SubstructureRedirectMask | SubstructureNotifyMask |
                    ColormapChangeMask | ButtonPressMask | PropertyChangeMask |
                    EnterWindowMask;
  XChangeWindowAttributes(dpy, screens[screen].root, CWCursor | CWEventMask,
                          &attr);

  /* Make sure all our communication to the server got through. */
  XSync(dpy, False);
}

/**
Find the screen for which root is the root window.
*/
ScreenInfo *getScreenFromRoot(Window root) {
  for (int screen = 0; screen < screen_count; screen++) {
    if (screens[screen].root == root) {
      return &screens[screen];
    }
  }
  return 0;
}
