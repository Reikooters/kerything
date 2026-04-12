// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "GuiUtils.h"
#include <ctime>

namespace GuiUtils {

    QString normalizeFsTypeForHelper(const QString& solidFsType) {
        const QString t = solidFsType.trimmed().toLower();
        if (t.contains("ntfs")) {
            return QStringLiteral("ntfs");
        }
        if (t.contains("ext4")) {
            return QStringLiteral("ext4");
        }
        return {};
    }

    std::string uint64ToFormattedTime(const uint64_t timeSeconds) {
        using time_t_limits = std::numeric_limits<std::time_t>;

        // If time_t is signed, its max might not fit in uint64_t on weird platforms,
        // but in practice time_t is <= 64 bits. This is the safe intent:
        const auto max_time_t_as_u64 =
            static_cast<std::uint64_t>(time_t_limits::max());

        if (timeSeconds > max_time_t_as_u64) {
            return "out-of-range";
        }

        const std::time_t tt = static_cast<std::time_t>(timeSeconds);

        std::tm tm{};
        if (::localtime_r(&tt, &tm) == nullptr) {
            return "invalid-time";
        }

        std::array<char, 20> buf{}; // "YYYY-MM-DD HH:MM:SS" + '\0' = 20
        if (std::strftime(buf.data(), buf.size(), "%Y-%m-%d %H:%M:%S", &tm) == 0) {
            return "format-error";
        }
        return std::string{buf.data()};
    }

    std::string ntfsTimeToLocalStr(const uint64_t ntfsTime) {
        if (ntfsTime == 0) {
            return "N/A";
        }

        // NTFS FILETIME: 100 ns ticks since 1601-01-01 (UTC)
        // Convert to whole seconds since 1601.
        const std::uint64_t secs1601_u64 = ntfsTime / 10'000'000ULL;

        // 1601-01-01 -> 1970-01-01 difference in seconds
        static constexpr std::int64_t kSecsBetween1601And1970 = 11'644'473'600LL;

        // secs1601 fits comfortably in int64_t for all valid FILETIME values (max is about 1.84e12 seconds).
        const std::int64_t secs1601 = static_cast<std::int64_t>(secs1601_u64);
        const std::int64_t secs1970 = secs1601 - kSecsBetween1601And1970; // may be negative (pre-1970)

        using time_limits = std::numeric_limits<std::time_t>;

        // Range-check before casting to time_t (prevents implementation-defined narrowing/overflow).
        const auto min_tt = static_cast<std::int64_t>(time_limits::min());
        const auto max_tt = static_cast<std::int64_t>(time_limits::max());

        if (secs1970 < min_tt || secs1970 > max_tt) {
            return "out-of-range";
        }

        const std::time_t tt = static_cast<std::time_t>(secs1970);

        std::tm tm{};
        if (::localtime_r(&tt, &tm) == nullptr) {
            return "invalid-time";
        }

        std::array<char, 20> buf{}; // "YYYY-MM-DD HH:MM:SS" + '\0'
        if (std::strftime(buf.data(), buf.size(), "%Y-%m-%d %H:%M:%S", &tm) == 0) {
            return "format-error";
        }

        return std::string{buf.data()};
    }
}
