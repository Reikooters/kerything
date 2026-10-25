#ifndef KERYTHING_INDEXSUMMARY_H
#define KERYTHING_INDEXSUMMARY_H

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <type_traits>

enum class IndexStaleReason : quint32 {
    None = 0,
    LoadedFromDiskNeedsRefresh = 1u << 0,
    LiveUpdatesDisabled = 1u << 1,
    DaemonDisconnected = 1u << 2,
    LiveUpdateFailure = 1u << 3,
};

constexpr IndexStaleReason operator|(IndexStaleReason lhs, IndexStaleReason rhs) noexcept
{
    using Underlying = std::underlying_type_t<IndexStaleReason>;
    return static_cast<IndexStaleReason>(
        static_cast<Underlying>(lhs) | static_cast<Underlying>(rhs)
    );
}

constexpr IndexStaleReason operator&(IndexStaleReason lhs, IndexStaleReason rhs) noexcept
{
    using Underlying = std::underlying_type_t<IndexStaleReason>;
    return static_cast<IndexStaleReason>(
        static_cast<Underlying>(lhs) & static_cast<Underlying>(rhs)
    );
}

constexpr IndexStaleReason& operator|=(IndexStaleReason& lhs, IndexStaleReason rhs) noexcept
{
    lhs = lhs | rhs;
    return lhs;
}

[[nodiscard]] constexpr bool hasIndexStaleReason(
    IndexStaleReason reasons,
    IndexStaleReason reason
) noexcept {
    return (reasons & reason) != IndexStaleReason::None;
}

[[nodiscard]] constexpr bool hasAnyIndexStaleReason(IndexStaleReason reasons) noexcept
{
    return reasons != IndexStaleReason::None;
}

[[nodiscard]] constexpr qsizetype indexStaleReasonCount(IndexStaleReason reasons) noexcept
{
    qsizetype count = 0;

    if (hasIndexStaleReason(reasons, IndexStaleReason::LoadedFromDiskNeedsRefresh)) {
        ++count;
    }

    if (hasIndexStaleReason(reasons, IndexStaleReason::LiveUpdatesDisabled)) {
        ++count;
    }

    if (hasIndexStaleReason(reasons, IndexStaleReason::DaemonDisconnected)) {
        ++count;
    }

    if (hasIndexStaleReason(reasons, IndexStaleReason::LiveUpdateFailure)) {
        ++count;
    }

    return count;
}

[[nodiscard]] constexpr IndexStaleReason primaryIndexStaleReason(
    IndexStaleReason reasons
) noexcept {
    if (hasIndexStaleReason(reasons, IndexStaleReason::LoadedFromDiskNeedsRefresh)) {
        return IndexStaleReason::LoadedFromDiskNeedsRefresh;
    }

    if (hasIndexStaleReason(reasons, IndexStaleReason::LiveUpdatesDisabled)) {
        return IndexStaleReason::LiveUpdatesDisabled;
    }

    if (hasIndexStaleReason(reasons, IndexStaleReason::DaemonDisconnected)) {
        return IndexStaleReason::DaemonDisconnected;
    }

    if (hasIndexStaleReason(reasons, IndexStaleReason::LiveUpdateFailure)) {
        return IndexStaleReason::LiveUpdateFailure;
    }

    return IndexStaleReason::None;
}

struct IndexSummary {
    quint64 indexId = 0;
    QString deviceId;
    QString displayName;
    QString label;
    QString devNode;
    QString fsType;
    QString primaryMountPoint;
    QStringList mountPoints;
    qint64 lastIndexedTime = 0;
    qint64 lastUpdatedTime = 0;
    qsizetype recordCount = 0;
    qsizetype deletedRecordCount = 0;
    bool ready = false;
    bool mounted = false;
    bool searchable = false;
    bool showOfflineResults = true;
    IndexStaleReason staleReasons = IndexStaleReason::None;
    bool stale = false;

    bool savedIndexAvailable = false;
    qint64 savedIndexSizeBytes = 0;
    QDateTime savedIndexModifiedAt;
};

#endif // KERYTHING_INDEXSUMMARY_H