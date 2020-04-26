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
      strut_{0, 0, 0, 0} {
  visible_areas_ = std::vector<Rect>(1, Rect{0, 0, width_, height_});
}

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

  // Create the popup window, to be used for the resize feedback window,
  // and the menu window.
  XSetWindowAttributes attr;
  attr.event_mask = ButtonMask | ButtonMotionMask | ExposureMask;
  const unsigned int fg = Resources::I->GetColour(Resources::POPUP_TEXT_COLOUR);
  const unsigned int bg =
      Resources::I->GetColour(Resources::POPUP_BACKGROUND_COLOUR);
  Rect r{0, 0, 1, 1};
  popup_ = xlib::CreateNamedWindow("LWM size popup", r, 1, fg, bg);
  XChangeWindowAttributes(dpy_, popup_, CWEventMask, &attr);
  menu_ = xlib::CreateNamedWindow("LWM unhide menu", r, 1, fg, bg);
  XChangeWindowAttributes(dpy_, menu_, CWEventMask, &attr);

  // Announce our interest in the root_ window.
  attr.cursor = cursor_map_->Root();
  attr.event_mask = SubstructureRedirectMask | SubstructureNotifyMask |
                    ColormapChangeMask | ButtonPressMask | PropertyChangeMask |
                    EnterWindowMask;
  XChangeWindowAttributes(dpy_, root_, CWCursor | CWEventMask, &attr);

  // Tell all the applications what icon sizes we prefer.
  xlib::ImageIcon::ConfigureIconSizes();

  // Make sure all our communication to the server got through.
  XSync(dpy_, false);
  ScanWindowTree();
  InitEWMH();
}

void LScr::InitEWMH() {
  // Announce EWMH compatibility on the screen.
  Rect r{-200, -200, 1, 1};
  ewmh_compat_ = xlib::CreateNamedWindow("LWM EWMH", r, 0, 0, 0);
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

void LScr::ScanWindowTree() {
  xlib::WindowTree wt = xlib::WindowTree::Query(dpy_, root_);
  for (const Window w : wt.children) {
    AddClient(w);
  }
  // Tell all the clients they don't have input focus. This has two effects:
  // 1: the client will respond by drawing its border (always)
  // 2: if we're in click-to-focus mode, the client will grab input events, so
  //    that it can detect clicks within the window being managed.
  // We do that now, after we've scanned the window tree, so everything is in
  // its final state.
  for (auto it : clients_) {
    it.second->FocusLost();
  }
}

Client* LScr::GetOrAddClient(Window w) {
  if (w == Popup() || w == Menu()) {
    return nullptr;  // No client for our own popup windows.
  }
  Client* c = GetClient(w);
  if (c) {
    return c;
  }
  c = AddClient(w);
  DebugCLI::NotifyClientAdd(c);
  return c;
}

Client* LScr::AddClient(Window w) {
  XWindowAttributes attr;
  XGetWindowAttributes(dpy_, w, &attr);
  if (attr.override_redirect) {
    return nullptr;
  }
  Client* c = new Client(w, root_);
  // LOGI() << "New client " << attr.width << "x" << attr.height << "+" <<
  // attr.x
  //       << "+" << attr.y << ", g = " << attr.win_gravity;
  c->size.x = attr.x;
  c->size.y = attr.y;
  c->size.width = attr.width;
  c->size.height = attr.height;
  c->border = attr.border_width;
  // map_state is likely already IsViewable if we're being called from
  // ScanWindowTree (ie on start-up), but will not be if the window is in the
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
  Rect r = Rect::FromXYWH(c->size.x, c->size.y - textHeight(), c->size.width,
                          c->size.height + textHeight());
  std::ostringstream name;
  name << "LWM frame for " << WinID(c->window);
  c->parent = xlib::CreateNamedWindow(name.str(), r, 1, black(), white());
  XSetWindowAttributes attr;
  // DO NOT SET PointerMotionHintMask! Doing so allows X to send just one
  // notification to the window until the key or button state changes. This
  // prevents us from properly updating the cursor as we move the pointer around
  // our window furniture.
  attr.event_mask = ExposureMask | EnterWindowMask | LeaveWindowMask |
                    ButtonMask | SubstructureRedirectMask |
                    SubstructureNotifyMask | PointerMotionMask;
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
    w = xlib::WindowTree::ParentOf(w);
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
  DebugCLI::NotifyClientRemove(c);
  delete c;
}

Rect LScr::GetPrimaryVisibleArea(bool withStruts) const {
  Rect res{0, 0, 0, 0};
  for (const Rect& r : LScr::I->VisibleAreas(withStruts)) {
    if (r.area() > res.area()) {
      res = r;
    } else if (r.area() == res.area()) {
      // Two screens of the same size: we pick the one furthest up, and furthest
      // to the left.
      if (r.yMin < res.yMin) {
        res = r;
      } else if (r.yMin == res.yMin) {
        if (r.xMin < res.xMin) {
          res = r;
        }
      }
    }
  }
  return res;
}

std::vector<Rect> areasMinusStruts(std::vector<Rect> in, EWMHStrut strut) {
  // First, derive the width and height, and subtract the struts from them.
  int xMax = 0;
  int yMax = 0;
  for (const Rect& r : in) {
    if (r.xMax > xMax) {
      xMax = r.xMax;
    }
    if (r.yMax > yMax) {
      yMax = r.yMax;
    }
  }
  // Find [xy]M(in|ax), representing the total bounding box we have to clip all
  // our screens to.
  xMax -= strut.right;
  yMax -= strut.bottom;
  const int xMin = strut.left;
  const int yMin = strut.top;
  std::vector<Rect> res;
  for (const Rect& r : in) {
    res.push_back(Rect{std::max(xMin, r.xMin), std::max(yMin, r.yMin),
                       std::min(xMax, r.xMax), std::min(yMax, r.yMax)});
  }
  return res;
}

std::vector<Rect> LScr::VisibleAreas(bool withStruts) const {
  if (!withStruts) {
    return visible_areas_;
  }
  return areasMinusStruts(visible_areas_, strut_);
}

struct moveData {
  Client* c;
  int x;
  int y;
  bool sendConfigNotify;
  bool sizeChanged;
};

int quantise(int dimension, int increment) {
  if (increment < 2) {
    return dimension;
  }
  return dimension - (dimension % increment);
}

int xMaxFrom(const std::vector<Rect>& rs) {
  int res = 0;
  for (const Rect& r : rs) {
    res = std::max(res, r.xMax);
  }
  return res;
}

Rect mirrorX(const Rect& r, int xMax) {
  return Rect{xMax - r.xMax, r.yMin, xMax - r.xMin, r.yMax};
}

std::vector<Rect> mirrorAllX(const std::vector<Rect>& rs, int xMax) {
  std::vector<Rect> res;
  for (const Rect& r : rs) {
    res.push_back(mirrorX(r, xMax));
  }
  return res;
}

Rect flipXY(const Rect& r) {
  return Rect{r.yMin, r.xMin, r.yMax, r.xMax};
}

std::vector<Rect> flipAllXY(const std::vector<Rect>& rs) {
  std::vector<Rect> res;
  for (const Rect& r : rs) {
    res.push_back(flipXY(r));
  }
  return res;
}

Rect mapLeftEdge(Rect rect,
                 const std::vector<Rect>& oldVis,
                 const std::vector<Rect>& newVis) {
  Rect oldLE;
  int oldLEOverlap = 0;
  // Find the old monitor the window overlaps the most.
  for (const Rect& r : oldVis) {
    if (r.xMin != 0) {
      continue;
    }
    const int h = Rect::Intersect(rect, r).height();
    if (h > oldLEOverlap) {
      oldLEOverlap = h;
      oldLE = r;
    }
  }
  // Find the first screen at x=0, and try to position it at roughly the same
  // height.
  for (const Rect& r : newVis) {
    if (r.xMin != 0) {
      continue;
    }
    Rect res = rect;
    // If the window was Y maximised, or its height is larger than that of the
    // new screen at this position, then maximise it.
    if (oldLEOverlap == oldLE.height() || rect.height() >= r.height()) {
      res.yMin = r.yMin;
      res.yMax = r.yMax;
    } else {
      // Window fits within the screen - ensure its Y position is relatively
      // similar to what it was.
      int yoff = rect.yMin - oldLE.yMin;
      if (yoff <= 0) {
        yoff = 0;
      } else {
        yoff = yoff * (r.height() - rect.height()) /
               (oldLE.height() - rect.height());
      }
      res.yMin = r.yMin + yoff;
      res.yMax = res.yMin + rect.height();
    }
    // Ensure the window isn't wider than the screen.
    if (res.width() > r.width()) {
      res.xMax = res.xMin + r.width();
      // ...but that means we have to ensure we don't lose it, if its xMin is
      // more than the width of the screen off the left.
      const int fixOffset = 10 - res.xMax;
      if (fixOffset > 0) {
        res.xMin += fixOffset;
        res.xMax += fixOffset;
      }
    }
    return res;
    break;
  }
  // Didn't manage to figure anything out - return same thing.
  return rect;
}

bool mapEdges(Rect rect,
              const std::vector<Rect>& oldVis,
              const std::vector<Rect>& newVis,
              Rect* newRect) {
  // If the window abuts the left edge of the screen, or extends beyond it,
  // keep it there, but ensure that its Y span is within the new vertical area
  // of the left edge, and that it's no wider than that screen.
  if (rect.xMin <= 0) {
    *newRect = mapLeftEdge(rect, oldVis, newVis);
    return true;
  }
  // Same for the right edge. Because this code is a bit complicated, we
  // implement this by mirroring the before/after screens horizontally, then
  // calling mapLeftEdge, before mirroring back the resulting rect.
  const int oldXMax = xMaxFrom(oldVis);
  const int newXMax = xMaxFrom(newVis);
  if (rect.xMax >= oldXMax) {
    Rect res = mapLeftEdge(mirrorX(rect, oldXMax), mirrorAllX(oldVis, oldXMax),
                           mirrorAllX(newVis, newXMax));
    *newRect = mirrorX(res, newXMax);
    return true;
  }
  return false;
}

Rect tallestScreenAtX(const std::vector<Rect>& vis, int x) {
  int maxHeight = 0;
  Rect res = Rect{0, 0, 0, 0};
  Rect rightmost = Rect{0, 0, 0, 0};
  for (const Rect& r : vis) {
    if (r.xMax > rightmost.xMax) {
      rightmost = r;
    }
    if (x < r.xMin || x > r.xMax) {
      continue;
    }
    if (r.height() > maxHeight) {
      maxHeight = r.height();
      res = r;
    }
  }
  return res.empty() ? rightmost : res;
}

Rect sourceScreen(const std::vector<Rect>& vis, const Rect& r) {
  int area = 0;
  Rect res;
  for (const Rect& scr : vis) {
    const int ra = Rect::Intersect(scr, r).area();
    if (ra > area) {
      area = ra;
      res = scr;
    }
  }
  if (area) {
    return res;
  }
  // No overlap found anywhere; fall back to...
  return tallestScreenAtX(vis, r.xMin);
}

Rect forceWithinRectX(Rect r, Rect target) {
  Rect res = r;
  if (res.width() >= target.width()) {
    res.xMin = target.xMin;
    res.xMax = target.xMax;
    return res;
  }
  if (res.xMin < target.xMin) {
    res.xMin = target.xMin;
  } else if (res.xMin > target.xMax - r.width()) {
    res.xMin = target.xMax - r.width();
  }
  res.xMax = res.xMin + r.width();
  return res;
}

int scale(int pos, int size, int oldMax, int newMax) {
  if (oldMax <= size || newMax <= size) {
    return pos;
  }
  return pos * (newMax - size) / (oldMax - size);
}

int maybeScaleDown(int v, int oldMax, int newMax) {
  return (newMax >= oldMax) ? v : (v * newMax / oldMax);
}

Rect MapToNewAreas(Rect rect,
                   const std::vector<Rect>& oldVis,
                   const std::vector<Rect>& newVis) {
  Rect res;
  const int oldXMax = xMaxFrom(oldVis);
  const int newXMax = xMaxFrom(newVis);
  // If this window is height-maximised on any of the original visible areas,
  // we deal with it specially.
  for (const Rect& r : oldVis) {
    if (Rect::Intersect(r, rect).height() == r.height()) {
      res.xMin = scale(rect.xMin, rect.width(), oldXMax, newXMax);
      // Find the highest screen at the middle X location, and use that as a
      // target.
      const int scaledWidth = maybeScaleDown(rect.width(), oldXMax, newXMax);
      const Rect target = tallestScreenAtX(newVis, res.xMin + scaledWidth / 2);
      res.xMax = res.xMin + rect.width();
      res = forceWithinRectX(res, target);
      res.yMin = target.yMin;
      res.yMax = target.yMax;
      return res;
    }
  }
  // Deal with windows that are up against the X or Y extremes of the
  // old visible area. To save on code, which do this by using mirroring and
  // flipping, and just implement code for the x=0 edge.
  if (mapEdges(rect, oldVis, newVis, &res)) {
    return res;
  }
  if (mapEdges(flipXY(rect), flipAllXY(oldVis), flipAllXY(newVis), &res)) {
    return flipXY(res);
  }
  // If we got here, the window is floating about within some screen.
  // One thing we can be reasonably certain of is that if the window was
  // maximised in either dimension, it will have been taken care of by one of
  // the mapEdges calls. So we can happily ignore that.
  // Possibly the simplest approach now is to:
  // Map the X position according to old vs new X extents.
  res.xMin = scale(rect.xMin, rect.width(), oldXMax, newXMax);
  // Find the highest screen at the middle X location, and use that as a target.
  const int scaledWidth = maybeScaleDown(rect.width(), oldXMax, newXMax);
  const Rect target = tallestScreenAtX(newVis, res.xMin + scaledWidth / 2);
  // If the window is too wide to fit in the screen, force it down to occupy
  // the whole screen area.
  if (rect.width() >= target.width()) {
    res.xMin = target.xMin;
    res.xMax = target.xMax;
  } else {
    // Window is small enough to fit in the target window. However, we want
    // to ensure it's entirely within one monitor, so adjust accordingly.
    res.xMax = res.xMin + rect.width();
    res = forceWithinRectX(res, target);
  }
  // At this point, res has xMin and xMax set correctly. Now deal with the Y
  // coordinate.
  // Again, if the window is too tall to fit in the monitor, force its y
  // coordinates.
  if (rect.height() > target.height()) {
    res.yMin = target.yMin;
    res.yMax = target.yMax;
  } else {
    // Find the screen that the old rect mostly intersected (or just pick one at
    // its x position).
    const Rect source = sourceScreen(oldVis, rect);
    // If the window was off the top or bottom, push it entirely within the
    // target.
    if (rect.yMin < source.yMin) {
      res.yMin = target.yMin;
    } else if (rect.yMax >= source.yMax) {
      res.yMin = target.yMax - rect.height();
    } else {
      // Window was entirely within the Y scope of the source. Scale it so it
      // occupies the same kind of Y position in the target.
      res.yMin = target.yMin + (rect.yMin - source.yMin) *
                                   (target.height() - rect.height()) /
                                   (source.height() - rect.height());
    }
    res.yMax = res.yMin + rect.height();
  }
  return res;
}

// How to do this:
// Find the old visible area containing the window.
// Scale window centre to new display w/h, and map that to new visible area.
// Sort out new position/size according to mapping from old to new visible area.
// Once all the internal sizes are updated, and we have a list of actions to
// take, switch in the new visible_areas_, and then send all the size change/
// configure notify requests.
void LScr::SetVisibleAreas(std::vector<Rect> visible_areas) {
  int nScrWidth = 0;
  int nScrHeight = 0;
  for (const Rect& r : visible_areas_) {
    if (r.xMax > nScrWidth) {
      nScrWidth = r.xMax;
    }
    if (r.yMax > nScrHeight) {
      nScrHeight = r.yMax;
    }
  }

  const std::vector<Rect> oldVis = areasMinusStruts(visible_areas_, strut_);
  const std::vector<Rect> newVis = areasMinusStruts(visible_areas, strut_);

  std::vector<moveData> moves;

  // Now, go through the windows and adjust their sizes and locations to
  // conform to the new screen layout.
  for (auto it : clients_) {
    Client* c = it.second;
    // Ignore clients that set struts; we expect these to watch for screen
    // changes for themselves, and move their windows if necessary.
    // Of course, if we were to move them, we'd want to be using the strutless
    // visible areas, not the ones with the struts removed, otherwise we'd
    // reposition strutty windows so they don't interesect their own struts,
    // which is wrong.
    if (c->HasStruts()) {
      // If this client has set a strut, it's reserved an area of the screen for
      // it to place its own window in. As such, we must avoid forcing that
      // window into the visible area with struts excluded, as doing so would
      // prevent the client from placing its window in its own reserved area.
      // A better approach may be to use the visible areas *without* the struts
      // removed in order to potential force strutted windows into the visible
      // area of the screen. However, as they're reserving a window edge
      // already, they probably should be listening for xrandr events and moving
      // their windows appropriately, in which case there's nothing for us to
      // do here.
      continue;
    }

    Rect newRect = MapToNewAreas(c->RectWithBorder(), oldVis, newVis);

    // Now we have newRect, which describes where we'd like to put the window,
    // including its frame. Translate that down to the client window
    // coordinates (if the client is framed).
    if (c->framed) {
      newRect.yMin += textHeight();
    }

    const int oldx = c->size.x;
    const int oldy = c->size.y;
    const int oldw = c->size.width;
    const int oldh = c->size.height;

    // Set the new window sizes, paying attention to the client's wishes on
    // size increments.
    c->size.x = newRect.xMin;
    c->size.y = newRect.yMin;
    c->size.width = quantise(newRect.width(), c->size.width_inc);
    c->size.height = quantise(newRect.height(), c->size.height_inc);

    const bool sizeChanged = c->size.width != oldw || c->size.height != oldh;
    const bool posChanged = c->size.x != oldx || c->size.y != oldy;
    const bool sendConfigNotify = posChanged && !sizeChanged;
    moves.push_back(
        moveData{c, c->size.x, c->size.y, sendConfigNotify, sizeChanged});
  }

  // Now we've determined what we need to do with the windows, we should put the
  // new screen geometry in place so that Client_MakeSane and friends can use
  // it.
  visible_areas_ = visible_areas;
  width_ = nScrWidth;
  height_ = nScrHeight;

  // All set up now, let's move all the windows around.
  for (moveData& move : moves) {
    Client* c = move.c;
    Client_MakeSane(c, ENone, move.x, move.y, 0, 0);
    xlib::XMoveResizeWindow(c->parent, c->size.x, c->size.y - textHeight(),
                            c->size.width, c->size.height + textHeight());
    if (move.sendConfigNotify) {
      c->SendConfigureNotify();
    }
    if (move.sizeChanged) {
      xlib::XMoveResizeWindow(c->window, borderWidth(),
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
