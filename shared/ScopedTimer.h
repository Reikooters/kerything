// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_SCOPEDTIMER_H
#define KERYTHING_SCOPEDTIMER_H

#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>

class ScopedTimer {
public:
    explicit ScopedTimer(std::string label)
        : label_(std::move(label)),
          start_(std::chrono::steady_clock::now()) {}

    ~ScopedTimer() {
        const auto end = std::chrono::steady_clock::now();
        const std::chrono::duration<double> elapsed = end - start_;

        std::cerr << label_
                  << " took "
                  << std::fixed
                  << std::setprecision(4)
                  << elapsed.count()
                  << "s\n";
    }

private:
    std::string label_;
    std::chrono::steady_clock::time_point start_;
};

#endif //KERYTHING_SCOPEDTIMER_H
