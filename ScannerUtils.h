// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_SCANNERUTILS_H
#define KERYTHING_SCANNERUTILS_H

#include <string>
#include <cstddef>

namespace ScannerUtils {
    /**
     * Converts a UTF-16 string to a UTF-8 encoded string.
     *
     * This function takes a pointer to a UTF-16 encoded string and its length,
     * converts it to a UTF-8 encoded string, and returns the result. If the
     * input is null or its length is zero, it returns an empty string.
     *
     * @param utf16_ptr Pointer to the UTF-16 encoded string. Must not be null.
     * @param length The number of UTF-16 code units in the string.
     *
     * @return A UTF-8 encoded string. If the UTF-16 data is invalid,
     *         returns "Invalid UTF-16 Data".
     */
    std::string utf16ToUtf8(const char16_t* utf16_ptr, size_t length);
}

#endif //KERYTHING_SCANNERUTILS_H