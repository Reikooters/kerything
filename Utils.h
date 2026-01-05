// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_UTILS_H
#define KERYTHING_UTILS_H

#include <string>
#include "lib/utf8.h"

namespace Utils {
    std::string utf16ToUtf8(const char16_t* utf16_ptr, size_t length);
}

#endif //KERYTHING_UTILS_H