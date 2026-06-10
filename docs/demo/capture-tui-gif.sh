#!/usr/bin/env bash
# Headless animated capture of nqiftop-demo (the ncurses front-end) -> GIF.
#
# Runs the synthetic, privacy-safe nqiftop-demo inside an xterm under Xvfb and
# records the window with ffmpeg, then palette-optimises to a GIF. The demo
# drives its OWN scripted key tour internally (TuiApp::handleKey on a timer),
# so no xdotool keystroke timing is needed — ffmpeg just films the terminal.
#
# Uses an Xft TrueType face (DejaVu Sans Mono) so the box-drawing, arrow and
# block glyphs (→ ▸ ▾ ▼ ├ └ █ Σ ↓ ↑) render — a bitmap -fn font shows tofu.
# The xterm's real mapped geometry is read back with xdotool and handed to
# ffmpeg, so the crop is exact regardless of font metrics.
#
# Every interface, address, PID and container shown is fabricated (RFC 5737 /
# RFC 3849 doc ranges); XDG_CONFIG_HOME is sandboxed to a tmp dir so the run
# neither reads nor writes the user's real nqiftop settings. Self-contained
# cleanup. Build the target first:
#   cmake -B build -DQIFTOP_BUILD_DEMO=ON && cmake --build build --target nqiftop-demo
set -uo pipefail
cd /home/ines/src/qt6tst

OUT="${1:-/tmp/nqiftop-demo.gif}"
SECS="${2:-34}"                              # capture duration (covers the tour)
COLS=100; ROWS=30                            # terminal geometry
FACE="${NQIFTOP_DEMO_FACE:-DejaVu Sans Mono}"
FSIZE="${NQIFTOP_DEMO_FSIZE:-13}"
FPS=12
SCALE=1000                                   # output width (px); height auto
RAW=/tmp/nqiftop-demo-raw.mp4
CFG="$(mktemp -d)"                           # sandboxed XDG_CONFIG_HOME

if [[ ! -x ./build/nqiftop-demo ]]; then
    echo "error: ./build/nqiftop-demo not built — run:" >&2
    echo "  cmake -B build -DQIFTOP_BUILD_DEMO=ON && cmake --build build --target nqiftop-demo" >&2
    exit 1
fi

# Generous virtual screen; the xterm sizes itself from cols/rows + font and we
# crop to its actual geometry afterwards.
Xvfb :98 -screen 0 1400x900x24 >/tmp/xvfb-tui.log 2>&1 &
XV=$!
sleep 2

env DISPLAY=:98 TERM=xterm-256color XDG_CONFIG_HOME="$CFG" \
    xterm -class XTermDemo -geometry ${COLS}x${ROWS}+0+0 \
          -fa "$FACE" -fs "$FSIZE" \
          -bg black -fg white -b 0 -bw 0 \
          -xrm 'xterm*vt100.allowTitleOps: false' \
          -e ./build/nqiftop-demo >/tmp/nqiftop-demo.log 2>&1 &
XT=$!
sleep 3   # let the xterm map + the demo populate data before recording

# Read the xterm's real mapped geometry (font metrics make it font-dependent).
eval "$(DISPLAY=:98 xdotool search --class XTermDemo getwindowgeometry --shell | head -8)"
PX="${X:-0}"; PY="${Y:-0}"; PW="${WIDTH:-1000}"; PH="${HEIGHT:-600}"
# ffmpeg x11grab needs even dimensions for yuv420p.
PW=$(( PW - PW % 2 )); PH=$(( PH - PH % 2 ))

ffmpeg -y -f x11grab -draw_mouse 0 -video_size ${PW}x${PH} -framerate $FPS \
    -i ":98+${PX},${PY}" -t "$SECS" -pix_fmt yuv420p "$RAW" >/tmp/ffrec-tui.log 2>&1

# Two-pass palette for crisp text/colour, then gifsicle for size.
PAL=/tmp/nqiftop-pal.png
ffmpeg -y -i "$RAW" -vf "fps=${FPS},scale=${SCALE}:-1:flags=lanczos,palettegen=stats_mode=diff" \
    "$PAL" >/tmp/ffpal-tui.log 2>&1
ffmpeg -y -i "$RAW" -i "$PAL" \
    -lavfi "fps=${FPS},scale=${SCALE}:-1:flags=lanczos[x];[x][1:v]paletteuse=dither=bayer:bayer_scale=3:diff_mode=rectangle" \
    /tmp/nqiftop-demo-unopt.gif >/tmp/ffgif-tui.log 2>&1
gifsicle -O3 --lossy=60 /tmp/nqiftop-demo-unopt.gif -o "$OUT" 2>/tmp/gifsicle-tui.log || \
    cp /tmp/nqiftop-demo-unopt.gif "$OUT"

kill -9 "$XT" 2>/dev/null || true
kill -9 "$XV" 2>/dev/null || true
rm -rf "$CFG"

echo "captured xterm geometry: ${PW}x${PH}+${PX},${PY}"
ls -la "$OUT" && file "$OUT"
