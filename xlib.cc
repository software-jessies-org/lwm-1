#include "lwm.h"

WindowTree WindowTree::Query(Display* dpy, Window w) {
  WindowTree res = {};
  Window* ch = nullptr;
  unsigned int num_ch = 0;
  // It doesn't matter which root window we give this call.
  XQueryTree(dpy, w, &res.root, &res.parent, &ch, &num_ch);
  XFreer ch_freer(ch);
  if (res.parent) {
    res.self = w;
  }
  res.children.reserve(num_ch);
  for (int i = 0; i < num_ch; i++) {
    res.children.push_back(ch[i]);
  }
  return res;
}

// Returns the parent window of w, or NULL if we hit the root or on error.
Window WindowTree::ParentOf(Window w) {
  WindowTree wt = WindowTree::Query(LScr::I->Dpy(), w);
  return (wt.parent == wt.root) ? 0 : wt.parent;
}

// targetImageIconSize returns the max size we want to use for window icons.
// This is determined by the minimum of the space available in the two places
// we display icons, which are the title bar of the window, and the 'unhide'
// menu.
static int targetImageIconSize() {
  const int mh = menuItemHeight();
  const int th = titleBarHeight();
  return (mh < th) ? mh : th;
}

static std::map<unsigned long, ImageIcon*>* image_icon_cache;

static ImageIcon* fromCache(unsigned long hash) {
  if (!image_icon_cache) {
    return nullptr;
  }
  auto it = image_icon_cache->find(hash);
  if (it == image_icon_cache->end()) {
    return nullptr;
  }
  return it->second;
}

static void toCache(unsigned long hash, ImageIcon* icon) {
  if (!image_icon_cache) {
    image_icon_cache = new std::map<unsigned long, ImageIcon*>;
  }
  (*image_icon_cache)[hash] = icon;
}

static unsigned long hashData(unsigned long* data, unsigned long len) {
  // For simplicity, coerce the data into a string, and then hash it.
  std::string s((char*)data, len * sizeof(unsigned long));
  std::hash<std::string> h;
  return h(s);
}

static unsigned long hashPixmaps(Pixmap img, Pixmap mask) {
  // Assuming the same image and mask are used together (which is probably a
  // safe assumption), we can just use the img as the hash.
  mask = mask;
  return (unsigned long)img;
}

ImageIcon::ImageIcon(Pixmap img,
                     Pixmap mask,
                     unsigned int img_w,
                     unsigned int img_h,
                     unsigned int depth)
    : img_(img), mask_(mask), img_w_(img_w), img_h_(img_h), depth_(depth) {}

static void copyWithScaling(XImage* src, XImage* dest) {
  // Special handling for 1bpp images (masks): when the mask is plotted into
  // an XImage using XGetImage, it gets inverted. Therefore, to create a
  // properly-functioning mask, we need to invert it back, which we do here at
  // scaling time because we're looping over pixels anyway.
  const bool invert = dest->depth == 1;
  for (int y = 0; y < dest->height; y++) {
    int src_y = y * src->height / dest->height;
    for (int x = 0; x < dest->width; x++) {
      int src_x = x * src->width / dest->width;
      unsigned long val = XGetPixel(src, src_x, src_y);
      if (invert) {
        val = !val;
      }
      XPutPixel(dest, x, y, val);
    }
  }
}

static Pixmap pixmapFromXImage(XImage* img) {
  Pixmap pm =
      XCreatePixmap(dpy, LScr::I->Root(), img->width, img->height, img->depth);
  GC igc = XCreateGC(dpy, pm, 0, nullptr);
  XPutImage(dpy, pm, igc, img, 0, 0, 0, 0, img->width, img->height);
  XDestroyImage(img);
  XFreeGC(dpy, igc);
  return pm;
}

static void allocateDataForXImage(XImage* img) {
  img->data = (char*)calloc(img->height, img->bytes_per_line);
}

// static
ImageIcon* ImageIcon::Create(Pixmap img, Pixmap mask) {
  if (!img) {
    return nullptr;
  }
  const unsigned long pm_hash = hashPixmaps(img, mask);
  ImageIcon* result = fromCache(pm_hash);
  if (result) {
    return result->clone();
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
  const int targetSize = targetImageIconSize();
  if (h <= targetSize) {
    // Image needs no scaling, and can be returned directly.
    result = new ImageIcon(img, mask, w, h, depth);
    toCache(pm_hash, result);
    return result->clone();
  }
  
  // The image is too large for our needs. Figure out how big to make the
  // width and height dimensions.
  const int width = (w < targetSize) ? w : targetSize;
  const int height = (h < targetSize) ? h : targetSize;
  
  // Fetch the pixmap from the XServer and render it into an XImage, so we can
  // manipulate it.
  XImage* src_img = XGetImage(dpy, img, 0, 0, w, h, 0xffffff, ZPixmap);
  // Create a destination image of the right size.
  XImage* dest_img =
      XCreateImage(dpy, DefaultVisual(dpy, LScr::kOnlyScreenIndex), 24, ZPixmap,
                   0, nullptr, width, height, 32, 0);
  if (!dest_img) {
    return nullptr;
  }
  // Fill in the image with data sampled from the src_image, and then turn it
  // back into a pixmap.
  allocateDataForXImage(dest_img);
  copyWithScaling(src_img, dest_img);
  img = pixmapFromXImage(dest_img);
  
  // If there is a mask specified, do all the above steps we did for the main
  // image data, but for the mask as well.
  if (mask) {
    XImage* src_mask = XGetImage(dpy, mask, 0, 0, w, h, 1, ZPixmap);
    XImage* dest_mask =
        XCreateImage(dpy, DefaultVisual(dpy, LScr::kOnlyScreenIndex), 1,
                     XYBitmap, 0, nullptr, width, height, 32, 0);
    allocateDataForXImage(dest_mask);
    if (!dest_mask) {
      return nullptr;
    }
    copyWithScaling(src_mask, dest_mask);
    mask = pixmapFromXImage(dest_mask);
  }
  result = new ImageIcon(img, mask, width, height, 24);
  toCache(pm_hash, result);
  return result->clone();
}

// Use Google Chrome or Chromium to test CreateFromPixels.
// static
ImageIcon* ImageIcon::CreateFromPixels(unsigned long* data, unsigned long len) {
  if (data == nullptr || len < 2) {
    return nullptr;
  }
  const unsigned long pm_hash = hashData(data, len);
  ImageIcon* result = fromCache(pm_hash);
  if (result) {
    return result->clone();
  }

  const int src_width = data[0];
  const int src_height = data[1];
  if (src_width == 0 || src_height == 0 || len < (2 + src_width * src_height)) {
    fprintf(stderr, "Invalid width (%d) vs height (%d) vs size (%d)\n",
            src_width, src_height, int(len));
    return nullptr;
  }
  // Calculate the destination size of the icon, either the same as source, or
  // the desired size if that's smaller.
  // This code assumes the icon is square (if not, it will be distorted).
  // But it's almost guaranteed to be so.
  const int targetSize = targetImageIconSize();
  const int width = (src_width < targetSize) ? src_width : targetSize;
  const int height = (src_height < targetSize) ? src_height : targetSize;

  XImage* img = XCreateImage(dpy, DefaultVisual(dpy, LScr::kOnlyScreenIndex),
                             24, ZPixmap, 0, nullptr, width, height, 32, 0);
  if (!img) {
    return nullptr;
  }
  XImage* mask = XCreateImage(dpy, DefaultVisual(dpy, LScr::kOnlyScreenIndex),
                              1, XYBitmap, 0, nullptr, width, height, 32, 0);
  if (!mask) {
    XDestroyImage(img);
    return nullptr;
  }
  allocateDataForXImage(img);
  allocateDataForXImage(mask);

  for (int y = 0; y < height; y++) {
    // Scale the x, y coordinates into the image we're creating so they map
    // to the subsampled space of the source image.
    unsigned long* src_line = data + 2 + src_width * (y * src_height / height);
    for (int x = 0; x < width; x++) {
      unsigned long argb = src_line[(x * src_width) / width];
      unsigned long rgb = argb & 0xffffff;
      XPutPixel(img, x, y, rgb);
      // MSB of alpha is in bit 31, so take its inverse as the 1 bit mask.
      XPutPixel(mask, x, y, !(argb & (1 << 31)));
    }
  }
  result = new ImageIcon(pixmapFromXImage(img), pixmapFromXImage(mask), width,
                         height, 24);
  toCache(pm_hash, result);
  return result->clone();
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
