// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include <ctime>
#include "Utils.h"

namespace Utils {
    std::string utf16ToUtf8(const char16_t* utf16_ptr, size_t length) {
        if (!utf16_ptr || length == 0)
            return "";

        std::string out;
        out.reserve(length * 2); // Pre-allocate to avoid multiple reallocs
        try {
            utf8::utf16to8(utf16_ptr, utf16_ptr + length, std::back_inserter(out));
        } catch (const utf8::invalid_utf16& e) {
            return "Invalid UTF-16 Data";
        }
        return out;
    }

    std::string uint64ToFormattedTime(uint64_t time) {
        std::time_t mtime = time;
        char time_buf[20];
        std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", std::localtime(&mtime));
        return std::string{time_buf};
    }
}
