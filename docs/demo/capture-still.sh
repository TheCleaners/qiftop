#!/usr/bin/env bash
# Headless still-frame capture of qiftop-demo. Self-contained cleanup.
set -uo pipefail
cd /home/ines/src/qt6tst

STYLE="${1:-FUSION}"   # FUSION (built-in dark) or a QT_STYLE_OVERRIDE name
OUT="${2:-/tmp/demo-frame.png}"
W=1280; H=748

# Stage a private HOME for Openbox with our dark theme + maximize rule, so it
# doesn't touch the real user's WM config.
OBHOME="$(mktemp -d)"
mkdir -p "$OBHOME/.themes" "$OBHOME/.config/openbox"
cp -r docs/demo/openbox/qiftop-dark "$OBHOME/.themes/"
cp docs/demo/openbox/rc.xml "$OBHOME/.config/openbox/rc.xml"

Xvfb :99 -screen 0 ${W}x${H}x24 >/tmp/xvfb.log 2>&1 &
XV=$!
sleep 2

# Dark-themed WM so the window gets a title bar that blends with the app.
# NOTE: openbox reads rc.xml from XDG_CONFIG_HOME (not HOME); unset it so it
# defaults to $OBHOME/.config and actually picks up our qiftop-dark theme.
env -u XDG_CONFIG_HOME HOME="$OBHOME" DISPLAY=:99 openbox >/tmp/ob.log 2>&1 &
WM=$!
sleep 1

DARKFLAG=""
ENVSTYLE=()
if [[ "$STYLE" == "FUSION" ]]; then
    DARKFLAG="--dark"
else
    # gnome platform theme supplies themed (symbolic, palette-recoloured)
    # toolbar icons; the Adwaita-Dark STYLE forces the dark palette on top
    # (qgnomeplatform alone can't find a dark colour-scheme headless). Both
    # are needed: drop either and you lose icons or lose dark.
    ENVSTYLE=(QT_QPA_PLATFORMTHEME=gnome "QT_STYLE_OVERRIDE=$STYLE")
fi

env DISPLAY=:99 QT_QPA_PLATFORM=xcb "${ENVSTYLE[@]}" \
    ./build/qiftop-demo --width $W --height $H $DARKFLAG >/tmp/demo.log 2>&1 &
DEMO=$!
sleep 4

DISPLAY=:99 import -window root "$OUT" 2>/tmp/import.log \
    || ffmpeg -y -f x11grab -video_size ${W}x${H} -i :99 -frames:v 1 "$OUT" >/tmp/ff.log 2>&1

kill -9 "$DEMO" 2>/dev/null || true
kill -9 "$WM"   2>/dev/null || true
kill -9 "$XV"   2>/dev/null || true
rm -rf "$OBHOME"
echo "demo.log:"; head -6 /tmp/demo.log
ls -la "$OUT" 2>/dev/null && file "$OUT"

