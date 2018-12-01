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
  static WindowTree Query(Display* dpy, Window w);

  // Parent returns the parent window of w, or 0 if the parent is the root
  // window.
  static Window ParentOf(Window w);
};

// ImageIcon holds and image, and optionally a mask, for painting an icon on
// the screen. It is used to draw application icons in the unhide menu, and in
// the title bar of windows that have them.
// Given a specific box to draw into, this will draw the image in the middle
// of the box, or if the icon is larger than the box it clips the image so that
// the image's middle is visible inside the given box.
// Images are not scaled.
class ImageIcon {
 public:
  // Create either creates an ImageIcon capable of drawing the icon on a 24bit
  // display, or returns null.
  static ImageIcon* Create(Pixmap img, Pixmap mask);

  // Paints this image on the given window, centred within the box given by
  // x, y, w, h.
  void Paint(Window w, int x, int y, int width, int height);

 private:
  ImageIcon(Pixmap img,
            Pixmap mask,
            unsigned int img_w,
            unsigned int img_h,
            unsigned int depth);

  Pixmap img_;
  Pixmap mask_;
  unsigned int img_w_ = 0;
  unsigned int img_h_ = 0;
  unsigned int depth_ = 0;
};

#endif  // LWM_XLIB_H_included
