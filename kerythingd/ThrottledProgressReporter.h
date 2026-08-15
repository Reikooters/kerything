// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHINGD_THROTTLEDPROGRESSREPORTER_H
#define KERYTHINGD_THROTTLEDPROGRESSREPORTER_H

#include <chrono>
#include <cstdint>
#include <utility>

#include "Protocol.h"

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

    void operator()(Protocol::ScanProgress progress)
    {
        const bool totalKnown = progress.total > 0;

        // Clamp progress only when callers supplied a known total.
        // A total of zero means "unknown total", which is valid for scanners
        // that discover entries while walking directories.
        if (totalKnown && progress.processed > progress.total) {
            progress.processed = progress.total;
        }

        const bool completed = totalKnown && (progress.processed == progress.total);

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
        sink_(std::move(progress));
    }

private:
    Sink sink_;
    std::chrono::milliseconds minInterval_;
    Clock::time_point nextEmit_;
};

#endif // KERYTHINGD_THROTTLEDPROGRESSREPORTER_H