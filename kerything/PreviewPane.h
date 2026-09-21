// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  derickso <https://github.com/derickso>

#ifndef KERYTHING_PREVIEWPANE_H
#define KERYTHING_PREVIEWPANE_H

#include <QCache>
#include <QFileInfo>
#include <QFrame>
#include <QLabel>
#include <QPixmap>
#include <QPointer>
#include <QProcess>
#include <QTimer>
#include <QUrl>
#include <QWidget>

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

    void previewUrl(const QUrl& url);
    void showUnmounted();
    void clearPreview(const QString& placeholder = QStringLiteral("No item selected"));

private Q_SLOTS:
    void onDebounceTimeout();

private:
    struct PreviewCacheEntry {
        QPixmap pixmap;
        QString metadataText;
        bool isIcon = false;
    };

    void setPreviewContent(const QPixmap& pixmap, const QString& metadataText);
    QString generateMetadataHtml(const QFileInfo& fileInfo) const;
    void generateFallbackOrIcon(const QUrl& url, const QString& meta);
    void generateVideoThumbnail(const QUrl& url, const QString& meta);
    void showThemeIcon(const QUrl& url, const QString& meta);
    void cancelCurrentJob();
    static bool isImageFile(const QFileInfo& fileInfo);
    static bool isVideoFile(const QFileInfo& fileInfo);
    static bool userWantsFilmstrip();

    QLabel* titleLabel_ = nullptr;
    PreviewImageWidget* previewImageWidget_ = nullptr;
    QLabel* metadataLabel_ = nullptr;

    QTimer debounceTimer_;
    QUrl currentUrl_;
    QString currentMetadataText_;
    bool isThemeIcon_ = false;

    QCache<QString, PreviewCacheEntry> memoryCache_;

#ifdef KERYTHING_WITH_KF6
    QPointer<KIO::PreviewJob> currentJob_;
#endif
    QPointer<QProcess> currentProcess_;
};

#endif // KERYTHING_PREVIEWPANE_H