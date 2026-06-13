#pragma once

// Portable curses include for nqiftop.
//
// Linux (and pkgsrc) ship the curses API as <ncurses.h>; the BSD base
// systems (NetBSD, OpenBSD) ship a wide-character-capable <curses.h> with
// no <ncurses.h>. Both expose the same API subset nqiftop relies on, so
// pick whichever header exists. __has_include is standard C++17.
//
// nqiftop renders box-drawing and other multibyte glyphs via the WIDE API
// (add_wch / addwstr). Some ncurses headers only declare those prototypes
// when _XOPEN_SOURCE_EXTENDED / NCURSES_WIDECHAR is requested, so ask for
// them before the header is pulled in.
#ifndef _XOPEN_SOURCE_EXTENDED
#  define _XOPEN_SOURCE_EXTENDED 1
#endif
#ifndef NCURSES_WIDECHAR
#  define NCURSES_WIDECHAR 1
#endif

#if defined(__has_include)
#  if __has_include(<ncurses.h>)
#    include <ncurses.h>
#  else
#    include <curses.h>
#  endif
#else
#  include <ncurses.h>
#endif

