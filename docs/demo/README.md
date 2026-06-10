# Demo capture harness

Tooling that generates the README screenshot + animated GIF from a
**fully synthetic, privacy-safe** run of the real qiftop GUI. Nothing here
touches a live kernel, conntrack table, DBus agent, or the host's real
network — every interface, address, hostname, PID and container shown is
fabricated:

* addresses use the reserved documentation ranges — RFC 5737 TEST-NET
  (`192.0.2.0/24`, `198.51.100.0/24`, `203.0.113.0/24`) and RFC 3849
  (`2001:db8::/32`),
* hostnames use the reserved `example.*` domains (RFC 2606).

So a capture can be committed to the repo without leaking anything about
the machine it was recorded on.

## Pieces

| File | Role |
|------|------|
| `qiftop-demo.cpp` | A gated executable (`-DQIFTOP_BUILD_DEMO=ON`) that builds the **real** `MainWindow` with the in-memory test fakes (`tests/fakes/FakeMonitors.h`) and feeds it a curated synthetic dataset, animating the gauges + rates. Pretends to talk to a full-capability agent so the v0.2 process / container columns un-gate. |
| `nqiftop-demo.cpp` | Terminal sibling: same gate, wires the **real** `Screen` + `TuiApp` (the ncurses front-end) to the fakes and runs a scripted key tour internally via `TuiApp::handleKey` (group by process/container, expand a detail tree, open Settings, cycle themes, expand an interface). Deterministic — needs no `xdotool` for input. |
| `openbox/` | A minimal dark Openbox theme (`qiftop-dark`, orange accent) + `rc.xml` that maximizes the main window and centres dialogs, so the headless capture gets a real, on-brand title bar. |
| `capture-still.sh` | Brings up `Xvfb` + Openbox, runs the demo, grabs one frame → PNG. |
| `capture-gif.sh` | Same, but drives a scripted GUI tour with `xdotool` (open the View dropdown → group by container → by process → open Preferences → tour the side-nav + Colours page) and records it with `ffmpeg`, then palette-optimises to a GIF. |
| `capture-tui-gif.sh` | Records `nqiftop-demo` running in an `xterm` under `Xvfb` (Xft `DejaVu Sans Mono` face so the box-drawing/arrow/block glyphs render) → palette-optimised GIF. The xterm's real geometry is read back with `xdotool` for an exact crop. |
| `probe.sh` | Coordinate-probe helper: clicks an `(x, y)` and screenshots, for re-finding toolbar / dialog click targets when the layout changes. |

## Dependencies

```sh
sudo apt install xvfb openbox xdotool ffmpeg gifsicle imagemagick \
    xterm fonts-dejavu-core adwaita-qt6 qgnomeplatform-qt6
```

The GUI capture renders with the **Adwaita-Dark** Qt style + the gnome
platform theme (for themed toolbar icons); `qiftop-demo --dark` provides a
built-in Fusion dark palette fallback for environments without those. The
TUI capture only needs `xterm` + a Unicode monospace TTF (`fonts-dejavu-core`).

## Regenerating the demo media

The animated GIF shown in the README is **not committed to the repo** (to
keep it lean) — it is attached as an asset to the **`v0.2-rc1`** GitHub
Release, and both the README and the release-notes preamble pin that one
fixed URL. The GIF is **not regenerated per release**: every release
reuses the `v0.2-rc1` asset unless the UI changes fundamentally enough to
warrant a fresh capture.

To refresh it (only on a fundamental UI change): regenerate, re-upload to
the **same pinned tag**, and the existing URLs resolve to the new GIF
automatically — no README / workflow edits needed. If you instead pin a
new tag, update the URL in `README.md` *and* `.github/workflows/release.yml`.

```sh
cmake -B build -DQIFTOP_BUILD_DEMO=ON
cmake --build build --target qiftop-demo nqiftop-demo -j"$(nproc)"

# GUI: animated tour → /tmp/demo.gif
bash docs/demo/capture-gif.sh /tmp/demo.gif Adwaita-Dark 26
gifsicle -O3 --lossy=80 /tmp/demo.gif -o /tmp/demo.gif

# TUI: ncurses tour → /tmp/nqiftop-demo.gif (self-driving; ~34 s)
bash docs/demo/capture-tui-gif.sh /tmp/nqiftop-demo.gif 34

# overwrite the pinned assets so every release's URL resolves to the update:
gh release upload v0.2-rc1 /tmp/demo.gif /tmp/nqiftop-demo.gif --clobber
```

Both GIFs are pinned to the **`v0.2-rc1`** release and reused across
releases; `nqiftop-demo.gif` is the terminal front-end capture shown
second in the README.

`capture-still.sh` produces a single PNG frame — handy for iterating on
the look or for a static thumbnail, though the README uses only the GIF.

The `qiftop-demo` / `nqiftop-demo` targets are never built by default and
are not installed — they exist purely for documentation media.
