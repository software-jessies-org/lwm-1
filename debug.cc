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

void cmdXRandr(std::string line) {
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
    rects.push_back(Rect{0, 0, DisplayWidth(dpy, 0), DisplayHeight(dpy, 0)});
  }
  LOGI() << "Setting visible areas to " << rects;
  LScr::I->SetVisibleAreas(rects);
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
