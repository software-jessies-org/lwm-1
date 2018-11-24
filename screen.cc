#include "lwm.h"
#include "xlib.h"

// The static LScr instance.
LScr* LScr::I;

LScr::LScr(Display* dpy)
    : dpy_(dpy), cursor_map_(new CursorMap(dpy)), strut_{0, 0, 0, 0} {
  // Allocate grey colour, to be used for unfocused windows.
  XColor colour, exact;
  XAllocNamedColor(dpy_, DefaultColormap(dpy_, kOnlyScreenIndex), "DimGray",
                   &colour, &exact);
  grey_ = colour.pixel;

  // Generate a graphics context for all our drawing needs.
  XGCValues gv;
  gv.foreground = Black() ^ White();
  gv.background = White();
  gv.function = GXxor;
  gv.line_width = 2;
  gv.subwindow_mode = IncludeInferiors;
  const Window root = Root();
  gc_ = XCreateGC(
      dpy_, root,
      GCForeground | GCBackground | GCFunction | GCLineWidth | GCSubwindowMode,
      &gv);

  // Create the popup window, to be used for the menu, and for the little window
  // that shows us how big windows are while resizing them.
  popup_ = XCreateSimpleWindow(dpy_, root, 0, 0, 1, 1, 1, Black(), White());
  XSetWindowAttributes attr;
  attr.event_mask = ButtonMask | ButtonMotionMask | ExposureMask;
  XChangeWindowAttributes(dpy_, popup_, CWEventMask, &attr);

  // Announce our interest in the root window.
  attr.cursor = cursor_map_->Root();
  attr.event_mask = SubstructureRedirectMask | SubstructureNotifyMask |
                    ColormapChangeMask | ButtonPressMask | PropertyChangeMask |
                    EnterWindowMask;
  XChangeWindowAttributes(dpy_, root, CWCursor | CWEventMask, &attr);

  // Make sure all our communication to the server got through.
  XSync(dpy_, false);
  ScanWindowTree();
}

void LScr::ScanWindowTree() {
  std::map<Window, Client*> new_clients;
  const Window root = Root();
  WindowTree wt = WindowTree::Query(dpy_, root);
  for (const Window win : wt.children) {
    XWindowAttributes attr;
    XGetWindowAttributes(dpy_, win, &attr);
    if (attr.override_redirect || win == screen->popup) {
      continue;
    }
    // If we already have this client, move it into the new map.
    if (clients_[win]) {
      new_clients[win] = clients_[win];
      clients_.erase(win);
      continue;
    }
    // We haven't seen this client before, so create and add to the new map.
    Client* c = new Client(win, root);
    new_clients[win] = c;
    c->size.x = attr.x;
    c->size.y = attr.y;
    c->size.width = attr.width;
    c->size.height = attr.height;
    c->border = attr.border_width;
    if (attr.map_state == IsViewable) {
      c->internal_state = IPendingReparenting;
      manage(c);
    }
  }
  // Tidy up all the old clients which no longer exist, and flip clients_ to
  // contain the new contents.
  for (auto it : clients_) {
    delete it.second;
  }
  clients_.swap(new_clients);
}

Client *LScr::GetClient(Window w) const {
  if (w == 0 || w == Root()) return nullptr;
  while (w) {
    const auto it = clients_.find(w);
    if (it != clients_.end()) return it->second;
    w = WindowTree::ParentOf(w);
  }
  return nullptr;
}

void LScr::Remove(Client *c) {
  auto it = clients_.find(c->window);
  if (it == clients_.end()) return;
  clients_.erase(it);
  delete c;
}
