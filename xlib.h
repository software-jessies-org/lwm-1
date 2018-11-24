#ifndef LWM_XLIB_H_included
#define LWM_XLIB_H_included

#include <stdio.h>
#include <stdlib.h>

#include <string>
#include <vector>

#include <X11/SM/SMlib.h>
#include <X11/X.h>
#include <X11/Xatom.h>
#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <X11/Xos.h>
#include <X11/Xproto.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/extensions/Xrandr.h>

struct WindowTree {
  Window self;
  Window parent;
  Window root;
  std::vector<Window> children;
  unsigned int num_children;
  
  // Query returns the set of children of the given window.
  static WindowTree Query(Display *dpy, Window w);
  
  // Parent returns the parent window of w, or 0 if the parent is the root
  // window.
  static Window ParentOf(Window w);
};

#endif // LWM_XLIB_H_included
