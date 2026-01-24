// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_SCANNERMANAGER_H
#define KERYTHING_SCANNERMANAGER_H

#include <QObject>
#include <QString>
#include <optional>
#include "ScannerEngine.h"

class ScannerManager : public QObject {
    Q_OBJECT
public:
    explicit ScannerManager(QObject *parent = nullptr);
    ~ScannerManager() override;

    /**
     * @brief Starts the scanning process for the given device.
     * This runs synchronously (blocking) but processes events to keep UI alive.
     */
    std::optional<ScannerEngine::SearchDatabase> scanDevice(const QString &devicePath, const QString &fsType);

    [[nodiscard]] bool isRunning() const { return m_isRunning; }
    void requestCancel() { m_cancelRequested = true; }

signals:
    void progressMessage(const QString &message);
    void progressValue(int percent);
    void errorMessage(const QString &title, const QString &message);
    void scannerStarted();
    void scannerFinished();

private:
    bool m_isRunning = false;
    bool m_cancelRequested = false;
};

#endif //KERYTHING_SCANNERMANAGER_H