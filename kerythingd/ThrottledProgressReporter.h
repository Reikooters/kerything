// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHINGD_THROTTLEDPROGRESSREPORTER_H
#define KERYTHINGD_THROTTLEDPROGRESSREPORTER_H

#include <chrono>
#include <cstdint>
#include <utility>

// Small helper that limits how often progress updates are forwarded.
// It is meant to wrap a callback like Q_EMIT scanProgress(...).
// - It forwards updates at most once per minInterval.
// - It always forwards the final "completed" update.
// This keeps fast scans from spamming the UI with progress events.
template <typename Sink>
class ThrottledProgressReporter {
public:
    using Clock = std::chrono::steady_clock;

    explicit ThrottledProgressReporter(
        Sink sink,
        std::chrono::milliseconds minInterval = std::chrono::milliseconds(100))
        : sink_(std::move(sink))
        , minInterval_(minInterval)
        , nextEmit_(Clock::now())
    {
    }

    void operator()(uint64_t done, uint64_t total)
    {
        // Avoid divide-by-zero style edge cases in callers that may not know
        // the total yet.
        if (total == 0) {
            total = 1;
        }

        // Clamp progress so "done" never exceeds "total".
        if (done > total) {
            done = total;
        }

        const bool completed = (done == total);

        // If we're not finished yet, only allow an update when enough time
        // has passed since the last forwarded event.
        const auto now = Clock::now();
        if (!completed && now < nextEmit_) {
            return;
        }

        // For intermediate updates, push the next allowed emit time forward.
        // Completion is always forwarded immediately, so it does not matter
        // whether the interval has elapsed.
        if (!completed) {
            nextEmit_ = now + minInterval_;
        }

        // Forward the progress update to the real sink.
        // In ScannerWorker this sink can simply call Q_EMIT scanProgress(...).
        sink_(done, total);
    }

private:
    Sink sink_;
    std::chrono::milliseconds minInterval_;
    Clock::time_point nextEmit_;
};

#endif // KERYTHINGD_THROTTLEDPROGRESSREPORTER_H