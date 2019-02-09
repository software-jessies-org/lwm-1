#include <X11/Xlib.h>
#include <X11/Xos.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>

#include <stdio.h>
#include <stdlib.h>

#include "lwm.h"

bool isLeftEdge(Edge e) {
  return e == ETopLeft || e == ELeft || e == EBottomLeft;
}

bool isRightEdge(Edge e) {
  return e == ETopRight || e == ERight || e == EBottomRight;
}

bool isTopEdge(Edge e) {
  return e == ETopLeft || e == ETop || e == ETopRight;
}

bool isBottomEdge(Edge e) {
  return e == EBottomLeft || e == EBottom || e == EBottomRight;
}

std::ostream& operator<<(std::ostream& os, const Rect& r) {
  os << "Rect[" << r.xMin << "," << r.yMin << ";" << r.xMax << "," << r.yMax
     << "]";
  return os;
}

// static
Point Point::Sub(Point a, Point b) {
  return Point{a.x - b.x, a.y - b.y};
}

// static
Rect Rect::Intersect(const Rect& a, const Rect& b) {
  Rect res = Rect{std::max(a.xMin, b.xMin), std::max(a.yMin, b.yMin),
                  std::min(a.xMax, b.xMax), std::min(a.yMax, b.yMax)};
  if (res.xMax > res.xMin && res.yMax > res.yMin) {
    return res;
  }
  // No intersection.
  res = Rect{0, 0, 0, 0};
  return res;
}

// static
Rect Rect::Translate(Rect r, Point p) {
  return Rect{r.xMin + p.x, r.yMin + p.y, r.xMax + p.x, r.yMax + p.y};
}
