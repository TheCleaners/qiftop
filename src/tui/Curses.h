#pragma once

// Portable curses include for nqiftop.
//
// Linux (and pkgsrc) ship the curses API as <ncurses.h>; the BSD base
// systems (NetBSD, OpenBSD) ship a wide-character-capable <curses.h> with
// no <ncurses.h>. Both expose the same API subset nqiftop relies on, so
// pick whichever header exists. __has_include is standard C++17.
#if defined(__has_include)
#  if __has_include(<ncurses.h>)
#    include <ncurses.h>
#  else
#    include <curses.h>
#  endif
#else
#  include <ncurses.h>
#endif
