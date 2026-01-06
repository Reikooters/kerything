// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include <QProcess>
#include <QCoreApplication>
#include <QDataStream>
#include <QDebug>
#include <QMessageBox>
#include "ScannerManager.h"

ScannerManager::ScannerManager(QObject *parent) : QObject(parent) {}

ScannerManager::~ScannerManager() {
    // If the manager is destroyed while a scan is running
    // (e.g. user closed the dialog window), request a cancel.
    requestCancel();
}

std::optional<ScannerEngine::SearchDatabase> ScannerManager::scanDevice(const QString &devicePath, const QString &fsType) {
    m_isRunning = true;
    m_cancelRequested = false;
    Q_EMIT scannerStarted();
    Q_EMIT progressMessage("Authenticating and starting scanner...");

    // Using pkexec triggers the system authentication dialog (Native KDE feel)
    // We assume 'kerything-scanner-helper' is in the same directory as our app

    // Use unique_ptr to prevent leaks on early returns
    auto helper = std::make_unique<QProcess>();
    QString helperPath = QCoreApplication::applicationDirPath() + "/kerything-scanner-helper";

    // We want to handle the data as a stream
    qDebug() << "Launching helper:" << helperPath << "on" << devicePath << "type:" << fsType;
    helper->start("pkexec", {helperPath, devicePath, fsType});

    // Loop until finished, keeping the UI responsive
    while (!helper->waitForFinished(100)) {
        QCoreApplication::processEvents();

        // Check if user clicked the "Cancel" button or closed the dialog
        if (m_cancelRequested) {
            qDebug() << "Cancellation requested. Abandoning process...";

            helper->kill(); // Send SIGKILL

            // Do not call waitForFinished here, otherwise the GUI will freeze.
            // We tell the process to delete itself whenever it finally manages to exit.

            // Ownership transfer: We release from unique_ptr because the process
            // needs to live long enough to die properly in the background.
            QProcess* helperPtr = helper.release();
            connect(helperPtr, &QProcess::finished, helperPtr, &QObject::deleteLater);

            m_isRunning = false;

            Q_EMIT progressMessage("Scanner cancelled.");

            Q_EMIT scannerFinished();
            return std::nullopt;
        }
    }

    if (helper->exitCode() != 0) {
        m_isRunning = false;

        qDebug() << "Helper failed with exit code" << helper->exitCode();

        Q_EMIT progressMessage("Scanner failed.");
        Q_EMIT errorMessage("Scanner Helper Failed",
            QString("The scanner process exited with code %1.\n\n"
                    "This usually means the partition is busy or pkexec was cancelled.")
            .arg(helper->exitCode()));

        Q_EMIT scannerFinished();
        return std::nullopt;
    }

    // Helper process has finished, now we consume the data
    qDebug() << "Helper finished with exit code" << helper->exitCode();

    Q_EMIT progressMessage("Transferring data from helper...");
    QCoreApplication::processEvents();

    QByteArray rawData = helper->readAllStandardOutput();

    if (rawData.isEmpty()) {
        m_isRunning = false;

        qDebug() << "No data received from helper.";

        Q_EMIT progressMessage("No data received.");
        Q_EMIT errorMessage("No Data Received",
            "The scanner helper finished but sent no data. If this partition is very large, "
            "it might have run out of memory.");

        Q_EMIT scannerFinished();
        return std::nullopt;
    }

    // Use a QDataStream directly on the process's stdout
    QDataStream stream(rawData);
    stream.setByteOrder(QDataStream::LittleEndian);

    ScannerEngine::SearchDatabase db;

    auto readVal = [&](auto& val) {
        return stream.readRawData(reinterpret_cast<char*>(&val), sizeof(val)) == sizeof(val);
    };

    // 1. Read record count
    quint64 recordCount = 0;
    if (!readVal(recordCount)) {
        m_isRunning = false;

        qDebug() << "Failed to read record count";

        Q_EMIT progressMessage("Data stream error.");
        Q_EMIT errorMessage("Data Stream Error", "Failed to read record count from the helper stream.");

        Q_EMIT scannerFinished();
        return std::nullopt;
    }
    qDebug() << "Helper reporting" << recordCount << "records. Allocating memory...";

    // 2. Read records
    try {
        db.records.resize(recordCount);
    } catch (const std::exception& e) {
        m_isRunning = false;

        qDebug() << "CRASH prevented! Attempted to resize to" << recordCount << "records. Error:" << e.what();

        Q_EMIT progressMessage("Memory allocation failed.");
        Q_EMIT errorMessage("Memory Allocation Failed",
            QString("Failed to allocate memory for %1 records.\nError: %2")
            .arg(recordCount).arg(e.what()));

        Q_EMIT scannerFinished();
        return std::nullopt;
    }

    stream.readRawData(reinterpret_cast<char*>(db.records.data()), static_cast<qint64>(recordCount * sizeof(ScannerEngine::FileRecord)));
    qDebug() << "Records transfer complete.";

    // 3. Read string pool size
    quint64 poolSize = 0;
    if (!readVal(poolSize)) {
        m_isRunning = false;
        Q_EMIT scannerFinished();

        return std::nullopt;
    }

    qDebug() << "String pool size:" << poolSize;

    // 4. Read string pool
    db.stringPool.resize(poolSize);
    stream.readRawData(db.stringPool.data(), static_cast<qint64>(poolSize));

    // 5. Pre-sort data by name ascending
    qDebug() << "Data transfer complete. Sorting data...";
    Q_EMIT progressMessage("Data transfer complete. Sorting data...");
    QCoreApplication::processEvents();
    db.sortByNameAscendingParallel();

    // 6. Build the trigram index
    qDebug() << "Building trigrams index...";
    Q_EMIT progressMessage("Building search index...");
    QCoreApplication::processEvents();
    db.buildTrigramIndexParallel();

    qDebug() << "Index generation complete.";
    m_isRunning = false;
    Q_EMIT scannerFinished();
    return db;
}