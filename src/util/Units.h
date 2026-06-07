#pragma once

#include <QString>

namespace util {

// Format a byte rate using IEC binary units (KiB/s, MiB/s, GiB/s).
[[nodiscard]] inline QString formatByteRate(double bytesPerSec)
{
    constexpr double kKiB = 1024.0;
    constexpr double kMiB = kKiB * 1024.0;
    constexpr double kGiB = kMiB * 1024.0;

    if (bytesPerSec >= kGiB)
        return QStringLiteral("%1 GiB/s").arg(bytesPerSec / kGiB, 0, 'f', 2);
    if (bytesPerSec >= kMiB)
        return QStringLiteral("%1 MiB/s").arg(bytesPerSec / kMiB, 0, 'f', 2);
    if (bytesPerSec >= kKiB)
        return QStringLiteral("%1 KiB/s").arg(bytesPerSec / kKiB, 0, 'f', 1);
    return QStringLiteral("%1 B/s").arg(static_cast<qint64>(bytesPerSec));
}

// Format a cumulative byte count using IEC binary units.
[[nodiscard]] inline QString formatBytes(quint64 bytes)
{
    constexpr double kKiB = 1024.0;
    constexpr double kMiB = kKiB * 1024.0;
    constexpr double kGiB = kMiB * 1024.0;
    constexpr double kTiB = kGiB * 1024.0;

    const auto b = static_cast<double>(bytes);
    if (b >= kTiB) return QStringLiteral("%1 TiB").arg(b / kTiB, 0, 'f', 2);
    if (b >= kGiB) return QStringLiteral("%1 GiB").arg(b / kGiB, 0, 'f', 2);
    if (b >= kMiB) return QStringLiteral("%1 MiB").arg(b / kMiB, 0, 'f', 2);
    if (b >= kKiB) return QStringLiteral("%1 KiB").arg(b / kKiB, 0, 'f', 1);
    return QStringLiteral("%1 B").arg(bytes);
}

} // namespace util
