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
  popup_ = XCreateSimpleWindow(dpy_, root_, 0, 0, 1, 1, 1, fg, bg);
  XChangeWindowAttributes(dpy_, popup_, CWEventMask, &attr);
  menu_ = XCreateSimpleWindow(dpy_, root_, 0, 0, 1, 1, 1, fg, bg);
  XChangeWindowAttributes(dpy_, menu_, CWEventMask, &attr);
  
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
  c = addClient(w);
  DebugCLI::NotifyClientAdd(c);
  return c;
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

static int quantise(int dimension, int increment) {
  if (increment < 2) {
    return dimension;
  }
  return dimension - (dimension % increment);
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

  const int oScrWidth = Width();
  const int oScrHeight = Height();
  // Now, go through the windows and adjust their sizes and locations to
  // conform to the new screen layout.
  for (auto it : clients_) {
    Client* c = it.second;
    Rect rect = c->RectWithBorder();
    // Find the visible area in the old setup which has the largest overlap with
    // this rect. We use this to determine whether the window is maximised in
    // either direction.
    bool maxX = false;
    bool maxY = false;
    // The wasTotallyIn variables are set if the window was entirely visible
    // before this screen change. This is used to force the window to still be
    // entirely visible in the same dimension after the change, but we don't
    // want to force that if the user deliberately had the window dangling off
    // the side.
    bool wasTotallyInX = false;
    bool wasTotallyInY = false;
    Rect biggestOverlap = {};
    for (const Rect& r : oldVis) {
      Rect overlap = Rect::Intersect(rect, r);
      maxX |= overlap.width() == r.width();
      maxY |= overlap.height() == r.height();
      wasTotallyInX |= overlap.width() == rect.width();
      wasTotallyInY |= overlap.height() == rect.height();
      if (overlap.area() > biggestOverlap.area()) {
        biggestOverlap = overlap;
      }
    }
    // Map the old centre of the window into the new screen coordinates.
    Point newPos = rect.middle();
    newPos.x = newPos.x * nScrWidth / oScrWidth;
    newPos.y = newPos.y * nScrHeight / oScrHeight;
    // Shift the bounds so its centre moves to newPos.
    Rect newRect = Rect::Translate(rect, Point::Sub(newPos, rect.middle()));
    // Try to find a new screen which intersects most with newRect.
    Rect bestScreen = {};
    int bestArea = 0;
    for (const Rect& r : newVis) {
      const int area = Rect::Intersect(newRect, r).area();
      if (area > bestArea) {
        bestArea = area;
        bestScreen = r;
      }
    }
    // If we found no intersections, try to find the closest.
    if (bestArea == 0) {
      int bestDistance = INT_MAX;
      for (const Rect& r : newVis) {
        int xd = 0;
        if (newRect.xMax < r.xMin) {
          xd = r.xMin - newRect.xMax;
        } else if (r.xMax < newRect.xMin) {
          xd = newRect.xMin - r.xMax;
        }
        int yd = 0;
        if (newRect.yMax < r.yMin) {
          yd = r.yMin - newRect.yMax;
        } else if (r.yMax < newRect.yMin) {
          yd = newRect.yMin - r.yMax;
        }
        const int dist = std::max(xd, yd);
        if (dist < bestDistance) {
          bestDistance = dist;
          bestScreen = r;
        }
      }
    }
    // Now figure out the rectangle we want to end up with. Start off with
    // newRect, which is the same size as the old rect, but has been shifted to
    // an equivalent position in the new desktop bounds.
    // Use all the width if we were X-maximised, or larger than this screen.
    if (maxX || newRect.width() >= bestScreen.width()) {
      newRect.xMin = bestScreen.xMin;
      newRect.xMax = bestScreen.xMax;
    } else if (wasTotallyInX) {  // Keep entirely inside the rectangle.
      if (newRect.xMax > bestScreen.xMax) {
        newRect =
            Rect::Translate(newRect, Point{bestScreen.xMax - newRect.xMax, 0});
      } else if (newRect.xMin < bestScreen.xMin) {
        newRect =
            Rect::Translate(newRect, Point{bestScreen.xMin - newRect.xMin, 0});
      }
    } else {  // Keep the same amount of window width visible as before.
      const int ol = biggestOverlap.width();
      if (newRect.xMin > bestScreen.xMax - ol) {
        newRect = Rect::Translate(
            newRect, Point{bestScreen.xMax - ol - newRect.xMin, 0});
      } else if (newRect.xMax < bestScreen.xMin + ol) {
        newRect = Rect::Translate(
            newRect, Point{bestScreen.xMin + ol - newRect.xMax, 0});
      }
    }
    // Now do all the same things, but for the Y direction.
    if (maxY || newRect.height() >= bestScreen.height()) {
      newRect.yMin = bestScreen.yMin;
      newRect.yMax = bestScreen.yMax;
    } else if (wasTotallyInY) {  // Keep entirely inside the rectangle.
      if (newRect.yMax > bestScreen.yMax) {
        newRect =
            Rect::Translate(newRect, Point{0, bestScreen.yMax - newRect.yMax});
      } else if (newRect.yMin < bestScreen.yMin) {
        newRect =
            Rect::Translate(newRect, Point{0, bestScreen.yMin - newRect.yMin});
      }
    } else {  // Keep the same amount of window height visible as before.
      const int ol = biggestOverlap.height();
      if (newRect.yMin > bestScreen.yMax - ol) {
        newRect = Rect::Translate(
            newRect, Point{0, bestScreen.yMax - ol - newRect.yMin});
      } else if (newRect.yMax < bestScreen.yMin + ol) {
        newRect = Rect::Translate(
            newRect, Point{0, bestScreen.yMin + ol - newRect.yMax});
      }
    }

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
    XMoveResizeWindow(dpy, c->parent, c->size.x, c->size.y - textHeight(),
                      c->size.width, c->size.height + textHeight());
    if (move.sendConfigNotify) {
      sendConfigureNotify(c);
    }
    if (move.sizeChanged) {
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
