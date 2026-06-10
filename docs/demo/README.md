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
| `openbox/` | A minimal dark Openbox theme (`qiftop-dark`, orange accent) + `rc.xml` that maximizes the main window and centres dialogs, so the headless capture gets a real, on-brand title bar. |
| `capture-still.sh` | Brings up `Xvfb` + Openbox, runs the demo, grabs one frame → PNG. |
| `capture-gif.sh` | Same, but drives a scripted UI tour with `xdotool` (open the View dropdown → group by container → by process → open Preferences → tour the side-nav + Colours page) and records it with `ffmpeg`, then palette-optimises to a GIF. |
| `probe.sh` | Coordinate-probe helper: clicks an `(x, y)` and screenshots, for re-finding toolbar / dialog click targets when the layout changes. |

## Dependencies

```sh
sudo apt install xvfb openbox xdotool ffmpeg gifsicle imagemagick \
    adwaita-qt6 qgnomeplatform-qt6
```

The capture renders with the **Adwaita-Dark** Qt style + the gnome
platform theme (for themed toolbar icons); `qiftop-demo --dark` provides a
built-in Fusion dark palette fallback for environments without those.

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
cmake --build build --target qiftop-demo -j"$(nproc)"

# animated tour → /tmp/demo.gif
bash docs/demo/capture-gif.sh /tmp/demo.gif Adwaita-Dark 26
gifsicle -O3 --lossy=80 /tmp/demo.gif -o /tmp/demo.gif

# overwrite the pinned asset so every release's URL resolves to the update:
gh release upload v0.2-rc1 /tmp/demo.gif --clobber
```

`capture-still.sh` produces a single PNG frame — handy for iterating on
the look or for a static thumbnail, though the README uses only the GIF.

The `qiftop-demo` target is never built by default and is not installed —
it exists purely for documentation media.
