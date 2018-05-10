#include "xlib.h"

WindowTree WindowTree::Query(Display *dpy, Window w) {
  WindowTree res = {};
  Window *ch = NULL;
  unsigned int num_ch = 0;
  // It doesn't matter which root window we give this call.
  XQueryTree(dpy, w, &res.root, &res.parent, &ch, &num_ch);
  if (res.parent) {
    res.self = w;
  }
  res.children.reserve(num_ch);
  for (int i = 0; i < num_ch; i++) {
    res.children.push_back(ch[i]);
  }
  if (ch) {
    XFree(ch);
  }
  return res;
}
