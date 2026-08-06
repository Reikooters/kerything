// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHINGD_FANOTIFYWATCHER_H
#define KERYTHINGD_FANOTIFYWATCHER_H

#include <QObject>
#include <QSocketNotifier>
#include <QString>
#include <QTimer>

#include <vector>

#include "../shared/LiveUpdateEvent.h"

class FanotifyWatcher final : public QObject {
    Q_OBJECT

public:
    FanotifyWatcher(
        QString deviceId,
        QString mountPoint,
        QObject* parent = nullptr
    );

    ~FanotifyWatcher() override;

    [[nodiscard]] bool start();
    [[nodiscard]] bool isRunning() const noexcept;

    [[nodiscard]] QString deviceId() const;
    [[nodiscard]] QString mountPoint() const;

Q_SIGNALS:
    void overflow(QString deviceId);
    void fatalError(QString deviceId, QString errorText);
    void eventsReady(QString deviceId, QString mountPoint, std::vector<LiveUpdateEvent> events);

private Q_SLOTS:
    void onActivated();
    void flushPendingEvents();

private:
    void drainEvents();
    void captureEvent(const struct fanotify_event_metadata* metadata);
    void closeFanotifyFd();

    QString deviceId_;
    QString mountPoint_;
    int fanotifyFd_ = -1;
    QSocketNotifier* notifier_ = nullptr;
    QTimer batchTimer_;
    std::vector<LiveUpdateEvent> pendingEvents_;
};

#endif // KERYTHINGD_FANOTIFYWATCHER_H