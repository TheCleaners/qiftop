# $NetBSD$
#
# pkgsrc package for qiftop (BSD client side).
#
# STATUS: SCAFFOLDING — not yet verified end-to-end with a real pkgsrc tree.
# This builds the BSD client surface only: libqiftop, the qiftop GUI, the
# nqiftop TUI, and the check_qiftop monitoring plugin. The privileged
# qiftop-agent is Linux-only (CMake gates it to QIFTOP_PLATFORM==linux) and is
# NOT part of this package.
#
# To finish/verify (see dist/pkgsrc/README.md):
#   1. Drop this directory into a pkgsrc tree under net/qiftop (or use it as a
#      local/wip package), then run `make makesum` to populate distinfo from
#      the release tarball, and `make print-PLIST > PLIST` to regenerate the
#      file list from an actual DESTDIR install (the PLIST here is a
#      best-effort starting point and almost certainly needs adjustment —
#      several install(FILES) rules ship Linux-only data such as the systemd
#      unit and the D-Bus system policy that should be pruned for BSD).
#   2. Build with `make` and fix up PLIST/options until clean.

DISTNAME=	qiftop-0.2.5
CATEGORIES=	net
MASTER_SITES=	${MASTER_SITE_GITHUB:=TheCleaners/}
GITHUB_PROJECT=	qiftop
GITHUB_TAG=	v${PKGVERSION_NOREV}

MAINTAINER=	pkgsrc-users@NetBSD.org
HOMEPAGE=	https://github.com/TheCleaners/qiftop
COMMENT=	Qt6 iftop-style network monitor (GUI, ncurses TUI, library)
LICENSE=	gnu-gpl-v2 OR gnu-gpl-v3

USE_LANGUAGES=	c c++
USE_CMAKE=	yes
USE_TOOLS+=	pkg-config

# C++20.
GCC_REQD+=	10

# Build only the client side; the agent is Linux-only and tests/auto-package
# are off for a distro build (the agent target's POST_BUILD cpack hook would
# otherwise fire, and it is gated out on BSD anyway).
CMAKE_ARGS+=	-DCMAKE_BUILD_TYPE=Release
CMAKE_ARGS+=	-DQIFTOP_BUILD_TESTS=OFF
CMAKE_ARGS+=	-DQIFTOP_AUTO_PACKAGE=OFF

# Per-flow capture uses libpcap/BPF (base); per-interface stats use
# getifaddrs(3) (base); the nqiftop TUI uses base curses (wide-capable —
# pkgsrc ncurses is intentionally NOT a dependency, see docs/PORTABILITY.md
# §7.1). Qt6 comes from pkgsrc.
.include "../../x11/qt6-qtbase/buildlink3.mk"
.include "../../mk/curses.buildlink3.mk"
.include "../../mk/bsd.pkg.mk"
