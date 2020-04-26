#ifndef GEOMETRY_H_included
#define GEOMETRY_H_included

#include <iostream>
#include <string>
#include <vector>

/**
Window edge, used in resizing. The `edge' ENone is used to signify a
window move rather than a resize. The code is sufficiently similar that
this isn't a special case to be treated separately.
*/
enum Edge {
  ETopLeft,
  ETop,
  ETopRight,
  ERight,
  ENone,
  ELeft,
  EBottomLeft,
  EBottom,
  EBottomRight,
  EClose,     // Special 'Edge' to denote the close icon.
  EContents,  // Special again: not any action, it's the client window.
  E_LAST
};

struct Point {
  int x;
  int y;

  // Returns b - a.
  static Point Sub(Point a, Point b);
};

struct Rect {
  int xMin;
  int yMin;
  int xMax;
  int yMax;

  bool contains(int x, int y) const {
    return x >= xMin && y >= yMin && x < xMax && y < yMax;
  }

  int width() const { return xMax - xMin; }
  int height() const { return yMax - yMin; }
  int area() const { return width() * height(); }
  bool empty() const { return area() == 0; }

  Point middle() const { return Point{(xMin + xMax) / 2, (yMin + yMax) / 2}; }

  inline bool operator==(const Rect& o) const {
    return xMin == o.xMin && yMin == o.yMin && xMax == o.xMax && yMax == o.yMax;
  }

  inline bool operator!=(const Rect& o) const { return !operator==(o); }

  static Rect FromXYWH(int x, int y, int w, int h);

  // Returns a new Rect which is shifted by the given x and y translation.
  static Rect Translate(Rect r, Point p);

  // Returns the intersection of the two rectangles or, if they don't intersect,
  // the empty rectangle 0,0,0,0.
  static Rect Intersect(const Rect& a, const Rect& b);

  // Parse rectangles in X11 style (1280x960+23+25).
  // Returns the canonical empty rectangle if parsing fails.
  static Rect Parse(std::string str);
};

std::ostream& operator<<(std::ostream& os, const Point& p);
std::ostream& operator<<(std::ostream& os, const Rect& r);
std::ostream& operator<<(std::ostream& os, const std::vector<Rect>& rs);

#endif
