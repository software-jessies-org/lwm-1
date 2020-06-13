#!/bin/bash
#
# Usage:
#  force_title.sh <window id>
#
# This program runs zenity to request a new window title, and on success, runs
# setvisname to set the visible name property on the window id provided.

die() {
  echo "$1"
  exit 1
}

if [ "$1" = "" ]; then
  die "Usage: force_title.sh <window id>"
fi

which zenity || die "zenity is not installed"
title=$(zenity --entry --width=600)
if [ ! "$?" = "0" ]; then
  echo "User canceled"
  exit 0
fi

setvisname "$1" "$title"
