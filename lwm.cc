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

bool is_initialising;
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
int fake_screen_areas;        // -fakescreen

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
static void setScreenAreasFromXRandR();

// Enable this using the -fakescreen flag. When this is used, we pretend that
// xrandr has told us our display area is divided in half, with a left and a
// right monitor. We adjust the left so it begins some pixels down from the
// top, and the right side extends to some pixels up from the bottom.
// This is for testing xrandr support on VNC (which itself doesn't support
// xrandr, at least the one I'm using).
static void setFakeScreenAreasForTesting() {
  const int w = DisplayWidth(dpy, 0);
  const int h = DisplayHeight(dpy, 0);
  std::vector<Rect> rects;
  rects.push_back(Rect{0, fake_screen_areas, w / 2, h});
  rects.push_back(Rect{w / 2, 0, w, h - fake_screen_areas});
  LScr::I->SetVisibleAreas(rects);
}

std::vector<std::string> Split(const std::string& in,
                               const std::string& split) {
  std::vector<std::string> res;
  int start = 0;
  while (true) {
    int end = in.find(split, start);
    if (end == std::string::npos) {
      res.push_back(in.substr(start));
      return res;
    }
    res.push_back(in.substr(start, end - start));
    start = end + split.size();
  }
}

/*ARGSUSED*/
extern int main(int argc, char* argv[]) {
  DebugCLI* debugCLI = nullptr;
  argv0 = argv[0];
  std::vector<std::string> debug_init_commands;
  for (int i = 1; i < argc; i++) {
    if (!strncmp(argv[i], "-d=", 3)) {
      for (int j = 3; argv[i][j]; j++) {
        setDebugArg(argv[i][j]);
      }
    } else if (!strncmp(argv[i], "-fakescreen=", 12)) {
      fake_screen_areas = atoi(argv[i] + 12);
    } else if (!strncmp(argv[i], "-debugcli", 9)) {
      debugCLI = new DebugCLI;
      if (argv[i][9] == '=') {
        // Argument is a sequence of commands, separated by ;.
        debug_init_commands = Split(std::string(argv[i]+10), ";");
      }
    }
  }

  is_initialising = true;
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
  session_init(argc, argv);

  // Do we need to support XRandR?
  int rr_event_base, rr_error_base;
  bool have_rr = XRRQueryExtension(dpy, &rr_event_base, &rr_error_base);
  if (have_rr) {
    XRRSelectInput(dpy, LScr::I->Root(), RRScreenChangeNotifyMask);
    setScreenAreasFromXRandR();
  }

  // If the user has run us with 'fake screen areas', then set up two pretend
  // visible areas, to simulate running on a two-monitor system.
  if (fake_screen_areas) {
    setFakeScreenAreasForTesting();
  }

  // See if the server has the Shape Window extension.
  shape = serverSupportsShapes();

  // Initialisation is finished; from now on, errors are not going to be fatal.
  is_initialising = false;

  // The main event loop.
  int dpy_fd = ConnectionNumber(dpy);
  int max_fd = dpy_fd + 1;
  if (ice_fd > dpy_fd) {
    max_fd = ice_fd + 1;
  }
  
  // Just before we start the loop, execute any commands we've been told to
  // run on start-up.
  if (debugCLI) {
    debugCLI->Init(debug_init_commands);
  }
  
  while (!forceRestart) {
    fd_set readfds;

    FD_ZERO(&readfds);
    FD_SET(dpy_fd, &readfds);
    if (ice_fd > 0) {
      FD_SET(ice_fd, &readfds);
    }
    if (debugCLI) {
      FD_SET(STDIN_FILENO, &readfds);
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
      if (debugCLI && FD_ISSET(STDIN_FILENO, &readfds)) {
        debugCLI->Read();
      }
    }
  }
  // Someone hit us with a SIGHUP: better exec ourselves to force a config
  // reload and cope with changing screen sizes.
  execvp(argv0, argv);
}

static void rrScreenChangeNotify(XEvent* ev) {
  XRRScreenChangeNotifyEvent* rrev = (XRRScreenChangeNotifyEvent*)ev;
  int nScrWidth = rrev->width;
  int nScrHeight = rrev->height;
  // If my laptop is connected to a screen that is switched off, of I try
  // to switch to an external screen when none is connected, LWM gets this
  // event with a new size of 320x200. This forces all the windows to be
  // crushed into tiny little boxes, which is really annoying to repair
  // once I've got the external screen connected and X sorted out again.
  // A simple solution to this is to ignore any notifications smaller than
  // the smallest vaguely sensible size I can think of (and, TBH, this is
  // really too small to be sensible already).
  if (nScrWidth < 600 || nScrHeight < 400) {
    LOGW() << "Ignoring tiny screen dimensions from xrandr: " << nScrWidth
           << "x" << nScrHeight;
    return;
  }

  static long lastSerial;
  if (rrev->serial == lastSerial) {
    LOGI() << "Dropping duplicate event for serial " << lastSerial;
    return;  // Drop duplicate message (we get lots of these).
  }
  lastSerial = rrev->serial;
  setScreenAreasFromXRandR();
}

static void setScreenAreasFromXRandR() {
  XRRScreenResources* res = XRRGetScreenResourcesCurrent(dpy, LScr::I->Root());
  if (!res) {
    LOGE() << "Failed to get XRRScreenResources";
    return;
  }
  XFreer res_freer((void*)res);
  if (!res->ncrtc) {
    LOGE() << "Empty list of CRTs";
    return;
  }
  // Ignore any CRT with mode==0.
  // Change nScrWidth/nScrHeight according to the total extent of all visible
  // areas, and don't rely on the size provided in the event itself. This is
  // because when switching from internal+external monitors to internal only,
  // the first couple of notifications claim the old area. However, querying
  // the CRT info already gets the correct sizes and locations (including
  // mode=0 for those that are disabled).
  std::vector<Rect> visible;
  for (int i = 0; i < res->ncrtc; i++) {
    const RRCrtc crt = res->crtcs[i];
    LOGI() << "Looking up CRT " << i << ": " << crt;
    XRRCrtcInfo* crtInfo = XRRGetCrtcInfo(dpy, res, crt);
    LOGI() << "  CRT size " << crtInfo->width << "x" << crtInfo->height
           << ", offset " << crtInfo->x << "," << crtInfo->y
           << " (mode=" << crtInfo->mode << ")";
    if (!crtInfo->mode) {
      continue;
    }
    const int xMin = crtInfo->x;
    const int yMin = crtInfo->y;
    const int xMax = xMin + crtInfo->width;
    const int yMax = yMin + crtInfo->height;
    visible.push_back(Rect{xMin, yMin, xMax, yMax});
  }
  LScr::I->SetVisibleAreas(visible);
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
