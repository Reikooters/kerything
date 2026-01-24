// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include <QProcess>
#include <QCoreApplication>
#include <QDataStream>
#include <QDebug>
#include <QMessageBox>
#include <limits>
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

    // Launch helper
    qDebug() << "Launching helper:" << helperPath << "on" << devicePath << "type:" << fsType;
    helper->start("pkexec", {helperPath, devicePath, fsType});

    QByteArray rawData;
    rawData.reserve(1024 * 1024 * 16); // start with 16 MiB to reduce early reallocations

    QByteArray stderrBuf;
    stderrBuf.reserve(4096);
    int lastPct = -1;

    // Percent completion updates from the helper are written to stderr
    // on a line beginning with KERYTHING_PROGRESS then the integer value.
    // Parse those as we get them and update the progress bar,
    // while ignoring any other lines written to stderr.
    auto drainAndParseStderr = [&](bool flushPartialLine = false) {
        stderrBuf += helper->readAllStandardError();
        std::optional<int> latestPctSeen;

        auto consumeLine = [&](QByteArrayView lineView) {
            static constexpr QByteArrayView kPrefix("KERYTHING_PROGRESS ");
            if (!lineView.startsWith(kPrefix)) {
                return;
            }

            bool ok = false;

            int pct = lineView.mid(kPrefix.size()).trimmed().toInt(&ok);

            if (!ok) {
                return;
            }

            pct = std::clamp(pct, 0, 100);
            latestPctSeen = pct;
        };

        while (true) {
            const int nl = stderrBuf.indexOf('\n');
            if (nl < 0) {
                break;
            }

            QByteArrayView lineView(stderrBuf.constData(), nl);
            consumeLine(lineView);

            stderrBuf.remove(0, nl + 1);
        }

        // If the process has finished, treat the remaining bytes as the last line.
        if (flushPartialLine && !stderrBuf.isEmpty()) {
            consumeLine(stderrBuf);
            stderrBuf.clear();
        }

        // If a progress update was received, update the percentage progress displayed,
        // but only if the new percentage value is different from the one currently shown.
        if (latestPctSeen && *latestPctSeen != lastPct) {
            lastPct = *latestPctSeen;
            Q_EMIT progressValue(lastPct);
            Q_EMIT progressMessage(QStringLiteral("Scanning device... %1%").arg(lastPct));
        }
    };

    // Loop until finished, keeping the UI responsive
    while (!helper->waitForFinished(100)) {
        QCoreApplication::processEvents();

        // drain stdout continuously to avoid deadlock if helper writes a lot.
        rawData += helper->readAllStandardOutput();

        // Drain stderr for progress updates (and ignore other stderr output)
        drainAndParseStderr();

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

    // Drain any remaining stdout/stderr after exit
    rawData += helper->readAllStandardOutput();
    drainAndParseStderr();

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

    // Helper process has finished, now we process the data
    qDebug() << "Helper finished with exit code" << helper->exitCode();

    Q_EMIT progressMessage("Processing data from helper...");
    QCoreApplication::processEvents();

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

    // Sanity: prevent absurd allocations
    static constexpr quint64 kMaxRecords = 500'000'000ULL; // 500M entries
    if (recordCount == 0 || recordCount > kMaxRecords) {
        m_isRunning = false;

        Q_EMIT progressMessage("Data stream error.");
        Q_EMIT errorMessage("Data Stream Error",
            QString("Helper returned an invalid record count: %1").arg(recordCount));

        Q_EMIT scannerFinished();
        return std::nullopt;
    }

    // 2. Read records (size checks first)
    const quint64 recordBytes64 = recordCount * static_cast<quint64>(sizeof(ScannerEngine::FileRecord));
    if (recordBytes64 / static_cast<quint64>(sizeof(ScannerEngine::FileRecord)) != recordCount) {
        m_isRunning = false;

        Q_EMIT progressMessage("Data stream error.");
        Q_EMIT errorMessage("Data Stream Error", "Record byte size overflow.");

        Q_EMIT scannerFinished();
        return std::nullopt;
    }

    if (recordBytes64 > static_cast<quint64>(std::numeric_limits<qint64>::max())) {
        m_isRunning = false;
        Q_EMIT progressMessage("Data stream error.");
        Q_EMIT errorMessage("Data Stream Error", "Record data is too large to read safely.");

        Q_EMIT scannerFinished();
        return std::nullopt;
    }

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

    if (stream.readRawData(reinterpret_cast<char*>(db.records.data()),
                           static_cast<qint64>(recordBytes64)) != static_cast<qint64>(recordBytes64)) {
        m_isRunning = false;

        Q_EMIT progressMessage("Data stream error.");
        Q_EMIT errorMessage("Data Stream Error", "Truncated stream while reading records.");

        Q_EMIT scannerFinished();
        return std::nullopt;
    }
    qDebug() << "Records transfer complete.";

    // 3. Read string pool size
    quint64 poolSize = 0;
    if (!readVal(poolSize)) {
        m_isRunning = false;

        Q_EMIT progressMessage("Data stream error.");
        Q_EMIT errorMessage("Data Stream Error", "Failed to read string pool size.");

        Q_EMIT scannerFinished();
        return std::nullopt;
    }

    // Sanity: prevent absurd allocations
    static constexpr quint64 kMaxPoolBytes = 8ULL * 1024 * 1024 * 1024; // 8 GiB
    if (poolSize == 0 || poolSize > kMaxPoolBytes) {
        m_isRunning = false;

        Q_EMIT progressMessage("Data stream error.");
        Q_EMIT errorMessage("Data Stream Error",
            QString("Helper returned an invalid string pool size: %1 bytes").arg(poolSize));
        Q_EMIT scannerFinished();

        return std::nullopt;
    }

    // Redundant extra check, just kept here in case the 8 GiB limit is ever removed
    if (poolSize > static_cast<quint64>(std::numeric_limits<qint64>::max())) {
        m_isRunning = false;

        Q_EMIT progressMessage("Data stream error.");
        Q_EMIT errorMessage("Data Stream Error", "String pool is too large to read safely.");

        Q_EMIT scannerFinished();
        return std::nullopt;
    }

    qDebug() << "String pool size:" << poolSize;

    // 4. Read string pool
    db.stringPool.resize(poolSize);
    if (stream.readRawData(db.stringPool.data(), static_cast<qint64>(poolSize)) != static_cast<qint64>(poolSize)) {
        m_isRunning = false;

        Q_EMIT progressMessage("Data stream error.");
        Q_EMIT errorMessage("Data Stream Error", "Truncated stream while reading string pool.");

        Q_EMIT scannerFinished();
        return std::nullopt;
    }

    // 5. Pre-sort data by name ascending
    qDebug() << "Data processing complete. Sorting data...";
    Q_EMIT progressMessage("Data processing complete. Sorting data...");
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