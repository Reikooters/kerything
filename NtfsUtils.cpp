// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include <cstdint>
#include <ctime>
#include <string>
#include "NtfsUtils.h"

namespace NtfsUtils {
    /**
     * NTFS timestamps are 64-bit values representing the number of
     * 100-nanosecond intervals since January 1, 1601 (UTC).
     */
    std::string ntfsTimeToStr(uint64_t ntfsTime) {
        if (ntfsTime == 0) {
            return "N/A";
        }

        // Convert to Unix timestamp: NTFS is 100ns intervals since 1601
        // Unix is seconds since 1970. Difference is 11644473600 seconds.
        time_t unixTime = static_cast<time_t>((ntfsTime / 10000000) - 11644473600ULL);

        char buf[20];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::gmtime(&unixTime));

        return std::string{buf};
    }
}
