#include "ewmh.h"
#include "lwm.h"
#include "xlib.h"

// The static LScr instance.
LScr* LScr::I;

LScr::LScr(Display* dpy)
    : dpy_(dpy),
      root_(RootWindow(dpy, kOnlyScreenIndex)),
      width_(DisplayWidth(dpy, kOnlyScreenIndex)),
      height_(DisplayHeight(dpy, kOnlyScreenIndex)),
      cursor_map_(new CursorMap(dpy)),
      utf8_string_atom_(XInternAtom(dpy, "UTF8_STRING", false)),
      strut_{0, 0, 0, 0} {}

void LScr::Init() {
  active_border_ = Resources::I->GetColour(Resources::BORDER_COLOUR);
  inactive_border_ = Resources::I->GetColour(Resources::INACTIVE_BORDER_COLOUR);

  // The graphics context used for the menu is a simple exclusive OR which will
  // toggle pixels between black and white. This allows us to implement
  // highlights really easily.
  XGCValues gv;
  gv.foreground = black() ^ white();
  gv.background = white();
  gv.function = GXxor;
  gv.line_width = 2;
  gv.subwindow_mode = IncludeInferiors;
  const unsigned long gv_mask =
      GCForeground | GCBackground | GCFunction | GCLineWidth | GCSubwindowMode;
  menu_gc_ = XCreateGC(dpy_, root_, gv_mask, &gv);

  // The GC used for the close button is the same as for the menu, except it
  // uses GXcopy, not GXxor, so we draw the chosen colour correctly.
  gv.foreground = Resources::I->GetColour(Resources::CLOSE_ICON_COLOUR);
  gv.background = white();
  gv.function = GXcopy;
  gc_ = XCreateGC(dpy_, root_, gv_mask, &gv);
  XSetLineAttributes(dpy, gc_, 2, LineSolid, CapProjecting, JoinMiter);

  gv.foreground =
      Resources::I->GetColour(Resources::INACTIVE_CLOSE_ICON_COLOUR);
  inactive_gc_ = XCreateGC(dpy_, root_, gv_mask, &gv);
  XSetLineAttributes(dpy, inactive_gc_, 2, LineSolid, CapProjecting, JoinMiter);

  // The title bar.
  gv.foreground = Resources::I->GetColour(Resources::TITLE_BG_COLOUR);
  title_gc_ = XCreateGC(dpy_, root_, gv_mask, &gv);

  // Create the popup window, to be used for the menu, and for the little window
  // that shows us how big windows are while resizing them.
  popup_ = XCreateSimpleWindow(
      dpy_, root_, 0, 0, 1, 1, 1,
      Resources::I->GetColour(Resources::POPUP_TEXT_COLOUR),
      Resources::I->GetColour(Resources::POPUP_BACKGROUND_COLOUR));
  XSetWindowAttributes attr;
  attr.event_mask = ButtonMask | ButtonMotionMask | ExposureMask;
  XChangeWindowAttributes(dpy_, popup_, CWEventMask, &attr);

  // Announce our interest in the root_ window.
  attr.cursor = cursor_map_->Root();
  attr.event_mask = SubstructureRedirectMask | SubstructureNotifyMask |
                    ColormapChangeMask | ButtonPressMask | PropertyChangeMask |
                    EnterWindowMask;
  XChangeWindowAttributes(dpy_, root_, CWCursor | CWEventMask, &attr);
  
  // Tell all the applications what icon sizes we prefer.
  ImageIcon::ConfigureIconSizes();
  
  // Make sure all our communication to the server got through.
  XSync(dpy_, false);
  scanWindowTree();
  initEWMH();
}

void LScr::initEWMH() {
  // Announce EWMH compatibility on the screen.
  ewmh_compat_ = XCreateSimpleWindow(dpy_, root_, -200, -200, 1, 1, 0, 0, 0);
  XChangeProperty(dpy_, ewmh_compat_, ewmh_atom[_NET_WM_NAME],
                  utf8_string_atom_, XA_CURSOR, PropModeReplace,
                  (const unsigned char*)"lwm", 3);

  // set root window properties
  XChangeProperty(dpy_, root_, ewmh_atom[_NET_SUPPORTED], XA_ATOM, 32,
                  PropModeReplace, (unsigned char*)ewmh_atom, EWMH_ATOM_LAST);

  XChangeProperty(dpy_, root_, ewmh_atom[_NET_SUPPORTING_WM_CHECK], XA_WINDOW,
                  32, PropModeReplace, (unsigned char*)&ewmh_compat_, 1);

  unsigned long data[4];
  data[0] = 1;
  XChangeProperty(dpy_, root_, ewmh_atom[_NET_NUMBER_OF_DESKTOPS], XA_CARDINAL,
                  32, PropModeReplace, (unsigned char*)data, 1);

  data[0] = width_;
  data[1] = height_;
  XChangeProperty(dpy_, root_, ewmh_atom[_NET_DESKTOP_GEOMETRY], XA_CARDINAL,
                  32, PropModeReplace, (unsigned char*)data, 2);

  data[0] = 0;
  data[1] = 0;
  XChangeProperty(dpy_, root_, ewmh_atom[_NET_DESKTOP_VIEWPORT], XA_CARDINAL,
                  32, PropModeReplace, (unsigned char*)data, 2);

  data[0] = 0;
  XChangeProperty(dpy_, root_, ewmh_atom[_NET_CURRENT_DESKTOP], XA_CARDINAL, 32,
                  PropModeReplace, (unsigned char*)data, 1);

  ewmh_set_strut();
  ewmh_set_client_list();
}

void LScr::scanWindowTree() {
  WindowTree wt = WindowTree::Query(dpy_, root_);
  for (const Window w : wt.children) {
    addClient(w);
  }
  // Tell all the clients to draw their window furniture. We do that now, after
  // we've scanned the window tree, so everything is in its final state.
  for (auto it : clients_) {
    it.second->DrawBorder();
  }
}

Client* LScr::GetOrAddClient(Window w) {
  if (w == Popup()) {
    return nullptr;  // No client for our own popup window.
  }
  Client* c = GetClient(w);
  if (c) {
    return c;
  }
  return addClient(w);
}

Client* LScr::addClient(Window w) {
  XWindowAttributes attr;
  XGetWindowAttributes(dpy_, w, &attr);
  if (attr.override_redirect) {
    return nullptr;
  }
  Client* c = new Client(w, root_);
  c->size.x = attr.x;
  c->size.y = attr.y;
  c->size.width = attr.width;
  c->size.height = attr.height;
  c->border = attr.border_width;
  // map_state is likely already IsViewable if we're being called from
  // scanWindowTree (ie on start-up), but will not be if the window is in the
  // process of being opened (ie we've been called from GetOrAddClient).
  // In the latter case, we don't call manage(c) here, but it'll be called
  // later, when maprequest() calls our Furnish() function.
  if (attr.map_state == IsViewable) {
    c->internal_state = IPendingReparenting;
    manage(c);
  }
  clients_[w] = c;
  return c;
}

void LScr::Furnish(Client* c) {
  c->parent = XCreateSimpleWindow(
      dpy_, root_, c->size.x, c->size.y - textHeight(), c->size.width,
      c->size.height + textHeight(), 1, black(), white());
  XSetWindowAttributes attr;
  attr.event_mask = ExposureMask | EnterWindowMask | ButtonMask |
                    SubstructureRedirectMask | SubstructureNotifyMask |
                    PointerMotionMask;
  XChangeWindowAttributes(dpy_, c->parent, CWEventMask, &attr);

  XResizeWindow(dpy_, c->window, c->size.width - 2 * borderWidth(),
                c->size.height - 2 * borderWidth());
  parents_[c->parent] = c;
}

Client* LScr::GetClient(Window w) const {
  if (w == 0 || w == Root()) {
    return nullptr;
  }
  const auto it = parents_.find(w);
  if (it != parents_.end()) {
    return it->second;
  }
  while (w) {
    const auto it = clients_.find(w);
    if (it != clients_.end()) {
      return it->second;
    }
    w = WindowTree::ParentOf(w);
  }
  return nullptr;
}

void LScr::Remove(Client* c) {
  focuser_.UnfocusClient(c);
  auto it = clients_.find(c->window);
  if (it == clients_.end()) {
    return;
  }
  parents_.erase(it->second->parent);
  clients_.erase(it);
  delete c;
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
static void moveOrChangeSize(int olds, int news, int* pos, int* size, int inc) {
  if (inc < 1) {
    inc = 1;
  }
  // For clarity, comments assume we're talking about the X dimension. This is
  // not necessarily the case, but it's easier to visualise if we pick one
  // dimension.
  int nearDist = *pos;                  // Distance from left edge of screen.
  int farDist = olds - (*pos + *size);  // Distance from right edge of screen.
  bool nearClose = (nearDist * 20 < olds);  // Very close to left edge?
  bool farClose = (farDist * 20 < olds);    // Very close to right edge?

  if (nearClose && farClose) {
    // Window is full width; scale it up or down keeping the left and right
    // edges the same distance from the screen edge.
    *size += ((news - olds) / inc) * inc;
  } else {
    // If we're not scaling the window because it's full-screen, then we need to
    // check its size to ensure it doesn't exceed the new screen size. If we'd
    // be using up more than 90% of the new size, we clip the size to 90%.
    if (news < olds && (*size * 10 / 9) > news) {
      *size = news * 9 / 10;
      *size -= (*size % inc);
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

void LScr::ChangeScreenDimensions(int nScrWidth, int nScrHeight) {
  const int oScrWidth = Width();
  const int oScrHeight = Height();
  // We've found the right screen. Refresh our idea of its size.
  // Note that we don't call DisplayWidth() and DisplayHeight() because they
  // seem to always return the original size, while the change notify event
  // has the updated size.
  if (oScrWidth == nScrWidth && oScrHeight == nScrHeight) {
    return;  // Don't process the same event multiple times.
  }
  width_ = nScrWidth;
  height_ = nScrHeight;
  // Now, go through the windows and adjust their sizes and locations to
  // conform to the new screen layout.
  for (auto it : clients_) {
    // TODO: Consider moving this logic into Client::.
    Client* c = it.second;
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
    XMoveResizeWindow(dpy, c->parent, c->size.x, c->size.y - textHeight(),
                      c->size.width, c->size.height + textHeight());
    if (c->size.width == oldw && c->size.height == oldh) {
      if (c->size.x != oldx || c->size.y != oldy) {
        sendConfigureNotify(c);
      }
    } else {
      XMoveResizeWindow(dpy, c->window, borderWidth(),
                        borderWidth() + textHeight(),
                        c->size.width - 2 * borderWidth(),
                        c->size.height - 2 * borderWidth());
    }
  }
}

bool LScr::ChangeStrut(const EWMHStrut& strut) {
#define SAME(x) strut.x == strut_.x
  if (SAME(left) && SAME(right) && SAME(top) && SAME(bottom)) {
    return false;  // No change.
  }
#undef SAME
  strut_ = strut;
  return true;
}
