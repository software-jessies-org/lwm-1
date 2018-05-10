#include <X11/Xlib.h>
#include <X11/Xos.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>

#include <stdio.h>
#include <stdlib.h>

#include "lwm.h"

Bool isLeftEdge(Edge e) {
  return e == ETopLeft || e == ELeft || e == EBottomLeft;
}

Bool isRightEdge(Edge e) {
  return e == ETopRight || e == ERight || e == EBottomRight;
}

Bool isTopEdge(Edge e) { return e == ETopLeft || e == ETop || e == ETopRight; }

Bool isBottomEdge(Edge e) {
  return e == EBottomLeft || e == EBottom || e == EBottomRight;
}
