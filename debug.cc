#include "ewmh.h"
#include "lwm.h"
#include "xlib.h"

// String manipulation functions.
// Yes, these are crude and inefficient, and involve a lot of string copying.
// However, this is a command-line debug interface, so I don't care.

// This is basically strtok in C++. Returns the bit of victim up to the next
// space, or end, removing the same part from victim. Yes, it modifies its
// argument. Do I care? No.
std::string nextToken(std::string& victim) {
  const size_t sep = victim.find(' ');
  std::string res = victim.substr(0, sep);
  if (sep == std::string::npos) {
    victim = "";
  } else {
    victim = victim.substr(sep);
  }
  while (!victim.empty() && victim[0] == ' ') {
    victim = victim.substr(1);
  }
  return res;
}

Rect fullScreenRect() {
  return Rect{0, 0, DisplayWidth(dpy, 0), DisplayHeight(dpy, 0)};
}

unsigned long deadColour() {
  XColor colour, exact;
  XAllocNamedColor(dpy, DefaultColormap(dpy, LScr::kOnlyScreenIndex), "grey",
                   &colour, &exact);
  return colour.pixel;
}

void DebugCLI::cmdXRandr(std::string line) {
  if (line == "?") {
    LOGI() << "With struts:    " << LScr::I->VisibleAreas(true);
    LOGI() << "Without struts: " << LScr::I->VisibleAreas(false);
    return;
  }
  std::vector<Rect> rects;
  while (!line.empty()) {
    const std::string tok = nextToken(line);
    Rect r = Rect::Parse(tok);
    if (r.empty()) {
      LOGE() << "Failed to parse rect '" << tok << "'";
    } else {
      rects.push_back(r);
    }
  }
  if (rects.empty()) {
    rects.push_back(fullScreenRect());
  }
  LOGI() << "Setting visible areas to " << rects;
  resetDeadZones(rects);
  LScr::I->SetVisibleAreas(rects);
}

void DebugCLI::resetDeadZones(const std::vector<Rect>& visible) {
  std::vector<Rect> dead(1, fullScreenRect());
  for (const Rect& vis : visible) {
    std::vector<Rect> new_dead;
    for (const Rect& d : dead) {
      const Rect i = Rect::Intersect(vis, d);
      if (i.empty()) {
        new_dead.push_back(d);
        continue;
      }
      // There's definitely an intersection.
      // Full-width rect above the visible area.
      if (i.yMin > d.yMin) {
        new_dead.push_back(Rect{d.xMin, d.yMin, d.xMax, i.yMin});
      }
      // Full-width rect below the visible area.
      if (i.yMax < d.yMax) {
        new_dead.push_back(Rect{d.xMin, i.yMax, d.xMax, d.yMax});
      }
      // Left of the visible area.
      if (i.xMin > d.xMin) {
        new_dead.push_back(Rect{d.xMin, i.yMin, i.xMin, i.yMax});
      }
      // Right of the visible area.
      if (i.xMax < d.xMax) {
        new_dead.push_back(Rect{i.xMax, i.yMin, d.xMax, i.yMax});
      }
    }
    dead = new_dead;
  }
  for (Window w : dead_zones_) {
    XDestroyWindow(dpy, w);
  }
  dead_zones_.clear();

  const unsigned long dead_colour = deadColour();
  for (const Rect& r : dead) {
    const Window w =
        XCreateSimpleWindow(dpy, LScr::I->Root(), r.xMin, r.yMin, r.width(),
                            r.height(), 0, dead_colour, dead_colour);
    XMapRaised(dpy, w);
    dead_zones_.push_back(w);
  }
  LOGI() << "Inaccessible areas are: " << dead;
}

void DebugCLI::Read() {
  char buf[1024];
  ssize_t bytes = read(STDIN_FILENO, buf, sizeof(buf));
  if (bytes == sizeof(buf)) {
    LOGE() << "A whole " << bytes << " bytes on one line? You're crazy.";
    return;
  }
  std::string line;
  for (int i = 0; i < sizeof(buf) && buf[i] != '\n'; i++) {
    line.push_back(buf[i]);
  }
  const std::string& cmd = nextToken(line);
  if (cmd == "xrandr") {
    cmdXRandr(line);
  } else {
    LOGI() << "Didn't understand command '" << cmd << "'";
  }
}
