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

  inline bool operator==(const Point& o) const { return x == o.x && y == o.y; }
  inline bool operator!=(const Point& o) const { return !operator==(o); }

  // Returns b - a.
  static Point Sub(Point a, Point b);
};

struct Area {
  int width;
  int height;

  inline bool operator==(const Area& o) const {
    return width == o.width && height == o.height;
  }
  inline bool operator!=(const Area& o) const { return !operator==(o); }

  int num_pixels() const { return width * height; }
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
  Area area() const { return Area{width(), height()}; }
  bool empty() const { return area().num_pixels() == 0; }

  Point origin() const { return Point{xMin, yMin}; }
  Point middle() const { return Point{(xMin + xMax) / 2, (yMin + yMax) / 2}; }

  inline bool operator==(const Rect& o) const {
    return xMin == o.xMin && yMin == o.yMin && xMax == o.xMax && yMax == o.yMax;
  }

  inline bool operator!=(const Rect& o) const { return !operator==(o); }

  static Rect FromXYWH(int x, int y, int w, int h);

  template <typename T>
  static Rect From(const T& t) {
    return FromXYWH(t.x, t.y, t.width, t.height);
  }

  template <typename T>
  void To(T& t) {
    t.x = xMin;
    t.y = yMin;
    t.width = width();
    t.height = height();
  }

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
std::ostream& operator<<(std::ostream& os, const Area& a);
std::ostream& operator<<(std::ostream& os, const std::vector<Rect>& rs);

class DimensionLimiter {
 public:
  DimensionLimiter() : DimensionLimiter(0, 0, 0, 1) {}
  DimensionLimiter(int min, int max, int base, int increment);

  // Given an old and new range, the new range being passed by reference,
  // adjusts the new range according to the limits. If one of the new min/max
  // values must be adjusted, Limit picks the one that has changed from its
  // old value, and fixes that one.
  void Limit(int oldMin, int oldMax, int& newMin, int& newMax) const;

  // Returns the size that should be displayed to the user. This takes into
  // account any size increments and base sizes. For example, if the window has
  // no limits or increments set, this just returns v. If, however, this is
  // something like an xterm, which has size increments equal to the character
  // size, and maybe a base equal to the size of the scrollbar, then the value
  // returned is the number of increments above the base, and thus the number
  // of characters. Thus, we end up showing "80 x 24", for example, for a
  // normal-sized xterm.
  int DisplayableSize(int v) const;

 private:
  int min_;
  int max_;
  int base_;
  int increment_;
};

#endif
