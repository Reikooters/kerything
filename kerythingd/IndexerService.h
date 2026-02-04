// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_KERYTHINGD_INDEXERSERVICE_H
#define KERYTHING_KERYTHINGD_INDEXERSERVICE_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QList>
#include <QMap>
#include <QProcess>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QByteArray>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <QtDBus/QDBusContext>

#include "../ScannerEngine.h"

class IndexerService final : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "net.reikooters.Kerything1.Indexer")

public:
    explicit IndexerService(QObject* parent = nullptr);
    ~IndexerService() override;

public slots:
    /**
     * Provides version information about the IndexerService and its API.
     *
     * @param versionOut A reference to a QString object that will be populated with the service version string.
     * @param apiVersionOut A reference to a quint32 variable that will be populated with the API version integer.
     */
    void Ping(QString& versionOut, quint32& apiVersionOut) const;

    // Returns:
    //  devicesOut: array of dicts (a{sv}) encoded as QVariantMap, each containing:
    //    deviceId: string           (e.g. "partuuid:xxxx")
    //    devNode: string            (e.g. "/dev/nvme0n1p2")
    //    fsType: string             (e.g. "ext4", "ntfs")
    //    label: string
    //    uuid: string               (filesystem UUID, if any)
    //    partuuid: string
    //    mounted: bool
    //    mountPoints: stringlist
    //    primaryMountPoint: string
    /**
     * Retrieves a list of known devices currently detectable by the IndexerService.
     *
     * @param devicesOut A reference to a QVariantList object that will be populated with dictionaries,
     *                   each representing a detected device. Each dictionary contains information such as:
     *                   - deviceId: A string representing the stable identifier of the device (e.g., "partuuid:xxxx").
     *                   - devNode: A string representing the device node path (e.g., "/dev/nvme0n1p2").
     *                   - fsType: A string representing the file system type of the device (e.g., "ext4", "ntfs").
     *                   - label: A string representing the label of the device, if available.
     *                   - uuid: A string representing the file system UUID of the device, if any.
     *                   - partuuid: A string representing the partition UUID of the device, if any.
     *                   - mounted: A boolean indicating whether the device is currently mounted.
     *                   - mountPoints: A list of strings representing the mount points of the device.
     *                   - primaryMountPoint: A string representing the primary mount point of the device, if any.
     */
    void ListKnownDevices(QVariantList& devicesOut) const;

    // List devices that currently have an in-memory index in this daemon instance.
    // Returns:
    //  indexedOut: array of dicts (a{sv}) encoded as QVariantMap, each containing:
    //    deviceId: string
    //    fsType: string
    //    generation: uint64
    //    entryCount: uint64
    //    lastIndexedTime: int64 (unix seconds; 0 = unknown)
    //    label: string (last known label; may be stale if device not present)
    //    uuid: string  (last known filesystem UUID; may be stale if device not present)
    /**
     * Retrieves a list of devices that currently have an in-memory index in this daemon instance.
     *
     * @param indexedOut A reference to a QVariantList object that will be populated with dictionaries,
     *                   each representing an indexed device. Each dictionary contains the following keys:
     *                   - deviceId: A string representing the stable identifier of the device (e.g., "partuuid:xxxx").
     *                   - fsType: A string representing the file system type of the device (e.g., "ext4", "ntfs").
     *                   - generation: A 64-bit unsigned integer representing the generation of the index.
     *                   - entryCount: A 64-bit unsigned integer representing the number of entries in the index.
     *                   - lastIndexedTime: A 64-bit signed integer representing the last indexed time in Unix seconds (0 = unknown).
     *                   - label: A string representing the last known label of the device (may be stale if device not present).
     *                   - uuid: A string representing the last known filesystem UUID of the device (may be stale if device not present).
     */
    void ListIndexedDevices(QVariantList& indexedOut) const;

    // Start indexing a device by stable daemon deviceId (e.g. "partuuid:...")
    // Returns: jobId (uint64)
    /**
     * Initiates the indexing process for the specified device.
     *
     * @param deviceId A QString representing the unique identifier of the device to be indexed.
     * @return A quint64 that represents the unique job ID of the indexing process.
     */
    quint64 StartIndex(const QString& deviceId);

    // Cancel a previously started job
    /**
     * Cancels a running indexing job identified by its unique job ID.
     *
     * @param jobId The unique identifier of the job that should be canceled.
     */
    void CancelJob(quint64 jobId);

    // Returns:
    //  totalHitsOut: uint64
    //  rowsOut: array of rows, where each row is:
    //    [entryId:uint64, deviceId:string, name:string, dirId:uint32, size:uint64, mtime:int64, flags:uint32]
    /**
     * Searches the index based on the provided query and filters, returning the matching results along with metadata.
     *
     * @param query The search query string used to filter the records in the index.
     * @param deviceIds A list of device IDs to limit the search scope. If empty, all devices are searched.
     * @param sortKey The key used to sort the results (e.g., a field name in the records).
     * @param sortDir The direction of sorting, either "asc" for ascending or "desc" for descending.
     * @param offset The zero-based index of the first record to fetch in the result set.
     * @param limit The maximum number of records to return in the result set.
     * @param options A map containing additional optional parameters for the search query.
     * @param totalHitsOut A reference to a quint64 variable that will be populated with the total number of records matching the query.
     * @param rowsOut A reference to a QVariantList that will be populated with the rows of results matching the query.
     */
    void Search(const QString& query,
                const QStringList& deviceIds,
                const QString& sortKey,
                const QString& sortDir,
                quint32 offset,
                quint32 limit,
                const QVariantMap& options,
                quint64& totalHitsOut,
                QVariantList& rowsOut) const;

    // // Returns:
    // //  out: array of pairs:
    // //    [dirId:uint32, path:string]
    // void ResolveDirectories(const QString& deviceId,
    //                         const QList<quint32>& dirIds,
    //                         QVariantList& out) const;

    // NOTE: Use QVariantList to avoid QtDBus/libdbus "array of array" marshalling issues in the prototype.
    // dirIds is a list of uint32 values (QVariant holding int/uint).
    /**
     * Resolves directory paths for a given device and a list of directory IDs.
     * For each directory ID, the method retrieves the corresponding directory path
     * and provides a list of ID-path pairs as output.
     *
     * @param deviceId The unique identifier of the device for which the directories should be resolved.
     * @param dirIds A list of directory IDs to be resolved.
     * @param out A reference to a QVariantList that will be populated with resolved ID-path pairs.
     *            Each pair is represented as a QVariantList containing the directory ID and its corresponding path.
     */
    void ResolveDirectories(const QString& deviceId,
                            const QVariantList& dirIds,
                            QVariantList& out) const;

    /**
     * Resolve one or more search result entries to usable paths + mount state.
     *
     * entryIds: list of uint64 values (QVariant holding ulonglong).
     *
     * out: array of dicts (a{sv}) encoded as QVariantMap, each containing:
     *  entryId: uint64
     *  deviceId: string
     *  name: string
     *  isDir: bool
     *  mounted: bool
     *  primaryMountPoint: string
     *  internalPath: string  (e.g. "/foo/bar.txt")
     *  displayPath: string   (e.g. "/mnt/Data/foo/bar.txt" or "[Label]/foo/bar.txt")
     *  internalDir: string   (e.g. "/foo")
     *  displayDir: string    (e.g. "/mnt/Data/foo" or "[Label]/foo")
     */
    void ResolveEntries(const QVariantList& entryIds,
                        QVariantList& out) const;

    /**
     * Removes an index for the given deviceId for the calling user:
     * - drops in-memory index
     * - deletes persisted snapshot (if any)
     *
     * If an indexing job is currently running for the same uid+deviceId, this method fails.
     */
    void ForgetIndex(const QString& deviceId);

signals:
    void JobAdded(quint64 jobId, const QVariantMap& props);
    void JobProgress(quint64 jobId, quint32 percent, const QVariantMap& props);
    void JobFinished(quint64 jobId, const QString& status, const QString& message, const QVariantMap& props);

    // emitted when a device index is updated in memory
    void DeviceIndexUpdated(const QString& deviceId, quint64 generation, quint64 entryCount);

    // emitted when a device index is removed for this uid
    void DeviceIndexRemoved(const QString& deviceId);

private:
    struct DeviceIndex {
        QString fsType;
        quint64 generation = 0;

        // unix seconds; 0 means unknown (e.g. loaded from older snapshot)
        qint64 lastIndexedTime = 0;

        // “Last known” display metadata (may be stale if device not present)
        QString labelLastKnown;
        QString uuidLastKnown;

        std::vector<ScannerEngine::FileRecord> records;
        std::vector<char> stringPool;

        // Search acceleration
        std::vector<ScannerEngine::TrigramEntry> flatIndex; // sorted by (trigram, recordIdx)

        // Precomputed sort orders (ascending)
        std::vector<quint32> orderByName;
        std::vector<quint32> orderByPath;
        std::vector<quint32> orderBySize;
        std::vector<quint32> orderByMtime;

        // Inverse of orderBy* : recordIdx -> rank (ascending position)
        std::vector<quint32> rankByName;
        std::vector<quint32> rankByPath;
        std::vector<quint32> rankBySize;
        std::vector<quint32> rankByMtime;

        // dirId (record index) -> full directory path
        mutable std::unordered_map<quint32, QString> dirPathCache;
    };

    struct Job {
        enum class State : quint8 { Running, Cancelling };

        quint64 jobId = 0;
        quint32 ownerUid = 0;

        QString deviceId;
        QString devNode;
        QString fsType;

        State state = State::Running;

        QProcess* proc = nullptr;
        QByteArray stderrBuf;
        QByteArray stdoutBuf;
        int lastPct = -1;
    };

    struct ParsedScan {
        std::vector<ScannerEngine::FileRecord> records;
        std::vector<char> stringPool;
        QString error;
    };

    [[nodiscard]] quint32 callerUidOr0() const;

    void ensureLoadedForUid(quint32 uid) const;
    void loadSnapshotsForUid(quint32 uid) const;

    [[nodiscard]] static QString baseIndexDirForUid(quint32 uid);
    [[nodiscard]] static QString snapshotPathFor(quint32 uid, const QString& deviceId);
    [[nodiscard]] static QString escapeDeviceIdForFilename(const QString& deviceId);

    bool saveSnapshot(quint32 uid, const QString& deviceId, const DeviceIndex& idx, QString* errorOut = nullptr) const;
    [[nodiscard]] std::optional<DeviceIndex> loadSnapshotFile(const QString& path, QString* deviceIdOut, QString* errorOut = nullptr) const;

    [[nodiscard]] std::optional<QVariantMap> findDeviceById(const QString& deviceId) const;

    [[nodiscard]] static quint64 makeEntryId(const QString& deviceId, quint32 recordIdx);

    [[nodiscard]] static quint32 deviceHash32(const QString& deviceId);
    [[nodiscard]] static QString joinInternalPath(const QString& internalDir, const QString& name);

    [[nodiscard]] static bool nameContainsCaseInsensitive(std::string_view haystack, std::string_view needle);

    [[nodiscard]] static ParsedScan parseHelperStdout(const QByteArray& raw);

    // Build acceleration structures
    static void buildTrigramIndex(DeviceIndex& idx);
    static void buildSortOrders(DeviceIndex& idx);

    [[nodiscard]] QString dirPathFor(quint32 uid, const QString& deviceId, quint32 dirId) const;

    // Helpers for fast searching
    static QStringList tokenizeQuery(const QString& query);
    static std::vector<quint32> deviceCandidatesForQuery(const DeviceIndex& idx, const QStringList& tokens);

    std::unordered_map<quint64, std::unique_ptr<Job>> m_jobs;
    quint64 m_nextJobId = 1;

    // uid -> (deviceId -> in-memory index)
    mutable std::unordered_map<quint32, std::unordered_map<QString, DeviceIndex>> m_indexesByUid;
    mutable std::unordered_set<quint32> m_loadedUids;
};

#endif //KERYTHING_KERYTHINGD_INDEXERSERVICE_H