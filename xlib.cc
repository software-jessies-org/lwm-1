extern "C" {
#include "xlib.h"
}

WindowTree QueryWindow(Display *dpy, Window w) {
  WindowTree res;
  memset(&res, 0, sizeof(res));
  // It doesn't matter which root window we give this call.
  XQueryTree(dpy, w, &res.root, &res.parent, &res.children, &res.num_children);
  if (res.parent) {
    res.self = w;
  }
  return res;
}

void FreeQueryWindow(WindowTree *wt) {
  if (wt->children) {
    XFree(wt->children);
  }
}
