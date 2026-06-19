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
| `nqiftop-demo.cpp` | Terminal sibling: same gate, wires the **real** `Screen` + `TuiApp` (the ncurses front-end) to the fakes and runs a scripted key tour internally via `TuiApp::handleKey` (group by process/container, open the modal detail inspector and browse adjacent rows, open Settings, cycle themes, inspect an interface). Deterministic — needs no `xdotool` for input. |
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

The animated GIFs shown in the README are **not committed to the repo** (to
keep it lean) — each one is attached as an asset to the GitHub Release where
the media was **last refreshed** (the "media release"). The README and the
release-notes preamble both point at that release's URL. As of writing the
media release is **`v0.3.2`**.

**Policy — pin to the refresh release, never clobber history.** Older releases
keep whatever media they shipped with, so a v0.2 release's demo roughly
reflects the v0.2 UI rather than getting silently rewritten. When the UI
changes enough to warrant a fresh capture, you upload the new GIFs to the
**next** release and re-point the README — you do **not** overwrite the assets
on old tags.

The demo GIF only appears in the notes of **stable** releases; RCs and
pre-releases (tags with a `-`, e.g. `v0.4.0-rc1`) skip it — see the
`prerelease` gate in `.github/workflows/release.yml`.

To refresh (on a fundamental UI change):

```sh
cmake -B build -DQIFTOP_BUILD_DEMO=ON
cmake --build build --target qiftop-demo nqiftop-demo -j"$(nproc)"

# GUI: animated tour → /tmp/demo.gif
bash docs/demo/capture-gif.sh /tmp/demo.gif Adwaita-Dark 26
gifsicle -O3 --lossy=80 /tmp/demo.gif -o /tmp/demo.gif

# TUI: ncurses tour → /tmp/nqiftop-demo.gif (self-driving; ~40 s)
bash docs/demo/capture-tui-gif.sh /tmp/nqiftop-demo.gif 40

# Upload to the NEW media release (the version that ships the refreshed UI),
# NOT --clobber onto an old tag:
gh release upload v0.3.2 /tmp/demo.gif /tmp/nqiftop-demo.gif
```

Then update the media tag in **three** places so everything resolves to the
new assets: the two URLs in `README.md`, and the `MEDIA_TAG` env in
`.github/workflows/release.yml`. (This file's "media release" line too, while
you're at it.)

`nqiftop-demo.gif` is the terminal front-end capture shown second in the
README. `capture-still.sh` produces a single PNG frame — handy for iterating
on the look or for a static thumbnail, though the README uses only the GIFs.

The `qiftop-demo` / `nqiftop-demo` targets are never built by default and
are not installed — they exist purely for documentation media.
