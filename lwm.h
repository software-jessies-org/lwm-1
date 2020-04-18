#ifndef LWM_LWM_H_included
#define LWM_LWM_H_included
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

#include <list>
#include <map>
#include <string>

#include "log.h"
#include "xlib.h"

/* --- Administrator-configurable defaults. --- */

#define HIDE_BUTTON Button3
#define MOVE_BUTTON Button2
#define RESHAPE_BUTTON Button1

// MOVING_BUTTON_MASK describes the bits which are set in the mouse statis mask
// value while either of the mouse buttons we can use for dragging/reshaping
// is down.
#define MOVING_BUTTON_MASK (Button1Mask | Button2Mask)

#define EDGE_RESIST 32

// How many pixels to move the auto-placement location down and to the right,
// after each window is placed.
#define AUTO_PLACEMENT_INCREMENT 40

/* --- End of administrator-configurable defaults. --- */

/** Window internal state. Yuck. */
enum IState { IPendingReparenting, INormal };

/**
 * Window edge, used in resizing. The `edge' ENone is used to signify a
 * window move rather than a resize. The code is sufficiently similar that
 * this isn't a special case to be treated separately.
 */
enum Edge {
  ETopLeft,
  ETop,
  ETopRight,
  ERight,
  ENone,
  ELeft,
  EBottomLeft,
  EBottom,
  EBottomRight,
  EClose,     // Special 'Edge' to denote the close icon.
  EContents,  // Special again: not any action, it's the client window.
  E_LAST
};

/**
 * EWMH direction for _NET_WM_MOVERESIZE
 */
enum EWMHDirection {
  DSizeTopLeft,
  DSizeTop,
  DSizeTopRight,
  DSizeRight,
  DSizeBottomRight,
  DSizeBottom,
  DSizeBottomLeft,
  DSizeLeft,
  DMove,
  DSizeKeyboard,
  DMoveKeyboard
};

/**
 * EWMH window type. See section 5.6 of the EWMH specification (1.2).
 * WTypeNone indicates that no EWMH window type as been set and MOTIF
 * hints should be used instead.
 */
enum EWMHWindowType {
  WTypeDesktop,
  WTypeDock,
  WTypeToolbar,
  WTypeMenu,
  WTypeUtility,
  WTypeSplash,
  WTypeDialog,
  WTypeNormal,
  WTypeNone
};

/**
 * EWMH window state, See section 5.7 of the EWMH specification (1.2).
 * lwm does not support all states. _NET_WM_STATE_HIDDEN is taken from
 * Client.hidden.
 */
struct EWMHWindowState {
  bool skip_taskbar;
  bool skip_pager;
  bool fullscreen;
  bool above;
  bool below;
};

std::ostream& operator<<(std::ostream& os, const EWMHWindowState& s);
std::ostream& operator<<(std::ostream& os, const XSizeHints& s);

/**
 * EWMH "strut", or area on each edge of the screen reserved for docking
 * bars/panels.
 */
struct EWMHStrut {
  unsigned int left;
  unsigned int right;
  unsigned int top;
  unsigned int bottom;
};

struct Point {
  int x;
  int y;

  // Returns b - a.
  static Point Sub(Point a, Point b);
};

struct Rect {
  int xMin;
  int yMin;
  int xMax;
  int yMax;

  bool contains(int x, int y) const {
    return x >= xMin && y >= yMin && x < xMax && y < yMax;
  }

  int width() const { return xMax - xMin; }
  int height() const { return yMax - yMin; }
  int area() const { return width() * height(); }
  bool empty() const { return area() == 0; }

  Point middle() const { return Point{(xMin + xMax) / 2, (yMin + yMax) / 2}; }

  inline bool operator==(const Rect& o) const {
    return xMin == o.xMin && yMin == o.yMin && xMax == o.xMax && yMax == o.yMax;
  }

  inline bool operator!=(const Rect& o) const { return !operator==(o); }

  static Rect FromXYWH(int x, int y, int w, int h);

  // Returns a new Rect which is shifted by the given x and y translation.
  static Rect Translate(Rect r, Point p);

  // Returns the intersection of the two rectangles or, if they don't intersect,
  // the empty rectangle 0,0,0,0.
  static Rect Intersect(const Rect& a, const Rect& b);

  // Parse rectangles in X11 style (1280x960+23+25).
  // Returns the canonical empty rectangle if parsing fails.
  static Rect Parse(std::string str);
};

std::ostream& operator<<(std::ostream& os, const Point& p);
std::ostream& operator<<(std::ostream& os, const Rect& r);
std::ostream& operator<<(std::ostream& os, const std::vector<Rect>& rs);

class Client {
 public:
  Client(Window w, Window parent)
      : window(w),
        parent(parent),
        trans(0),
        framed(false),
        border(0),
        state_(WithdrawnState),
        hidden(false),
        internal_state(INormal),
        proto(0),
        accepts_focus(true),
        cursor(ENone),
        wtype(WTypeNone) {
#define ZERO_STRUCT(x) memset(&x, 0, sizeof(x))
    ZERO_STRUCT(wstate);
    ZERO_STRUCT(strut);
    ZERO_STRUCT(size);
    ZERO_STRUCT(return_size);
#undef ZERO_STRUCT
  }

  ~Client() {
    LOGI() << "Deleting client for " << name_;
    delete icon_;
  }

  void SetName(const char* c, int len);
  const std::string& Name() const { return name_; }
  std::string MenuName() const;

  // Returns the edge corresponding to the action to be performed on the window.
  // The special cases 'EClose' and
  Edge EdgeAt(Window w, int x, int y) const;

  void Hide();
  void Unhide();

  void EnterFullScreen();
  void ExitFullScreen();

  void SendConfigureNotify();

  Window window;  // Client's window.
  Window parent;  // Window manager frame.
  Window trans;   // Window that client is a transient for.

  bool framed;  // true is lwm is maintaining a frame

  int border;  // Client's original border width.

  XSizeHints size;  // Client's current geometry information.

 private:
  // return_size stores the original size of the client window when it enters
  // full screen state, so it can be correctly brought out of full screen state
  // again.
  XSizeHints return_size;

 public:
  int State() const { return state_; }
  void SetState(int state);
  bool IsHidden() const { return state_ == IconicState; }
  bool IsWithdrawn() const { return state_ == WithdrawnState; }
  bool IsNormal() const { return state_ == NormalState; }

  bool HasFocus() const;
  static Client* FocusedClient();

  // Notifications to the Client that it has gained or lost focus.
  void FocusGained();
  void FocusLost();

  // Draws the contents of the furniture window.
  void DrawBorder();

  bool HasStruts() const {
    return strut.top || strut.bottom || strut.left || strut.right;
  }

  void SetSize(const Rect& r);

  // Rect defining the bounds of the window, either including LWM's window
  // furniture (WithBorder) or not (NoBorder).
  Rect RectWithBorder() const;
  Rect RectNoBorder() const;

 private:
  int state_;  // Window state. See ICCCM and <X11/Xutil.h>
 public:
  bool hidden;  // true if this client is hidden.
  IState internal_state;
  int proto;

  bool accepts_focus;  // Does this window want keyboard events?

  Edge cursor;  // indicates which cursor is being used for parent window

  EWMHWindowType wtype;
  EWMHWindowState wstate;
  EWMHStrut strut;  // reserved areas

  // SetIcon sets the window's title bar icon. If called with null, it will do
  // nothing (and leave any previously-set icon in place).
  void SetIcon(ImageIcon* icon);
  ImageIcon* Icon() { return icon_; }

 private:
  Rect EdgeBounds(Edge e) const;

  std::string name_;  // Name used for title in frame.
  ImageIcon* icon_ = nullptr;

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
};

// WinID is only used to pretty-print window IDs in hex.
struct WinID {
  explicit WinID(Window w) : w(w) {}
  Window w;
};

std::ostream& operator<<(std::ostream& os, const Client& c);
std::ostream& operator<<(std::ostream& os, const WinID& w);

class CursorMap {
 public:
  explicit CursorMap(Display* dpy);

  // Root() returns the standard pointer cursor we use most places, including
  // over the root window.
  Cursor Root() const { return root_; }

  // ForEdge returns the cursor appropriate to the given edge. This may be
  // arrows for the resizing areas, or a nice big 'X' for EClose.
  // Returns the same as Root() if there's no specific cursor for some edge.
  Cursor ForEdge(Edge e) const;

 private:
  Cursor root_;
  std::map<Edge, Cursor> edges_;
};

// Hider implements all the logic to do with hiding and unhiding windows, and
// providing the unhide menu.
class Hider {
 public:
  Hider() = default;

  void Hide(Client* c);
  void Unhide(Client* c);

  void OpenMenu(XButtonEvent* ev);
  void Paint();
  void MouseMotion(XEvent* ev);
  void MouseRelease(XEvent* ev);

 private:
  int itemAt(int x, int y) const;
  void drawHighlight(int itemIndex);
  void showHighlightBox(int itemIndex);
  void hideHighlightBox();

  struct Item {
    Item(Window w, bool hidden) : w(w), hidden(hidden) {}

    Window w;
    std::string name;
    bool hidden;
  };

  // hidden_ is updated any time a window is hidden or unhidden.
  std::list<Window> hidden_;

  // The following fields are changed when the menu is opened, and then used
  // to display the menu, handle mouse events etc. It is not changed by windows
  // opening and closing while the hide menu is open.
  int x_min_ = 0;
  int y_min_ = 0;
  int width_ = 0;
  int height_ = 0;
  int current_item_ = 0;  // Index of currently-selected item.
  std::vector<Item> open_content_;

  Window highlightL = 0;
  Window highlightR = 0;
  Window highlightT = 0;
  Window highlightB = 0;
};

class Focuser {
 public:
  Focuser() = default;

  // Notification that the mouse pointer has entered a window. This may or may
  // not result in a change of input focus.
  void EnterWindow(Window w, Time time);

  // Forces the focuser to forget the given client (either because the window
  // has been hidden or destroyed). If this was the currently-focused client,
  // focus will be transferred to the previously-focused client.
  void UnfocusClient(Client* c);

  // Forcibly gives focus to a client (possibly because the client requested it,
  // possibly because it's just been unhidden or created). Does nothing if the
  // given client already has input focus.
  void FocusClient(Client* c, Time time = CurrentTime);

  Client* GetFocusedClient();

 private:
  void RemoveFromHistory(Client* c);

  // Does the actual work of FocusClient, except without the safety-check for
  // 'do we currently have focus?'. This is needed to make the refocusing of
  // older-focused clients work when a window closes.
  void ReallyFocusClient(Client* c, Time time, bool give_focus);

  // last_entered_ is the last window the mouse pointer was seen entering. It
  // is *NOT* necessarily the window with input focus. In fact, if a new window
  // was opened and was given focus, the pointer may be over a completely
  // different (and unfocused) window.
  Window last_entered_;

  // The following list contains the history of focused windows. The Focuser
  // is notified of all window destructions, and must keep this list free of
  // Client pointers that are no longer valid.
  std::list<Client*> focus_history_;
};

// Screen information.
class LScr {
 public:
  explicit LScr(Display* dpy);

  // Init must be called once, immediately after the global LScr::I instance
  // has been assigned to this instance.
  void Init();

  Display* Dpy() const { return dpy_; }
  Window Root() const { return root_; }
  Window Popup() const { return popup_; }
  Window Menu() const { return menu_; }

  int Width() const { return width_; }
  int Height() const { return height_; }
  void ChangeScreenDimensions(int nScrWidth, int nScrHeight);

  unsigned long InactiveBorder() const { return inactive_border_; }
  unsigned long ActiveBorder() const { return active_border_; }
  CursorMap* Cursors() const { return cursor_map_; }

  GC GetCloseIconGC(bool active) { return active ? gc_ : inactive_gc_; }
  GC GetMenuGC() { return menu_gc_; }
  GC GetTitleGC() { return title_gc_; }

  // Sets the screen areas which are visible.
  // For one-monitor systems, this will be a single rectangle.
  // For multi-screen systems this will consist of one rect for each screen.
  // These may form a larger rectangle (eg 2 identical-sized monitors), or
  // there may be several unevenly-sized screens, and at arbitrary relative
  // positions. Essentially, anything supported by xrandr.
  // This includes areas which overlap.
  // Calling this function will cause all client windows to be resized and
  // repositioned if necessary to ensure they're still accessible.
  void SetVisibleAreas(std::vector<Rect> visible_areas);

  // Returns the rectangle describing the 'main' screen area. This is chosen
  // essentially by finding the largest monitor, and if there are several with
  // the same size, tie-breaking according to which has the lower Y, followed
  // by which has the lower X.
  // If withStruts is true, only the part of the visible area not used by
  // strutting furniture will be returned.
  Rect GetPrimaryVisibleArea(bool withStruts) const;

  // Returns all the visible areas. The areas returned are returned in no
  // specific order, and will abut *or overlap*.
  std::vector<Rect> VisibleAreas(bool withStruts) const;

  // Expose the utf8 string atom. This is used by ewmh.cc. Not sure why it can't
  // go in the main enumerated set of atoms, and indeed this whole atom support
  // looks like it needs refactoring. For now, though, ugly hack here:
  Atom GetUTF8StringAtom() const { return utf8_string_atom_; }

  const EWMHStrut& Strut() const { return strut_; }
  // ChangeStrut returns true if the new struts are different from the old.
  bool ChangeStrut(const EWMHStrut& strut);

  // GetClient returns the Client which owns the given window (including if w
  // is a sub-window of the main client window). Returns nullptr if there is
  // no client allocated for this window.
  Client* GetClient(Window w) const;

  // GetOrAddClient either returns the existing client, or creates a new one
  // and generates relevant window furniture. This may return nullptr if the
  // window should not be owned.
  Client* GetOrAddClient(Window w);

  void Furnish(Client* c);

  void Remove(Client* client);

  Hider* GetHider() { return &hider_; }
  Focuser* GetFocuser() { return &focuser_; }

  // Clients() returns the map of all clients, for iteration.
  const std::map<Window, Client*>& Clients() const { return clients_; }

  // This is used as a static pointer to the global LScr instance, initialised
  // on start-up in lwm.cc.
  static LScr* I;

  static constexpr int kOnlyScreenIndex = 0;

 private:
  void InitEWMH();
  void ScanWindowTree();
  Client* addClient(Window w);
  unsigned long black() const { return BlackPixel(dpy_, kOnlyScreenIndex); }
  unsigned long white() const { return WhitePixel(dpy_, kOnlyScreenIndex); }

  Display* dpy_ = nullptr;
  Window root_ = 0;
  int width_ = 0;
  int height_ = 0;
  std::vector<Rect> visible_areas_;
  CursorMap* cursor_map_;

  Hider hider_;
  Focuser focuser_;

  // The clients_ map is keyed by the top-level client Window ID. The values
  // are owned.
  std::map<Window, Client*> clients_;

  // The parents_ map is keyed by the LWM furniture windows when they are
  // created. It does not own the values (they're just pointers to the same
  // clients as in the clients_ map).
  std::map<Window, Client*> parents_;

  Atom utf8_string_atom_;

  Window popup_ = 0;
  Window menu_ = 0;
  Window ewmh_compat_ = 0;

  EWMHStrut strut_;  // reserved areas

  GC gc_;
  GC inactive_gc_;
  GC menu_gc_;
  GC title_gc_;

  // Extra colours.
  unsigned long inactive_border_ = 0;
  unsigned long active_border_ = 0;
};

/*
 * c->proto is a bitarray of these
 */
enum { Pdelete = 1, Ptakefocus = 2 };

/*
 * This should really have been in X.h --- if you select both ButtonPress
 * and ButtonRelease events, the server makes an automatic grab on the
 * pressed button for you. This is almost always exactly what you want.
 */
#define ButtonMask (ButtonPressMask | ButtonReleaseMask)

class DragHandler {
 public:
  DragHandler() = default;
  virtual ~DragHandler() = default;

  virtual void Start(XEvent* ev) = 0;
  // Return false to cancel the action immediately.
  virtual bool Move(XEvent* ev) = 0;
  virtual void End(XEvent* ev) = 0;
};

/* lwm.cc */
extern bool is_initialising;  // Set during start-up, cleared after.
extern Display* dpy;

// New, pretty fonts:
extern XftFont* g_font;
extern XftColor g_font_active_title;
extern XftColor g_font_inactive_title;
extern XftColor g_font_popup_colour;

// Functions for dealing with new pretty fonts:
extern int textHeight();
extern int textWidth(const std::string& s);
extern void drawString(Window w,
                       int x,
                       int y,
                       const std::string& s,
                       XftColor* c);

extern Atom _mozilla_url;
extern Atom motif_wm_hints;
extern Atom wm_state;
extern Atom wm_change_state;
extern Atom wm_protocols;
extern Atom wm_delete;
extern Atom wm_take_focus;
extern Atom compound_text;
extern bool shape;
extern int shape_event;
extern char* argv0;
extern bool forceRestart;
extern void shell(int);

/* client.cc */
extern bool Client_MakeSane(Client*, Edge, int, int, int, int);
extern bool Client_MakeSaneAndMove(Client* c,
                                   Edge edge,
                                   int x,
                                   int y,
                                   int w,
                                   int h);
extern void Client_SizeFeedback();
extern void size_expose();
extern void Client_Raise(Client*);
extern void Client_Lower(Client*);
extern void Client_Close(Client*);
extern void Client_Remove(Client*);
extern void Client_FreeAll();
extern void Client_ResetAllCursors();
extern int titleBarHeight();

/* disp.cc */
extern void DispatchXEvent(XEvent*);

/* error.cc */
extern int ignore_badwindow;
extern int errorHandler(Display*, XErrorEvent*);
extern void panic(const char*);

/* manage.cc */
extern void getWindowName(Client*);
extern void getNormalHints(Client*);
extern bool motifWouldDecorate(Client*);
extern void manage(Client*);
extern void withdraw(Client*);
extern void getTransientFor(Client*);
extern void Terminate(int);

/* mouse.cc */
struct MousePos {
  int x;
  int y;
  // For mask values, see:
  // https://tronche.com/gui/x/xlib/events/keyboard-pointer/keyboard-pointer.html
  unsigned int modMask;
};

extern MousePos getMousePosition();
extern int menuItemHeight();

/* shape.cc */
extern int shapeEvent(XEvent*);
extern int serverSupportsShapes();
extern int isShaped(Window);
extern void setShape(Client*);

/* resource.cc */
class Resources {
 public:
  static Resources* I;

  // Init must be called once, at program start.
  static void Init();

  // The types of string resource on offer.
  enum SR {
    S_BEGIN,  // Don't use this.
    TITLE_FONT,
    BUTTON1_COMMAND,
    BUTTON2_COMMAND,
    TITLE_BG_COLOUR,
    BORDER_COLOUR,
    INACTIVE_BORDER_COLOUR,
    WINDOW_HIGHLIGHT_COLOUR,
    TITLE_COLOUR,
    INACTIVE_TITLE_COLOUR,
    CLOSE_ICON_COLOUR,
    INACTIVE_CLOSE_ICON_COLOUR,
    POPUP_TEXT_COLOUR,
    POPUP_BACKGROUND_COLOUR,
    FOCUS_MODE,
    APP_ICON,
    S_END,  // This must be the last.
  };

  // The types of int resource on offer.
  enum IR {
    I_BEGIN,  // Don't use this.
    BORDER_WIDTH,
    TOP_BORDER_WIDTH,
    I_END,  // This must be the last.
  };

  // Retrieve a string resource.
  const std::string& Get(SR r);

  // Retrieve a string resource as a colour.
  unsigned long GetColour(SR r);

  // Retrieve a string resource as an XRenderColor (used for Xft fonts).
  XRenderColor GetXRenderColor(SR r);

  // Retrieve an int resource.
  int GetInt(IR r);

  // Retrieve the 'click to focus' resource (as a bool).
  bool ClickToFocus() {
    std::string fm = Get(FOCUS_MODE);
    // std::string== doesn't seem to ever return true; using old-fashioned
    // strcmp instead.
    return !strcmp(fm.c_str(), "click");
  }

  // Interpret the APP_ICON resource for the cases in which we need it.
  bool ProcessAppIcons() {
    std::string ai = Get(APP_ICON);
    return strcmp(ai.c_str(), "none");
  }
  bool AppIconInWindowTitle() {
    std::string ai = Get(APP_ICON);
    return !strcmp(ai.c_str(), "both") || !strcmp(ai.c_str(), "title");
  }
  bool AppIconInUnhideMenu() {
    std::string ai = Get(APP_ICON);
    return !strcmp(ai.c_str(), "both") || !strcmp(ai.c_str(), "menu");
  }

 private:
  Resources();
  void Set(SR res,
           XrmDatabase db,
           const std::string& name,
           const char* cls,
           const std::string& dflt);
  void Set(IR res,
           XrmDatabase db,
           const std::string& name,
           const char* cls,
           int dflt);

  std::vector<std::string> strings_;
  std::vector<int> ints_;
};

// Implements the built-in debug CLI. This is only available if -debugcli is
// passed on the command line.
class DebugCLI {
 public:
  DebugCLI();
  void Read();
  // Init runs commands on start-up (provided on the command line), and prints
  // out the hello message. Should be run even if there are no commands.
  void Init(const std::vector<std::string>& init_commands);

  static bool DebugEnabled(const Client* c);
  static std::string NameFor(const Client* c);

  // Called by LScr on client appearance/disappearance. Has no effect if
  // debugging is disabled.
  static void NotifyClientAdd(Client* c);
  static void NotifyClientRemove(Client* c);

 private:
  void ProcessLine(std::string line);
  void CmdXRandr(std::string line);
  void CmdDbg(std::string line);
  void ResetDeadZones(const std::vector<Rect>& visible);
  bool IsDebugEnabled(const Client* c);
  bool DisableDebugging(Window w);
  std::string LookupNameFor(const Client* c);

  bool debug_new_;

  // Windows which cover the areas of the desktop that are not visible, due to
  // the debug CLI fake xrandr commands.
  std::vector<Window> dead_zones_;

  // Windows we're debugging (value is their dbg name).
  std::map<Window, std::string> debug_windows_;
};

// String functions.
extern std::vector<std::string> Split(const std::string& in,
                                      const std::string& split);

// Handy accessors which parse resources if necessary, and return the relevant
// bit of config info.
int borderWidth();
int topBorderWidth();

/* session.cc */
extern int ice_fd;
extern void session_init(int argc, char* argv[]);
extern void session_process();
extern void session_end();

/* ewmh.cc */
extern Atom ewmh_atom[];
extern void ewmh_init();
extern EWMHWindowType ewmh_get_window_type(Window w);
extern bool ewmh_get_window_name(Client* c);
extern ImageIcon* ewmh_get_window_icon(Client* c);
extern bool ewmh_hasframe(Client* c);
extern void ewmh_set_state(Client* c);
extern void ewmh_get_state(Client* c);
extern void ewmh_change_state(Client* c,
                              unsigned long action,
                              unsigned long atom);
extern void ewmh_set_allowed(Client* c);
extern void ewmh_set_client_list();
extern void ewmh_get_strut(Client* c);
extern void ewmh_set_strut();
extern char const* ewmh_atom_name(Atom at);

// geometry.cc
extern bool isLeftEdge(Edge e);
extern bool isRightEdge(Edge e);
extern bool isTopEdge(Edge e);
extern bool isBottomEdge(Edge e);

// tests.cc
extern bool RunAllTests();

#endif  // LWM_LWM_H_included
