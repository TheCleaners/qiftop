#pragma once

// XDG Autostart helper. Manages a per-user .desktop entry under
// $XDG_CONFIG_HOME/autostart/ (typically ~/.config/autostart/) that
// every spec-compliant desktop environment honors at login. Kept
// header-light so it can be unit-tested without pulling in the rest
// of the UI stack.

#include <QString>

namespace qiftop::autostart {

// Full path to the autostart entry we manage. May be invoked even when
// the file does not exist (caller uses it for write/remove).
[[nodiscard]] QString entryPath();

// Returns true iff the autostart entry exists and is not disabled
// (Hidden=true / X-GNOME-Autostart-enabled=false). I.e. "will qiftop
// actually be launched at next login?".
[[nodiscard]] bool isEnabled();

// Write or remove the autostart entry to match `enabled`. When writing,
// the Exec line includes `--tray` so the autostarted instance comes up
// silently in the system tray instead of popping a window. Returns
// true on success; false on filesystem error (also logs via qWarning).
bool setEnabled(bool enabled);

} // namespace qiftop::autostart
