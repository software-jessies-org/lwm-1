#ifndef XLIB_H_included
#define XLIB_H_included

#include <X11/X.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xos.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>

typedef struct {
  Window self;
  Window parent;
  Window root;
  Window *children;
  unsigned int num_children;
} WindowTree;

// Calls XQueryTree, and returns the values as a struct, all properly
// initialised. Call FreeQueryWindow to clean up the result safely.
extern WindowTree QueryWindow(Display *dpy, Window w);
extern void FreeQueryWindow(WindowTree *wt);

#endif // XLIB_H_included
