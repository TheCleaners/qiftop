# pkgsrc packaging for qiftop (BSD) — SCAFFOLDING

This directory is **scaffolding for a pkgsrc package** and has **not been
verified end-to-end** in a real pkgsrc tree yet. It packages the BSD client
surface only — `libqiftop`, the `qiftop` GUI, the `nqiftop` TUI, and the
`check_qiftop` monitoring plugin. The privileged `qiftop-agent` is Linux-only
(CMake gates it to `QIFTOP_PLATFORM == linux`) and is not part of this package.

See [`../../docs/PORTABILITY.md` §7](../../docs/PORTABILITY.md) for the full
BSD implementation guide and the trial-and-error lessons behind this backend.

## Files

| File       | Purpose |
|------------|---------|
| `Makefile` | Package definition: GitHub source, CMake build, Qt6 + base curses deps. |
| `DESCR`    | Long description. |
| `PLIST`    | Installed file list, captured from a real staged `cmake --install` on NetBSD 11. |
| `distinfo` | Source checksums (SHA512 + Size as a starting point; BLAKE2s via `make makesum`). |

## How to finish & verify

1. Place this directory in a pkgsrc tree as `net/qiftop` (or wire it up as a
   local/wip package).
2. `make makesum` — regenerate `distinfo` (adds the required BLAKE2s line and
   authoritative hashes from the actually-fetched tarball).
3. `make` — build. One known rough edge to expect:
   - **Man-page compression.** Upstream CMake installs pre-gzipped `.gz` man
     pages; pkgsrc normally compresses man pages itself. Either make CMake
     install uncompressed man pages, or keep `.gz` and reconcile with
     `PLIST`/`MANCOMPRESSED`.

   (The Linux/agent-only data files — D-Bus policy, systemd unit, nftables
   shim, agent man pages and completions — are now gated on the agent target
   in `CMakeLists.txt` and do not install on BSD, so the `PLIST` is already
   free of them.)
4. `make print-PLIST > PLIST` — regenerate the file list from the real install
   once the above are settled.
5. `make package` / `pkglint` — finalize.

## Dependencies (all satisfied on NetBSD 11)

- `x11/qt6-qtbase` (pkgsrc)
- base `libcurses` (wide-capable — **do not** add pkgsrc `ncurses`)
- base `libpcap`, `getifaddrs(3)`, `sysctl` (no extra deps)

Capturing flows needs read access to `/dev/bpf*` (root).
