// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_GUIUTILS_H
#define KERYTHING_GUIUTILS_H

#include <QString>
#include <string>
#include <cstdint>

namespace GuiUtils {
    /**
     * Normalizes a filesystem type string to a standard format.
     *
     * This method processes the filesystem type provided as input, trims any
     * leading or trailing whitespace, converts the string to lowercase, and maps
     * specific filesystem types (e.g., NTFS and ext4) to their standardized
     * representations. If the input does not match any predefined filesystem type,
     * an empty string is returned.
     *
     * @param solidFsType The input filesystem type as a QString, which may include
     *                    leading/trailing whitespace or mixed casing.
     * @return A QString containing the normalized filesystem type (e.g., "ntfs" or "ext4"),
     *         or an empty string if no match exists.
     */
    [[nodiscard]] QString normalizeFsTypeForHelper(const QString& solidFsType);

    /**
     * Converts a timestamp in seconds since the Unix epoch to a formatted
     * date and time string in the local time zone.
     *
     * This method takes an input timestamp as a 64-bit unsigned integer,
     * validates it to ensure it is within the valid range for conversion
     * to a `std::time_t`, and formats it into the string "YYYY-MM-DD HH:MM:SS".
     * If the timestamp is outside the representable range for the local system,
     * the string "out-of-range" is returned. If a conversion or formatting
     * error occurs, "invalid-time" or "format-error" is returned.
     *
     * @param timeSeconds A 64-bit unsigned integer representing the time in seconds
     *                    since January 1, 1970 (UTC).
     * @return A std::string containing the formatted local date and time,
     *         or an error indicator such as "out-of-range", "invalid-time",
     *         or "format-error".
     */
    [[nodiscard]] std::string uint64ToFormattedTime(uint64_t timeSeconds);

    /**
     * Converts an NTFS timestamp to a human-readable date and time string in the local time zone.
     *
     * NTFS timestamps are 64-bit values representing the number of 100-nanosecond intervals
     * since January 1, 1601 (UTC). This method converts the given NTFS timestamp to the equivalent
     * local date and time string in the format "YYYY-MM-DD HH:MM:SS".
     *
     * If the input timestamp is zero, the string "N/A" is returned. If the timestamp represents a
     * time outside the representable range for the local system, "out-of-range" is returned. If a
     * formatting or conversion error occurs, "invalid-time" or "format-error" is returned.
     *
     * @param ntfsTime A 64-bit unsigned integer representing the NTFS timestamp
     *                 in 100-nanosecond intervals since January 1, 1601 (UTC).
     * @return A std::string containing the formatted local date and time string,
     *         or an error indicator such as "N/A", "out-of-range", "invalid-time", or "format-error".
     */
    [[nodiscard]] std::string ntfsTimeToLocalStr(uint64_t ntfsTime);
}

#endif //KERYTHING_GUIUTILS_H