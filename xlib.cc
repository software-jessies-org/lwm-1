#include "lwm.h"

WindowTree WindowTree::Query(Display* dpy, Window w) {
  WindowTree res = {};
  Window* ch = NULL;
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

// Returns the parent window of w, or NULL if we hit the root or on error.
Window WindowTree::ParentOf(Window w) {
  WindowTree wt = WindowTree::Query(LScr::I->Dpy(), w);
  return (wt.parent == wt.root) ? 0 : wt.parent;
}

ImageIcon::ImageIcon(Pixmap img,
                     Pixmap mask,
                     unsigned int img_w,
                     unsigned int img_h,
                     unsigned int depth)
    : img_(img), mask_(mask), img_w_(img_w), img_h_(img_h), depth_(depth) {}

ImageIcon* ImageIcon::Create(Pixmap img, Pixmap mask) {
  if (!img) {
    return nullptr;
  }
  Window ign1;
  int xr, yr;
  unsigned int w, h, borderWidth, depth;
  // Get hold of the width and height of the image.
  XGetGeometry(dpy, img, &ign1, &xr, &yr, &w, &h, &borderWidth, &depth);
  if (depth != 24) {
    // Not going to bother trying to paint stuff that's not colourful enough.
    return nullptr;
  }
  return new ImageIcon(img, mask, w, h, depth);
}

void ImageIcon::Paint(Window w, int x, int y, int width, int height) {
  if (depth_ != 1 && depth_ != 24) {
    return;
  }
  const int xo = (width - (int)img_w_) / 2;
  const int yo = (height - (int)img_h_) / 2;
  XGCValues gv;
  gv.function = GXcopy;
  unsigned long vmask = GCFunction;
  if (mask_) {
    gv.clip_mask = mask_;
    gv.clip_x_origin = x + xo;
    gv.clip_y_origin = y + yo;
    vmask |= GCClipMask | GCClipXOrigin | GCClipYOrigin;
  }
  GC gc = XCreateGC(dpy, w, vmask, &gv);
  // If the pixmap is smaller than what we want to draw, adjust the coordinates
  // so we draw within the bounding box.
  if (xo > 0) {
    x += xo;
    width = img_w_;
  }
  if (yo > 0) {
    y += yo;
    height = img_h_;
  }
  // If the pixmap is bigger than what we want to draw, adjust the src
  // coordinates to draw something in the middle of the source pixmap.
  const int src_x = (xo < 0) ? -xo : 0;
  const int src_y = (yo < 0) ? -yo : 0;
  if (depth_ == 1) {
    XCopyPlane(dpy, img_, w, gc, src_x, src_y, width, height, x, y, 1);
  } else {
    XCopyArea(dpy, img_, w, gc, src_x, src_y, width, height, x, y);
  }
  XFreeGC(dpy, gc);
}
