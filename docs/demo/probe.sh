#!/usr/bin/env bash
# Coordinate probe: open the View dropdown and capture, to find click targets.
set -uo pipefail
cd /home/ines/src/qt6tst
W=1280; H=748
OBHOME="$(mktemp -d)"
mkdir -p "$OBHOME/.themes" "$OBHOME/.config/openbox"
cp -r docs/demo/openbox/qiftop-dark "$OBHOME/.themes/"
cp docs/demo/openbox/rc.xml "$OBHOME/.config/openbox/rc.xml"

Xvfb :99 -screen 0 ${W}x${H}x24 >/tmp/xvfb.log 2>&1 & XV=$!
sleep 2
env -u XDG_CONFIG_HOME HOME="$OBHOME" DISPLAY=:99 openbox >/tmp/ob.log 2>&1 & WM=$!
sleep 1
env DISPLAY=:99 QT_QPA_PLATFORM=xcb QT_QPA_PLATFORMTHEME=gnome QT_STYLE_OVERRIDE=Adwaita-Dark \
    ./build/qiftop-demo --width $W --height $H >/tmp/demo.log 2>&1 & DEMO=$!
sleep 4

# Move to the candidate View-combo location and click to open the popup.
X="${1:-465}"; Y="${2:-75}"
DISPLAY=:99 xdotool mousemove "$X" "$Y" click 1
sleep 1
DISPLAY=:99 import -window root "/tmp/probe.png" 2>/dev/null

kill -9 "$DEMO" 2>/dev/null || true
kill -9 "$WM" 2>/dev/null || true
kill -9 "$XV" 2>/dev/null || true
rm -rf "$OBHOME"
echo "clicked ($X,$Y); saved /tmp/probe.png"
