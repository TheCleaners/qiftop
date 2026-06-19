#!/usr/bin/env bash
# Headless animated capture of qiftop-demo -> optimized GIF.
#
# Drives the synthetic GUI with a REAL mouse cursor (xdotool): opens the View
# dropdown and switches grouping (Flat -> By Container -> By Process -> Flat),
# then opens the Settings dialog and tours its side-nav (incl. the Colours
# page). The throughput gauges animate throughout. Recorded under Xvfb with a
# dark Adwaita-Qt body + an orange-accented Openbox title bar, then converted
# to a palette-optimized GIF. Self-contained cleanup.
set -uo pipefail
cd /home/ines/src/qt6tst

OUT="${1:-/tmp/qiftop-demo.gif}"
STYLE="${2:-Adwaita-Dark}"    # QT_STYLE_OVERRIDE name, or FUSION (built-in dark)
SECS="${3:-26}"               # capture duration (must cover the driver below)
W=1280; H=748
FPS=12
SCALE=1000                    # output width (px); height auto
RAW=/tmp/qiftop-demo-raw.mp4

OBHOME="$(mktemp -d)"
mkdir -p "$OBHOME/.themes" "$OBHOME/.config/openbox"
cp -r docs/demo/openbox/qiftop-dark "$OBHOME/.themes/"
cp docs/demo/openbox/rc.xml "$OBHOME/.config/openbox/rc.xml"

Xvfb :99 -screen 0 ${W}x${H}x24 >/tmp/xvfb.log 2>&1 &
XV=$!
sleep 2
# openbox reads rc.xml from XDG_CONFIG_HOME (not HOME) — unset it so it picks
# up our qiftop-dark theme from $OBHOME/.config.
env -u XDG_CONFIG_HOME HOME="$OBHOME" DISPLAY=:99 openbox >/tmp/ob.log 2>&1 &
WM=$!
sleep 1

DARKFLAG=""; ENVSTYLE=()
if [[ "$STYLE" == "FUSION" ]]; then
    DARKFLAG="--dark"
else
    # gnome platform theme = themed toolbar icons; Adwaita-Dark STYLE = dark
    # palette. Both needed (see capture-still.sh for the rationale).
    ENVSTYLE=(QT_QPA_PLATFORMTHEME=gnome "QT_STYLE_OVERRIDE=$STYLE")
fi
# No --script: the xdotool driver below performs the tour through the real UI.
env DISPLAY=:99 QT_QPA_PLATFORM=xcb "${ENVSTYLE[@]}" \
    ./build/qiftop-demo --width $W --height $H $DARKFLAG >/tmp/demo.log 2>&1 &
DEMO=$!
sleep 3   # let data populate before recording starts

# --- Scripted UI tour (runs concurrently with the recording) ----------------
# Coordinates are stable because Openbox maximizes the main window at (0,0).
# Combo-item selection is done by KEYBOARD (Down/Home + Return) because Qt
# repositions the popup based on the current item, so item pixel coords drift.
drive() {
    export DISPLAY=:99
    local VIEW="385 75" PREFS="113 75" PARK="640 430"
    sleep 1.5
    # Flat -> By Container
    xdotool mousemove $VIEW; sleep 0.4; xdotool click 1; sleep 1.3
    xdotool key Down Down Return; sleep 2.6
    # By Container -> By Process
    xdotool mousemove $VIEW; sleep 0.3; xdotool click 1; sleep 1.1
    xdotool key Down Return; sleep 2.6
    # By Process -> Flat
    xdotool mousemove $VIEW; sleep 0.3; xdotool click 1; sleep 1.0
    xdotool key Home Return; sleep 1.6
    # Open Preferences (gear), then tour the side-nav by KEYBOARD so we don't
    # depend on exact row pixels (the list gains/loses pages over time — a
    # stale Y coordinate is how the Colours page silently stopped being shown).
    # Click the top row once to focus + select "Monitoring", then arrow down:
    # Monitoring -> Display (1) -> ... -> Colors (4 downs total).
    xdotool mousemove $PREFS; sleep 0.5; xdotool click 1; sleep 1.6
    xdotool mousemove 297 56; sleep 0.4; xdotool click 1; sleep 1.0   # focus list (Monitoring)
    xdotool key Down; sleep 1.8                                       # Display
    xdotool key Down Down Down; sleep 2.8                             # -> Colors (theme + swatches)
    xdotool key Escape; sleep 1.0                                     # close the dialog (robust)
    xdotool mousemove $PARK; sleep 0.8
}
drive >/tmp/drive.log 2>&1 &
DRIVER=$!

# Record the X root (with the cursor) for SECS seconds.
ffmpeg -y -f x11grab -draw_mouse 1 -video_size ${W}x${H} -framerate $FPS \
    -i :99 -t "$SECS" -pix_fmt yuv420p "$RAW" >/tmp/ffrec.log 2>&1

# Two-pass palette for clean GIF colours, then gifsicle for size.
PAL=/tmp/qiftop-pal.png
ffmpeg -y -i "$RAW" -vf "fps=${FPS},scale=${SCALE}:-1:flags=lanczos,palettegen=stats_mode=diff" \
    "$PAL" >/tmp/ffpal.log 2>&1
ffmpeg -y -i "$RAW" -i "$PAL" \
    -lavfi "fps=${FPS},scale=${SCALE}:-1:flags=lanczos[x];[x][1:v]paletteuse=dither=bayer:bayer_scale=3:diff_mode=rectangle" \
    /tmp/qiftop-demo-unopt.gif >/tmp/ffgif.log 2>&1
gifsicle -O3 --lossy=60 /tmp/qiftop-demo-unopt.gif -o "$OUT" 2>/tmp/gifsicle.log || \
    cp /tmp/qiftop-demo-unopt.gif "$OUT"

kill -9 "$DRIVER" 2>/dev/null || true
kill -9 "$DEMO"   2>/dev/null || true
kill -9 "$WM"     2>/dev/null || true
kill -9 "$XV"     2>/dev/null || true
rm -rf "$OBHOME"

echo "demo.log:"; head -4 /tmp/demo.log
echo "drive.log tail:"; tail -3 /tmp/drive.log 2>/dev/null
ls -la "$OUT" && file "$OUT"
