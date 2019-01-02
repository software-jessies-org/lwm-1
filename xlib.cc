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

// static
void ImageIcon::ConfigureIconSizes() {
  // XSetIconSize sets the WM_ICON_SIZE property.
  // By requesting anything up to 1024 pixels on a side, we allow the app
  // to provide the largest icon size it's likely to have.
  // This means the app doesn't do any down-scaling for us. For Java apps this
  // is a good move, as while Java is perfectly capable of scaling down images
  // smoothly, it handily forgets this ability and uses the butt-ugly jagged
  // down-sampling method.
  const int min_size = targetImageIconSize();
  const int max_size = 1024;
  XIconSize sz = {min_size, min_size, max_size, max_size, 1, 1};
  XSetIconSizes(dpy, LScr::I->Root(), &sz, 1);
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

ImageIcon::ImageIcon(Pixmap active_img,
                     Pixmap inactive_img,
                     Pixmap menu_img,
                     unsigned int img_w,
                     unsigned int img_h,
                     unsigned int depth)
    : active_img_(active_img),
      inactive_img_(inactive_img),
      menu_img_(menu_img),
      img_w_(img_w),
      img_h_(img_h),
      depth_(depth) {}

// copyWithScaling scales down the contents of src into dest.
// The src image *must* be at least as large as the dest image.
// This function applies some very simple anti-aliasing. It could be improved
// by using sub-pixel accuracy, and weighted averages, but this doesn't seem to
// be necessary, given the results we get from this much simpler approach.
static void copyWithScaling(XImage* src, XImage* dest) {
  for (int y = 0; y < dest->height; y++) {
    const int src_min_y = y * src->height / dest->height;
    const int src_max_y = (y + 1) * src->height / dest->height;  // exclusive
    for (int x = 0; x < dest->width; x++) {
      const int src_min_x = x * src->width / dest->width;
      const int src_max_x = (x + 1) * src->width / dest->width;  // exclusive
      // An unsigned long has plenty of space to the left of even the red
      // component, so we don't bother shifting the components down and up
      // again. However, we must treat each component separately for the
      // averaging, otherwise we'll get bleed between the components due to
      // integer rounding when we divide by the number of pixels.
      unsigned long r = 0;
      unsigned long g = 0;
      unsigned long b = 0;
      for (int sy = src_min_y; sy < src_max_y; sy++) {
        for (int sx = src_min_x; sx < src_max_x; sx++) {
          const unsigned long val = XGetPixel(src, sx, sy);
          r += val & 0xff0000;
          g += val & 0xff00;
          b += val & 0xff;
        }
      }
      // We're never called in a case where dest is larger than src, so the
      // following calculation cannot divide by zero.
      const int div = (src_max_y - src_min_y) * (src_max_x - src_min_x);
      r = (r / div) & 0xff0000;
      g = (g / div) & 0xff00;
      b = (b / div) & 0xff;
      XPutPixel(dest, x, y, r | g | b);
    }
  }
}

static Pixmap pixmapFromXImage(XImage* img) {
  Pixmap pm =
      XCreatePixmap(dpy, LScr::I->Root(), img->width, img->height, img->depth);
  GC igc = XCreateGC(dpy, pm, 0, nullptr);
  XPutImage(dpy, pm, igc, img, 0, 0, 0, 0, img->width, img->height);
  XFreeGC(dpy, igc);
  return pm;
}

static void allocateDataForXImage(XImage* img) {
  img->data = (char*)calloc(img->height, img->bytes_per_line);
}

void xImageDataToImage(XImage* dest,
                       XImage* orig,
                       XImage* mask,
                       unsigned long background) {
  background |= 0xff000000;
  for (int y = 0; y < orig->height; y++) {
    for (int x = 0; x < orig->width; x++) {
      unsigned long rgb = XGetPixel(orig, x, y) | 0xff000000;
      XPutPixel(dest, x, y, XGetPixel(mask, x, y) ? rgb : background);
    }
  }
}

static void pixelDataToImage(XImage* img,
                             unsigned long* data,
                             int width,
                             int height,
                             unsigned long background) {
  const unsigned long bgr = background & 0xff0000;
  const unsigned long bgg = background & 0xff00;
  const unsigned long bgb = background & 0xff;
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      const unsigned long argb = *data++;
      const unsigned long a = (argb >> 24) & 0xff;  // alpha for foreground.
      const unsigned long bga = 0xff - a;           // alpha for background.
      // Treat the 3 channels separately, to avoid cross-channel bleed (which
      // makes the icons of rhythmbox and xfce4-mixer look like CGA vomit).
      unsigned long r = (((argb & 0xff0000) * a + bgr * bga) / 0xff) & 0xff0000;
      unsigned long g = (((argb & 0xff00) * a + bgg * bga) / 0xff) & 0xff00;
      unsigned long b = (((argb & 0xff) * a + bgb * bga) / 0xff) & 0xff;
      XPutPixel(img, x, y, r | g | b);
    }
  }
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
  unsigned int src_width, src_height, borderWidth, depth;
  // Get hold of the width and height of the image.
  XGetGeometry(dpy, img, &ign1, &xr, &yr, &src_width, &src_height, &borderWidth,
               &depth);
  if (depth != 24) {
    // Not going to bother trying to paint stuff that's not colourful enough.
    return nullptr;
  }
  // The image is too large for our needs. Figure out how big to make the
  // width and height dimensions.
  const int targetSize = targetImageIconSize();
  const int width = (src_width < targetSize) ? src_width : targetSize;
  const int height = (src_height < targetSize) ? src_height : targetSize;

  XImage* orig_img =
      XGetImage(dpy, img, 0, 0, src_width, src_height, 0xffffff, ZPixmap);
  XImage* mask_img = nullptr;
  if (mask) {
    mask_img = XGetImage(dpy, mask, 0, 0, src_width, src_height, 1, ZPixmap);
  }

  // src_img will be filled in with the data from orig_img, but with the mask
  // (and background) applied.
  XImage* src_img =
      XCreateImage(dpy, DefaultVisual(dpy, LScr::kOnlyScreenIndex), 24, ZPixmap,
                   0, nullptr, src_width, src_height, 32, 0);
  XImage* dest_img =
      XCreateImage(dpy, DefaultVisual(dpy, LScr::kOnlyScreenIndex), 24, ZPixmap,
                   0, nullptr, width, height, 32, 0);
  if (!src_img || !dest_img) {
    return nullptr;
  }
  allocateDataForXImage(src_img);
  allocateDataForXImage(dest_img);

  // For each possible background, generate the RGB values by applying the
  // image values and background value with the alpha channel.
  xImageDataToImage(src_img, orig_img, mask_img,
                    Resources::I->GetColour(Resources::TITLE_BG_COLOUR));
  copyWithScaling(src_img, dest_img);
  const Pixmap active_pm = pixmapFromXImage(dest_img);

  xImageDataToImage(src_img, orig_img, mask_img, LScr::I->InactiveBorder());
  copyWithScaling(src_img, dest_img);
  const Pixmap inactive_pm = pixmapFromXImage(dest_img);

  xImageDataToImage(src_img, orig_img, mask_img, 0xffffff);
  copyWithScaling(src_img, dest_img);
  const Pixmap menu_pm = pixmapFromXImage(dest_img);

  XDestroyImage(orig_img);
  if (mask_img) {
    XDestroyImage(mask_img);
  }
  XDestroyImage(src_img);
  XDestroyImage(dest_img);

  result = new ImageIcon(active_pm, inactive_pm, menu_pm, width, height, 24);
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

  XImage* src_img =
      XCreateImage(dpy, DefaultVisual(dpy, LScr::kOnlyScreenIndex), 24, ZPixmap,
                   0, nullptr, src_width, src_height, 32, 0);
  XImage* dest_img =
      XCreateImage(dpy, DefaultVisual(dpy, LScr::kOnlyScreenIndex), 24, ZPixmap,
                   0, nullptr, width, height, 32, 0);
  if (!src_img || !dest_img) {
    return nullptr;
  }
  allocateDataForXImage(src_img);
  allocateDataForXImage(dest_img);

  // For each possible background, generate the RGB values by applying the
  // image values and background value with the alpha channel.
  pixelDataToImage(src_img, data + 2, src_width, src_height,
                   Resources::I->GetColour(Resources::TITLE_BG_COLOUR));
  copyWithScaling(src_img, dest_img);
  const Pixmap active_pm = pixmapFromXImage(dest_img);

  pixelDataToImage(src_img, data + 2, src_width, src_height,
                   LScr::I->InactiveBorder());
  copyWithScaling(src_img, dest_img);
  const Pixmap inactive_pm = pixmapFromXImage(dest_img);

  pixelDataToImage(src_img, data + 2, src_width, src_height, 0xffffff);
  copyWithScaling(src_img, dest_img);
  const Pixmap menu_pm = pixmapFromXImage(dest_img);

  XDestroyImage(src_img);
  XDestroyImage(dest_img);
  result = new ImageIcon(active_pm, inactive_pm, menu_pm, width, height, 24);
  toCache(pm_hash, result);
  return result->clone();
}

void ImageIcon::paint(Window w,
                      Pixmap pm,
                      int x,
                      int y,
                      int width,
                      int height) {
  if (!pm) {
    return;
  }
  const int xo = (width - (int)img_w_) / 2;
  const int yo = (height - (int)img_h_) / 2;
  XGCValues gv;
  gv.function = GXcopy;
  GC gc = XCreateGC(dpy, w, GCFunction, &gv);
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
  XCopyArea(dpy, pm, w, gc, src_x, src_y, width, height, x, y);
  XFreeGC(dpy, gc);
}
