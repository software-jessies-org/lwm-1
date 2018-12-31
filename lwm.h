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

/* --- End of administrator-configurable defaults. --- */

/*
 * Window manager mode. wm is in one of five modes: it's either getting
 * user input to move/reshape a window, getting user input to make a
 * selection from the menu, waiting for user input to confirm a window close
 * (by releasing a mouse button after prssing it in a window's box),
 * waiting for user input to confirm a window hide (by releasing a mouse
 * button after prssing it in a window's frame),
 * or it's `idle' --- responding to events arriving
 * from the server, but not directly interacting with the user.
 * OK, so I lied: there's a sixth mode so that we can tell when wm's still
 * initialising.
 */
enum Mode {
  wm_initialising,
  wm_idle,
  wm_reshaping,
  wm_menu_up,
  wm_closing_window,
  wm_hiding_window
};

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

struct Rect {
  int xMin;
  int yMin;
  int xMax;
  int yMax;

  bool contains(int x, int y) const {
    return x >= xMin && y >= yMin && x <= xMax && y <= yMax;
  }

  int width() { return xMax - xMin; }
  int height() { return yMax - yMin; }
};

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
        wtype(WTypeNone),
        ncmapwins(0),
        cmapwins(nullptr),
        wmcmaps(nullptr) {
#define ZERO_STRUCT(x) memset(&x, 0, sizeof(x))
    ZERO_STRUCT(wstate);
    ZERO_STRUCT(strut);
    ZERO_STRUCT(size);
    ZERO_STRUCT(return_size);
    ZERO_STRUCT(cmap);
#undef ZERO_STRUCT
  }

  ~Client() {
    if (ncmapwins) {
      XFree(cmapwins);
      free(wmcmaps);
    }
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

  Window window;  // Client's window.
  Window parent;  // Window manager frame.
  Window trans;   // Window that client is a transient for.

  bool framed;  // true is lwm is maintaining a frame

  int border;  // Client's original border width.

  XSizeHints size;         // Client's current geometry information.
  XSizeHints return_size;  // Client's old geometry information.

  int State() const { return state_; }
  void SetState(int state);
  bool IsHidden() const { return state_ == IconicState; }
  bool IsWithdrawn() const { return state_ == WithdrawnState; }
  bool IsNormal() const { return state_ == NormalState; }
  
  bool HasFocus() const;
  static Client* FocusedClient();
  
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

  // Colourmap scum.
  Colormap cmap;
  int ncmapwins;
  Window* cmapwins;
  Colormap* wmcmaps;

  void SetIconPixmap(Pixmap icon, Pixmap mask);
  ImageIcon* Icon() { return icon_; }

 private:
  Rect edgeBounds(Edge e) const;

  std::string name_;  // Name used for title in frame.
  ImageIcon* icon_ = nullptr;

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
};

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

  int Width() const { return width_; }
  int Height() const { return height_; }
  void ChangeScreenDimensions(int nScrWidth, int nScrHeight);

  unsigned long InactiveBorder() const { return inactive_border_; }
  unsigned long ActiveBorder() const { return active_border_; }
  CursorMap* Cursors() const { return cursor_map_; }

  GC GetGC() { return gc_; }
  GC GetMenuGC() { return menu_gc_; }
  GC GetTitleGC() { return title_gc_; }

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

  // Clients() returns the map of all clients, for iteration.
  const std::map<Window, Client*>& Clients() const { return clients_; }

  // This is used as a static pointer to the global LScr instance, initialised
  // on start-up in lwm.cc.
  static LScr* I;
  
  static constexpr int kOnlyScreenIndex = 0;
  
 private:
  void initEWMH();
  void scanWindowTree();
  Client* addClient(Window w);
  unsigned long black() const { return BlackPixel(dpy_, kOnlyScreenIndex); }
  unsigned long white() const { return WhitePixel(dpy_, kOnlyScreenIndex); }

  Display* dpy_ = nullptr;
  Window root_ = 0;
  int width_ = 0;
  int height_ = 0;
  CursorMap* cursor_map_;

  Hider hider_;

  // The clients_ map is keyed by the top-level client Window ID. The values
  // are owned.
  std::map<Window, Client*> clients_;

  // The parents_ map is keyed by the LWM furniture windows when they are
  // created. It does not own the values (they're just pointers to the same
  // clients as in the clients_ map).
  std::map<Window, Client*> parents_;

  Atom utf8_string_atom_;

  Window popup_ = 0;
  Window ewmh_compat_ = 0;

  EWMHStrut strut_;  // reserved areas

  GC gc_;
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

/* lwm.cc */
extern Mode mode;
extern int start_x;
extern int start_y;
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
extern Atom wm_colormaps;
extern Atom compound_text;
extern bool shape;
extern int shape_event;
extern char* argv0;
extern bool forceRestart;
extern void shell(int);
extern void sendConfigureNotify(Client*);

// Debugging support (in lwm.cc).
extern bool debug_configure_notify;  // -d=c
extern bool debug_all_events;        // -d=e
extern bool debug_focus;             // -d=f
extern bool debug_map;               // -d=m
extern bool debug_property_notify;   // -d=p
extern bool printDebugPrefix(char const* filename, int line);

#define DBGF_IF(cond, fmt, ...)                         \
  do {                                                  \
    if (cond && printDebugPrefix(__FILE__, __LINE__)) { \
      fprintf(stderr, fmt, ##__VA_ARGS__);              \
      fputc('\n', stderr);                              \
    }                                                   \
  } while (0)

#define DBG_IF(cond, str)                               \
  do {                                                  \
    if (cond && printDebugPrefix(__FILE__, __LINE__)) { \
      fputs(str, stderr);                               \
      fputc('\n', stderr);                              \
    }                                                   \
  } while (0)

#define DBG(str) DBG_IF(1, str)
#define DBGF(fmt, ...) DBGF_IF(1, fmt, ##__VA_ARGS__)

/* client.cc */
extern Edge interacting_edge;
extern void Client_MakeSane(Client*, Edge, int*, int*, int*, int*);
extern void Client_DrawBorder(Client*, int);
extern void Client_SizeFeedback();
extern void size_expose();
extern void Client_ReshapeEdge(Client*, Edge);
extern void Client_Move(Client*);
extern void Client_Raise(Client*);
extern void Client_Lower(Client*);
extern void Client_Close(Client*);
extern void Client_Remove(Client*);
extern void Client_FreeAll();
extern void Client_ColourMap(XEvent*);
extern void Client_EnterFullScreen(Client* c);
extern void Client_ExitFullScreen(Client* c);
extern void Client_Focus(Client* c, Time time);
extern void Client_ResetAllCursors();

/* disp.cc */
extern void dispatch(XEvent*);
extern void reshaping_motionnotify(XEvent*);

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
extern void cmapfocus(Client*);
extern void getColourmaps(Client*);
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
    POPUP_TEXT_COLOUR,
    POPUP_BACKGROUND_COLOUR,
    S_END,  // This must be the last.
  };

  // The types of int resource on offer.
  enum IR {
    I_BEGIN,  // Don't use this.
    BORDER_WIDTH,
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

 private:
  Resources();
  void set(SR res,
           XrmDatabase db,
           const std::string& name,
           const char* cls,
           const std::string& dflt);
  void set(IR res,
           XrmDatabase db,
           const std::string& name,
           const char* cls,
           int dflt);

  std::vector<std::string> strings_;
  std::vector<int> ints_;
};

// Handy accessors which parse resources if necessary, and return the relevant
// bit of config info.
int borderWidth();

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

#endif  // LWM_LWM_H_included
