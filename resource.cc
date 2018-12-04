/*
 * lwm, a window manager for X11
 * Copyright (C) 1997-2016 Elliott Hughes, James Carter
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "lwm.h"

void Resources::Init() {
  I = new Resources();
}

Resources* Resources::I;

Resources::Resources() {
  strings_.resize(S_END);
  ints_.resize(I_END);

  XrmDatabase db = nullptr;
  char* resource_manager = XResourceManagerString(dpy);
  if (resource_manager) {
    XrmInitialize();
    db = XrmGetStringDatabase(resource_manager);
  }
  // Font used in title bars, and indeed everywhere we have fonts.
  set(TITLE_FONT, db, "titleFont", "Font", "roboto-16");
  // Command to execute when button 1 (left) is clicked on root window.
  set(BUTTON1_COMMAND, db, "button1", "Command", "");
  // Command to execute when button 2 (middle) is clicked on root window.
  set(BUTTON2_COMMAND, db, "button2", "Command", "xterm");
  // Background colour for title bar of the active window.
  set(TITLE_BG_COLOUR, db, "titleBGColour", "String", "#A0522D");
  // Border background colour of the active window.
  set(BORDER_COLOUR, db, "borderColour", "String", "#B87058");
  // Border and title background colour of inactive windows.
  set(INACTIVE_BORDER_COLOUR, db, "inactiveBorderColour", "String", "#785840");
  // Colour of the window highlight box displayed when the popup (unhide)
  // menu is open and the pointer is hovering over an entry in that menu, and
  // which shows the display bounds of the corresponding window.
  set(WINDOW_HIGHLIGHT_COLOUR, db, "windowHighlightColour", "String", "red");
  
  // The width of the border LWM adds to each window to allow resizing.
  set(BORDER_WIDTH, db, "border", "Border", 6);
}

const std::string& Resources::Get(SR sr) {
  if (sr < S_BEGIN || sr >= S_END) {
    return strings_[S_BEGIN];  // Will be empty string, because we never init
                               // it.
  }
  return strings_[sr];
}

// Retrieve an int resource.
int Resources::GetInt(IR ir) {
  if (ir < I_BEGIN || ir >= I_END) {
    return 0;
  }
  return ints_[ir];
}

static bool tryGet(XrmDatabase db,
                   const std::string& name,
                   const char* cls,
                   std::string* tgt) {
  if (!db) {
    return false;
  }
  char* type;
  XrmValue value;
  const std::string fullName = std::string("lwm.") + name;
  if (XrmGetResource(db, fullName.c_str(), cls, &type, &value)) {
    if (!strcmp(type, "String")) {
      *tgt = std::string(value.addr, value.size);
      return true;
    }
  }
  return false;
}

void Resources::set(SR res,
                    XrmDatabase db,
                    const std::string& name,
                    const char* cls,
                    const std::string& dflt) {
  if (!tryGet(db, name, cls, &(strings_[res]))) {
    strings_[res] = dflt;
  }
}

void Resources::set(IR res,
                    XrmDatabase db,
                    const std::string& name,
                    const char* cls,
                    int dflt) {
  ints_[res] = dflt;
  std::string strVal;
  if (!tryGet(db, name, cls, &strVal)) {
    return;
  }
  if (strVal == "") {
    return;
  }
  const int val = (int)strtol(strVal.c_str(), (char**)0, 0);
  // We assume 0 means failure; if 0 is a valid value, its default should be
  // 0 to allow default to be set.
  ints_[res] = val ? val : dflt;
}

// Border width is used a lot, so let's make it easily accessible.
int borderWidth() {
  return Resources::I->GetInt(Resources::BORDER_WIDTH);
}
