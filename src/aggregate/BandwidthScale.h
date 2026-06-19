#pragma once

#include <algorithm>
#include <cmath>

namespace qiftop::aggregate {

// Bandwidth-gauge scale math, shared by every frontend (the ncurses
// nqiftop row gauge, the Qt GUI's RowGaugeDelegate, a future web view) so
// the "what fraction of the view scale is this row" decision is made in
// exactly one place. Widgets-free, header-only, lives in libqiftop.

// Round `maxRate` UP to a "nice" 1/2/5 × 10^k value so the loudest row's
// full-row gauge maps to a readable scale (like iftop's top ruler).
inline double niceScale(double maxRate)
{
    if (maxRate <= 0.0)
        return 1024.0; // 1 KiB/s floor so a quiet link still has a scale
    const double e    = std::floor(std::log10(maxRate));
    const double base = std::pow(10.0, e);
    const double m    = maxRate / base;
    const double nice = (m <= 1.0) ? 1.0 : (m <= 2.0) ? 2.0 : (m <= 5.0) ? 5.0 : 10.0;
    return nice * base;
}

// Gauge fraction in [0,1]: a row's combined rate against the view scale.
inline double gaugeFraction(double value, double scale)
{
    if (scale <= 0.0)
        return 0.0;
    return std::clamp(value / scale, 0.0, 1.0);
}

} // namespace qiftop::aggregate
