// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "IndexPersistenceManager.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>

namespace {
    constexpr quint32 FormatMagic = 0x4B495831; // "KIX1"
    constexpr quint32 FormatVersion = 1;

    template <typename T>
    void writeVector(QDataStream& out, const std::vector<T>& vector)
    {
        out << static_cast<quint64>(vector.size());

        if (!vector.empty()) {
            out.writeRawData(
                reinterpret_cast<const char*>(vector.data()),
                static_cast<qsizetype>(vector.size() * sizeof(T))
            );
        }
    }

    template <typename T>
    bool readVector(QDataStream& in, std::vector<T>& vector, quint64 maxItems)
    {
        quint64 size = 0;
        in >> size;

        if (in.status() != QDataStream::Ok || size > maxItems) {
            return false;
        }

        vector.resize(static_cast<std::size_t>(size));

        if (!vector.empty()) {
            const qsizetype bytes = static_cast<qsizetype>(vector.size() * sizeof(T));
            if (in.readRawData(reinterpret_cast<char*>(vector.data()), bytes) != bytes) {
                return false;
            }
        }

        return in.status() == QDataStream::Ok;
    }

    void writeMounts(QDataStream& out, const std::vector<BlockDeviceMountInfo>& mounts)
    {
        out << static_cast<quint64>(mounts.size());

        for (const BlockDeviceMountInfo& mount : mounts) {
            out << mount.mountPoint;
            out << mount.fsType;
            out << mount.root;
            out << mount.subvolPath;
            out << mount.btrfsRootId;
        }
    }

    bool readMounts(QDataStream& in, std::vector<BlockDeviceMountInfo>& mounts)
    {
        quint64 size = 0;
        in >> size;

        if (in.status() != QDataStream::Ok || size > 4096) {
            return false;
        }

        mounts.clear();
        mounts.reserve(static_cast<std::size_t>(size));

        for (quint64 i = 0; i < size; ++i) {
            BlockDeviceMountInfo mount;
            in >> mount.mountPoint;
            in >> mount.fsType;
            in >> mount.root;
            in >> mount.subvolPath;
            in >> mount.btrfsRootId;

            if (in.status() != QDataStream::Ok) {
                return false;
            }

            mounts.push_back(std::move(mount));
        }

        return true;
    }
}

IndexPersistenceManager::IndexPersistenceManager(QObject* parent)
    : QObject(parent)
{
}

QString IndexPersistenceManager::indexDirectory() const
{
    const QString stateLocation = QStandardPaths::writableLocation(QStandardPaths::StateLocation);

    if (!stateLocation.trimmed().isEmpty()) {
        return QDir(stateLocation).filePath(QStringLiteral("indexes"));
    }

    const QString dataLocation = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(dataLocation).filePath(QStringLiteral("indexes"));
}

QString IndexPersistenceManager::fileSafeDeviceId(const QString& deviceId)
{
    const QByteArray hash = QCryptographicHash::hash(
        deviceId.toUtf8(),
        QCryptographicHash::Sha256
    ).toHex();

    return QString::fromLatin1(hash);
}

QString IndexPersistenceManager::pathForDeviceId(const QString& deviceId) const
{
    return QDir(indexDirectory()).filePath(fileSafeDeviceId(deviceId) + QStringLiteral(".kix"));
}

bool IndexPersistenceManager::saveCoreSnapshot(
    const IndexController::CoreSnapshot& snapshot,
    QString* errorText
) const {
    if (snapshot.deviceId.isEmpty()) {
        if (errorText) {
            *errorText = QStringLiteral("Device ID is empty.");
        }

        return false;
    }

    QDir dir(indexDirectory());
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        if (errorText) {
            *errorText = QStringLiteral("Could not create index directory: %1").arg(dir.absolutePath());
        }

        return false;
    }

    QSaveFile file(pathForDeviceId(snapshot.deviceId));
    if (!file.open(QIODevice::WriteOnly)) {
        if (errorText) {
            *errorText = file.errorString();
        }

        return false;
    }

    QDataStream out(&file);
    out.setVersion(QDataStream::Qt_6_0);
    out.setByteOrder(QDataStream::LittleEndian);

    out << FormatMagic;
    out << FormatVersion;

    out << snapshot.deviceId;
    out << snapshot.devNode;
    out << snapshot.fsType;
    out << snapshot.label;
    out << snapshot.mountPoints;
    out << snapshot.primaryMountPoint;
    writeMounts(out, snapshot.mounts);
    out << snapshot.mounted;
    out << snapshot.showOfflineResults;
    out << snapshot.lastIndexedTime;
    out << snapshot.lastUpdatedTime;

    writeVector(out, snapshot.fileRecords);
    writeVector(out, snapshot.fileRecordNamespaces);
    writeVector(out, snapshot.stringPool);
    writeVector(out, snapshot.deletedRecordBits);

    if (out.status() != QDataStream::Ok) {
        if (errorText) {
            *errorText = QStringLiteral("Failed while writing index data.");
        }

        return false;
    }

    if (!file.commit()) {
        if (errorText) {
            *errorText = file.errorString();
        }

        return false;
    }

    return true;
}

std::optional<IndexController::CoreSnapshot> IndexPersistenceManager::loadCoreSnapshot(
    const QString& deviceId,
    QString* errorText
) const {
    QFile file(pathForDeviceId(deviceId));
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorText) {
            *errorText = file.errorString();
        }

        return std::nullopt;
    }

    QDataStream in(&file);
    in.setVersion(QDataStream::Qt_6_0);
    in.setByteOrder(QDataStream::LittleEndian);

    quint32 magic = 0;
    quint32 version = 0;

    in >> magic;
    in >> version;

    if (magic != FormatMagic || version < 1 || version > FormatVersion) {
        if (errorText) {
            *errorText = QStringLiteral("Unsupported saved index format.");
        }

        return std::nullopt;
    }

    IndexController::CoreSnapshot snapshot;

    in >> snapshot.deviceId;
    in >> snapshot.devNode;
    in >> snapshot.fsType;
    in >> snapshot.label;
    in >> snapshot.mountPoints;
    in >> snapshot.primaryMountPoint;

    if (!readMounts(in, snapshot.mounts)) {
        if (errorText) {
            *errorText = QStringLiteral("Could not read mount metadata.");
        }

        return std::nullopt;
    }

    in >> snapshot.mounted;
    in >> snapshot.showOfflineResults;
    in >> snapshot.lastIndexedTime;
    in >> snapshot.lastUpdatedTime;

    static constexpr quint64 MaxRecords = 100'000'000;
    static constexpr quint64 MaxStringBytes = 16ull * 1024ull * 1024ull * 1024ull;
    static constexpr quint64 MaxDeletedWords = (MaxRecords + 63) / 64;

    if (!readVector(in, snapshot.fileRecords, MaxRecords) ||
        !readVector(in, snapshot.fileRecordNamespaces, MaxRecords) ||
        !readVector(in, snapshot.stringPool, MaxStringBytes) ||
        !readVector(in, snapshot.deletedRecordBits, MaxDeletedWords)) {
        if (errorText) {
            *errorText = QStringLiteral("Saved index is incomplete or invalid.");
        }

        return std::nullopt;
    }

    if (snapshot.deviceId != deviceId) {
        if (errorText) {
            *errorText = QStringLiteral("Saved index belongs to a different device.");
        }

        return std::nullopt;
    }

    if (!snapshot.fileRecordNamespaces.empty() &&
        snapshot.fileRecordNamespaces.size() != snapshot.fileRecords.size()) {
        if (errorText) {
            *errorText = QStringLiteral("Saved index namespace sidecar count does not match records.");
        }

        return std::nullopt;
    }

    return snapshot;
}

bool IndexPersistenceManager::removeSavedIndex(const QString& deviceId, QString* errorText) const
{
    QFile file(pathForDeviceId(deviceId));

    if (!file.exists()) {
        return true;
    }

    if (!file.remove()) {
        if (errorText) {
            *errorText = file.errorString();
        }

        return false;
    }

    return true;
}

bool IndexPersistenceManager::hasSavedIndex(const QString& deviceId) const
{
    return QFile::exists(pathForDeviceId(deviceId));
}

IndexPersistenceManager::SavedIndexInfo IndexPersistenceManager::savedIndexInfo(
    const QString& deviceId
) const {
    const QFileInfo fileInfo(pathForDeviceId(deviceId));

    if (!fileInfo.exists() || !fileInfo.isFile()) {
        return {};
    }

    return SavedIndexInfo{
        .available = true,
        .sizeBytes = fileInfo.size(),
        .modifiedAt = fileInfo.lastModified(),
    };
}