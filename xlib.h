#ifndef XLIB_H_included
#define XLIB_H_included

#include <vector>

#include <X11/X.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xos.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>

struct WindowTree {
  Window self;
  Window parent;
  Window root;
  std::vector<Window> children;
  unsigned int num_children;
  
  static WindowTree Query(Display *dpy, Window w);
};

#endif // XLIB_H_included
