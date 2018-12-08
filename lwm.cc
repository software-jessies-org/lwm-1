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

#include "lwm.h"
#include "xlib.h"

Mode mode;    // The window manager's mode. (See "lwm.h".)
int start_x;  // The X position where the mode changed.
int start_y;  // The Y position where the mode changed.

Display* dpy;  // The connection to the X server.

XftFont* g_font;
XftColor g_font_active_title;
XftColor g_font_inactive_title;
XftColor g_font_popup_colour;

bool shape;       // Does server have Shape Window extension?
int shape_event;  // ShapeEvent event type.

// Atoms we're interested in. See the ICCCM for more information.
Atom wm_state;
Atom wm_change_state;
Atom wm_protocols;
Atom wm_delete;
Atom wm_take_focus;
Atom wm_colormaps;
Atom compound_text;

// Netscape uses this to give information about the URL it's displaying.
Atom _mozilla_url;

// Debugging support.
static void setDebugArg(char ch) {
  switch (ch) {
    case 'c':
      debug_configure_notify = true;
      break;
    case 'e':
      debug_all_events = true;
      break;
    case 'f':
      debug_focus = true;
      break;
    case 'm':
      debug_map = true;
      break;
    case 'p':
      debug_property_notify = true;
      break;
    default:
      fprintf(stderr, "Unrecognised debug option: '%c'\n", ch);
  }
}

bool debug_configure_notify;  // -d=c
bool debug_all_events;        // -d=e
bool debug_focus;             // -d=f
bool debug_map;               // -d=m
bool debug_property_notify;   // -d=p

bool printDebugPrefix(char const* file, int line) {
  char buf[16];
  time_t t;
  time(&t);
  struct tm tm = *localtime(&t);
  strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
  fprintf(stderr, "%s %s:%d : ", buf, file, line);
  return true;
}

// if we're really short of a clue we might look at motif hints, and
// we're not going to link with motif, so we'll have to do it by hand
Atom motif_wm_hints;

bool forceRestart;
char* argv0;

static void rrScreenChangeNotify(XEvent* ev);

/*ARGSUSED*/
extern int main(int argc, char* argv[]) {
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

  // Open a connection to the X server.
  dpy = XOpenDisplay(NULL);
  if (dpy == 0) {
    panic("can't open display.");
  }
  if (ScreenCount(dpy) != 1) {
    fprintf(stderr,
            "Sorry, LWM no longer supports multiple screens, and you "
            "have %d set up.\nPlease consider using xrandr.\n",
            ScreenCount(dpy));
  }
  Resources::Init();

  // Set up an error handler.
  XSetErrorHandler(errorHandler);

  // Set up signal handlers.
  signal(SIGTERM, Terminate);
  signal(SIGINT, Terminate);
  signal(SIGHUP, Terminate);

  // Ignore SIGCHLD.
  struct sigaction sa;
  sa.sa_handler = SIG_IGN;
#ifdef SA_NOCLDWAIT
  sa.sa_flags = SA_NOCLDWAIT;
#else
  sa.sa_flags = 0;
#endif
  sigemptyset(&sa.sa_mask);
  sigaction(SIGCHLD, &sa, 0);

  // Internalize useful atoms.
  wm_state = XInternAtom(dpy, "WM_STATE", false);
  wm_change_state = XInternAtom(dpy, "WM_CHANGE_STATE", false);
  wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS", false);
  wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", false);
  wm_take_focus = XInternAtom(dpy, "WM_TAKE_FOCUS", false);
  wm_colormaps = XInternAtom(dpy, "WM_COLORMAP_WINDOWS", false);
  compound_text = XInternAtom(dpy, "COMPOUND_TEXT", false);

  _mozilla_url = XInternAtom(dpy, "_MOZILLA_URL", false);

  motif_wm_hints = XInternAtom(dpy, "_MOTIF_WM_HINTS", false);

  ewmh_init();

  int screenID = DefaultScreen(dpy);
  std::string titleFont = Resources::I->Get(Resources::TITLE_FONT);
  g_font = XftFontOpenName(dpy, screenID, titleFont.c_str());
  if (g_font == nullptr) {
    fprintf(stderr, "Couldn't find font %s; falling back", titleFont.c_str());
    g_font = XftFontOpenName(dpy, 0, "fixed");
    if (g_font == nullptr) {
      panic("Can't find a font");
    }
  }
  XRenderColor xrc = Resources::I->GetXRenderColor(Resources::TITLE_COLOUR);
  XftColorAllocValue(dpy, DefaultVisual(dpy, screenID),
                     DefaultColormap(dpy, screenID), &xrc,
                     &g_font_active_title);
  xrc = Resources::I->GetXRenderColor(Resources::INACTIVE_TITLE_COLOUR);
  XftColorAllocValue(dpy, DefaultVisual(dpy, screenID),
                     DefaultColormap(dpy, screenID), &xrc,
                     &g_font_inactive_title);
  xrc = Resources::I->GetXRenderColor(Resources::POPUP_TEXT_COLOUR);
  XftColorAllocValue(dpy, DefaultVisual(dpy, screenID),
                     DefaultColormap(dpy, screenID), &xrc,
                     &g_font_popup_colour);

  LScr::I = new LScr(dpy);
  LScr::I->Init();
  // ewmh_init_screen();
  session_init(argc, argv);

  // Do we need to support XRandR?
  int rr_event_base, rr_error_base;
  bool have_rr = XRRQueryExtension(dpy, &rr_event_base, &rr_error_base);
  if (have_rr) {
    XRRSelectInput(dpy, LScr::I->Root(), RRScreenChangeNotifyMask);
  }

  // See if the server has the Shape Window extension.
  shape = serverSupportsShapes();

  // Initialisation is finished, but we start off not interacting with the
  // user.
  mode = wm_idle;

  // The main event loop.
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

static void rrScreenChangeNotify(XEvent* ev) {
  XRRScreenChangeNotifyEvent* rrev = (XRRScreenChangeNotifyEvent*)ev;
  const int nScrWidth = rrev->width;
  const int nScrHeight = rrev->height;
  // If my laptop is connected to a screen that is switched off, of I try
  // to switch to an external screen when none is connected, LWM gets this
  // event with a new size of 320x200. This forces all the windows to be
  // crushed into tiny little boxes, which is really annoying to repair
  // once I've got the external screen connected and X sorted out again.
  // A simple solution to this is to ignore any notifications smaller than
  // the smallest vaguely sensible size I can think of (and, TBH, this is
  // really too small to be sensible already).
  if (nScrWidth < 600 || nScrHeight < 400) {
    fprintf(stderr, "Ignoring tiny screen dimensions from xrandr: %dx%d\n",
            nScrWidth, nScrHeight);
    return;
  }
  LScr::I->ChangeScreenDimensions(nScrWidth, nScrHeight);
}

void sendConfigureNotify(Client* c) {
  XConfigureEvent ce;
  ce.type = ConfigureNotify;
  ce.event = c->window;
  ce.window = c->window;
  if (c->framed) {
    ce.x = c->size.x + borderWidth();
    ce.y = c->size.y + borderWidth();
    ce.width = c->size.width - 2 * borderWidth();
    ce.height = c->size.height - 2 * borderWidth();
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
  XSendEvent(dpy, c->window, false, StructureNotifyMask, (XEvent*)&ce);
}

/*ARGSUSED*/
extern void shell(int button) {
  std::string command;
  if (button == Button1) {
    command = Resources::I->Get(Resources::BUTTON1_COMMAND);
  } else if (button == Button2) {
    command = Resources::I->Get(Resources::BUTTON2_COMMAND);
  }
  if (command.empty()) {
    return;
  }

  const char* sh = getenv("SHELL");
  if (!sh) {
    sh = "/bin/sh";
  }
  const char* display_str = DisplayString(dpy);

  switch (fork()) {
    case 0:  // Child.
      close(ConnectionNumber(dpy));
      if (display_str) {
        const int len = strlen(display_str) + 9;
        char* str = (char*)malloc(len);
        snprintf(str, len, "DISPLAY=%s", display_str);
        putenv(str);
      }
      execl(sh, sh, "-c", command.c_str(), NULL);
      fprintf(stderr, "%s: can't exec \"%s -c %s\"\n", argv0, sh,
              command.c_str());
      execlp("xterm", "xterm", NULL);
      exit(EXIT_FAILURE);
    case -1:  // Error.
      fprintf(stderr, "%s: couldn't fork\n", argv0);
      break;
  }
}

extern int textHeight() {
  return g_font->height;
}

extern void drawString(Window w,
                       int x,
                       int y,
                       const std::string& s,
                       XftColor* c) {
  int screenID = DefaultScreen(dpy);
  XftDraw* draw = XftDrawCreate(dpy, w, DefaultVisual(dpy, screenID),
                                DefaultColormap(dpy, screenID));
  XftDrawStringUtf8(draw, c, g_font, x, y,
                    reinterpret_cast<const FcChar8*>(s.c_str()), s.size());
  XftDrawDestroy(draw);
}

// Returns the width of the given string in pixels, rendered in the LWM font.
extern int textWidth(const std::string& s) {
  XGlyphInfo extents;
  XftTextExtentsUtf8(dpy, g_font, reinterpret_cast<const FcChar8*>(s.c_str()),
                     s.size(), &extents);
  return extents.xOff;
}
