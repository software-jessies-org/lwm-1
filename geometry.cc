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

bool isTopEdge(Edge e) { return e == ETopLeft || e == ETop || e == ETopRight; }

bool isBottomEdge(Edge e) {
  return e == EBottomLeft || e == EBottom || e == EBottomRight;
}
