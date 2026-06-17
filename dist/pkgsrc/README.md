# pkgsrc packaging for qiftop (BSD) — scaffolding

This directory is an unverified pkgsrc package scaffold. It packages the BSD
client surface only: `libqiftop`, `qiftop`, `nqiftop`, and `check_qiftop`.
The privileged `qiftop-agent` is Linux-only and is not installed here.

See [`../../docs/PORTABILITY.md` §7](../../docs/PORTABILITY.md) for the BSD
backend guide.

## Files

| File | Purpose |
|------|---------|
| `Makefile` | GitHub source, CMake build, Qt6 and base curses dependencies. |
| `DESCR` | Long description. |
| `PLIST` | Installed file list from a staged NetBSD 11 `cmake --install`. |
| `distinfo` | Initial SHA512/Size checksums; `make makesum` adds BLAKE2s and final hashes. |

## Finish and verify

1. Place this directory in a pkgsrc tree as `net/qiftop` or a local/wip package.
2. `make makesum`
3. `make`
   - Known issue: upstream installs pre-gzipped man pages; either install
     uncompressed pages or reconcile `.gz` entries with `PLIST`/`MANCOMPRESSED`.
4. `make print-PLIST > PLIST`
5. `make package` / `pkglint`

Linux-only agent files are CMake-gated and should not appear in the BSD `PLIST`.

## Dependencies (NetBSD 11)

- `x11/qt6-qtbase`
- base wide-capable `libcurses` (do **not** add pkgsrc `ncurses`)
- base `libpcap`, `getifaddrs(3)`, and `sysctl`

Flow capture needs read access to `/dev/bpf*` (root).
