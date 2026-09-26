// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  derickso <https://github.com/derickso>

#ifndef KERYTHING_PREVIEWPANE_H
#define KERYTHING_PREVIEWPANE_H

#include <QCache>
#include <QFileInfo>
#include <QFrame>
#include <QFutureWatcher>
#include <QLabel>
#include <QMimeDatabase>
#include <QPixmap>
#include <QPointer>
#include <QProcess>
#include <QTimer>
#include <QUrl>
#include <QWidget>

#include "PreviewMetadata.h"

#ifdef KERYTHING_WITH_KF6
#include <KFileItem>
#include <KIO/PreviewJob>
#endif

class PreviewImageWidget final : public QWidget {
    Q_OBJECT
public:
    explicit PreviewImageWidget(QWidget* parent = nullptr);
    void setPixmap(const QPixmap& pixmap, bool isIcon = false);
    void setText(const QString& text);
    void clear();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QPixmap pixmap_;
    QString text_;
    bool isIcon_ = false;
};

class PreviewPane final : public QFrame {
    Q_OBJECT

public:
    explicit PreviewPane(QWidget* parent = nullptr);
    ~PreviewPane() override;

    void previewUrl(
        const QUrl& url,
        const std::optional<PreviewMetadata>& indexedMetadata = std::nullopt
    );
    void showUnmounted();
    void clearPreview(const QString& placeholder = QStringLiteral("No item selected"));

private Q_SLOTS:
    void onDebounceTimeout();

private:
    struct PreviewCacheEntry {
        QPixmap pixmap;
        QSize dimensions;
        bool isIcon = false;
    };

    struct ImageLoadResult {
        QUrl url;
        QString cacheKey;
        QString metadataText;
        QImage image;
        QSize dimensions;
        quint64 generation = 0;
    };

    static const QMimeDatabase& mimeDatabase();
    void setPreviewContent(const QPixmap& pixmap, const QString& metadataText, bool isIcon);
    static QString generateMetadataHtml(const QFileInfo& fileInfo);
    static QString generateMetadataHtml(
        const QFileInfo& fileInfo,
        const std::optional<PreviewMetadata>& indexedMetadata = std::nullopt
    );
    static QString generateMetadataHtmlWithOptionalDimensions(
        const QFileInfo& fileInfo,
        const std::optional<QString>& dimensionsText,
        const std::optional<PreviewMetadata>& indexedMetadata = std::nullopt
    );
    static QString dimensionsTextForSize(const QSize& dimensions);
    static QString metadataHtmlWithDimensions(const QString& metadataText, const QSize& dimensions);
    static QString metadataHtmlWithDimensionsPlaceholder(const QString& metadataText);
    static QString metadataHtmlWithDimensionsUnknown(const QString& metadataText);
    static QString metadataHtmlWithDimensionsText(const QString& metadataText, const QString& dimensionsText);
    void generateFallbackOrIcon(const QUrl& url, const QString& meta, quint64 generation);
    void generateVideoThumbnail(const QUrl& url, const QString& meta, quint64 generation);
    void generateAudioThumbnail(const QUrl& url, const QString& meta, quint64 generation);
    void runExternalThumbnailCommand(
        const QUrl& url,
        const QString& meta,
        quint64 generation,
        const QString& program,
        const QStringList& args
    );
    void showThemeIcon(const QUrl& url, const QString& meta, quint64 generation);
    void cancelCurrentJob();
    void cancelImageLoad();
    void cachePreview(const QString& cacheKey, const QPixmap& pixmap, bool isIcon, const QSize& dimensions = {});
    QString previewCacheKey(const QUrl& url) const;
    int previewTargetWidth() const;
    static int pixmapCacheCostKiB(const QPixmap& pixmap);
    static QSize imageDimensions(const QUrl& url);
    static bool isImageUrl(const QUrl& url);
    static bool isImageFile(const QFileInfo& fileInfo);
    static bool isVideoFile(const QFileInfo& fileInfo);
    static bool isAudioFile(const QFileInfo& fileInfo);

    QLabel* titleLabel_ = nullptr;
    PreviewImageWidget* previewImageWidget_ = nullptr;
    QLabel* metadataLabel_ = nullptr;

    QTimer debounceTimer_;
    QUrl currentUrl_;
    QString currentMetadataText_;
    std::optional<PreviewMetadata> currentIndexedMetadata_;
    quint64 previewGeneration_ = 0;

    QCache<QString, PreviewCacheEntry> memoryCache_;

#ifdef KERYTHING_WITH_KF6
    QPointer<KIO::PreviewJob> currentJob_;
#endif
    QPointer<QProcess> currentProcess_;
    QPointer<QFutureWatcher<ImageLoadResult>> imageLoadWatcher_;
};

#endif // KERYTHING_PREVIEWPANE_H