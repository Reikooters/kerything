// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_SCOPEDACCUMULATEDTIMER_H
#define KERYTHING_SCOPEDACCUMULATEDTIMER_H

#include <chrono>

class ScopedAccumulatedTimer {
public:
    using Clock = std::chrono::steady_clock;
    using Duration = std::chrono::nanoseconds;

    explicit ScopedAccumulatedTimer(Duration& target) noexcept
        : target_(target),
          start_(Clock::now())
    {
    }

    ~ScopedAccumulatedTimer()
    {
        target_ += std::chrono::duration_cast<Duration>(
            Clock::now() - start_
        );
    }

    ScopedAccumulatedTimer(const ScopedAccumulatedTimer&) = delete;
    ScopedAccumulatedTimer& operator=(const ScopedAccumulatedTimer&) = delete;

    ScopedAccumulatedTimer(ScopedAccumulatedTimer&&) = delete;
    ScopedAccumulatedTimer& operator=(ScopedAccumulatedTimer&&) = delete;

private:
    Duration& target_;
    Clock::time_point start_;
};

#endif // KERYTHING_SCOPEDACCUMULATEDTIMER_H