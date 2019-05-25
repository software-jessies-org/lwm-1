// tests.cc
// This file contains all unit tests for LWM.
//
// I know, this isn't industry standard practice, putting all the tests in one
// file and compiling them into the main binary, but right now LWM has no tests,
// and I really feel the need to write automated tests for the code that decides
// where to move windows after an xrandr screen change, without too much effort.
//
// Note: some of the macros defined in this file are not 'safe', in that they
// don't protect themselves against being used in if() statements without {}.
// For this reason, always use {}, not one-line if statements.

#include "ewmh.h"
#include "lwm.h"
#include "xlib.h"

#define ARRAY_LEN(x) (sizeof(x) / sizeof(*x))

extern Rect MapToNewAreas(Rect rect,
                          const std::vector<Rect>& oldVis,
                          const std::vector<Rect>& newVis);

static bool failure;

struct mapToNewAreasCase {
  char const* const name;
  char const* const before;
  char const* const oldVis;
  char const* const newVis;
  char const* const want;
};

static const mapToNewAreasCase mapToNewAreasTestCases[] = {
    {"identity", "10x10+10+10", "100x100+0+0", "100x100+0+0", "10x10+10+10"},
    {"wider", "10x10+45+45", "100x100+0+0", "200x100+0+0", "10x10+95+45"},
    {"taller", "10x10+45+45", "100x100+0+0", "100x200+0+0", "10x10+45+95"},
    {"narrower", "10x10+45+45", "100x100+0+0", "50x100+0+0", "10x10+20+45"},
    {"shorter", "10x10+45+45", "100x100+0+0", "100x50+0+0", "10x10+45+20"},
    {"off left", "50x50-5+45", "500x500+0+0", "900x500+0+0", "50x50-5+45"},
    {"off right", "50x50+480+45", "500x500+0+0", "900x500+0+0", "50x50+880+45"},
    {"off top", "50x50+45-5", "500x500+0+0", "500x900+0+0", "50x50+45-5"},
    {"off bottom", "50x50+45+480", "500x500+0+0", "500x900+0+0",
     "50x50+45+880"},
    {"left narrow", "400x50-5+45", "500x500+0+0", "300x700+0+0", "300x50-5+65"},
    {"shrink", "400x500+5+5", "500x700+0+0", "300x230+0+0", "300x230+0+0"},
    {"shrink struts", "400x500+5+5", "500x700+0+0", "300x230+0+30",
     "300x230+0+30"},
    {"left multiscreen", "200x200+0+100", "500x700+0+0",
     "500x700+0+300 800x1000+500+0", "200x200+0+400"},
    {"right multiscreen", "200x200+500+100", "700x500+0+0",
     "700x500+0+300 800x1000+700+0", "200x200+1300+266"},
    {"keep left", "100x400+120+50", "700x500+0+0",
     "700x500+0+500 800x1000+500+0", "100x400+240+550"},
    {"keep right", "100x300+540+50", "700x500+0+0",
     "700x500+0+500 800x1000+500+0", "100x300+1080+175"},
    {"no straddle l", "200x300+220+50", "700x500+0+0",
     "700x500+0+500 700x700+700+0", "200x300+500+550"},
    {"no straddle r", "200x300+280+50", "700x500+0+0",
     "700x500+0+500 700x700+700+0", "200x300+700+100"},
    {"keep left ymax", "100x500+120+0", "700x500+0+0",
     "700x500+0+500 800x1000+700+0", "100x500+280+500"},
    {"keep right ymax", "100x500+540+0", "700x500+0+0",
     "700x500+0+500 800x1000+700+0", "100x1000+1260+0"},
    {"to small l", "200x500+100+500", "700x500+0+500 800x1000+700+0",
     "700x500+0+0", "200x500+38+0"},
    {"to small l wide", "800x500+100+500", "700x500+0+500 800x1000+700+0",
     "700x500+0+0", "700x500+0+0"},
    {"to small r", "200x1000+1000+0", "700x500+0+500 800x1000+700+0",
     "700x500+0+0", "200x500+384+0"},
    {"to small r wide", "800x1000+1000+0", "700x500+0+500 800x1000+700+0",
     "700x500+0+0", "700x500+0+0"},
    {"full size no crash", "500x500+0+0", "500x500+0+0", "900x500+0+0",
     "500x500+0+0"},
    {"full width no crash", "500x300+0+50", "500x500+0+0", "900x500+0+0",
     "500x300+0+50"},
};

static std::vector<Rect> parseRects(const std::string& rects) {
  std::vector<Rect> res;
  for (std::string r : Split(rects, " ")) {
    const Rect rect = Rect::Parse(r);
    if (rect.empty()) {
      return std::vector<Rect>();
    }
    res.push_back(rect);
  }
  return res;
}

static void runMapToNewAreasTests() {
  for (int i = 0; i < ARRAY_LEN(mapToNewAreasTestCases); i++) {
    const auto& tc = mapToNewAreasTestCases[i];
#define FAIL()    \
  failure = true; \
  LOGE() << "FAIL: " << tc.name << ": "

    LOGI() << "Test case: " << tc.name;
    const Rect in = Rect::Parse(tc.before);
    if (in.empty()) {
      FAIL() << "tc.before parse failed (" << tc.before << ")";
    }
    const Rect want = Rect::Parse(tc.want);
    if (want.empty()) {
      FAIL() << "tc.want parse failed (" << tc.want << ")";
    }
    const std::vector<Rect> oldVis = parseRects(tc.oldVis);
    if (oldVis.empty()) {
      FAIL() << "oldVis parse failed (" << tc.oldVis << ")";
    }
    const std::vector<Rect> newVis = parseRects(tc.newVis);
    if (newVis.empty()) {
      FAIL() << "newVis parse failed (" << tc.newVis << ")";
    }
    const Rect got = MapToNewAreas(in, oldVis, newVis);
    if (got != want) {
      FAIL() << "got != want: got " << got << ", want " << want;
    }
#undef FAIL
  }
}

// RunAllTests runs all tests, then returns true on success.
bool RunAllTests() {
  runMapToNewAreasTests();
  if (failure) {
    LOGF() << "FAAAAIIILED!!!";
  } else {
    LOGI() << "Passed";
  }
  return !failure;
}
