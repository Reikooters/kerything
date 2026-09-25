// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  derickso <https://github.com/derickso>

#include "PreviewPane.h"

#include <algorithm>
#include <memory>

#include <QDateTime>
#include <QFileInfo>
#include <QIcon>
#include <QImage>
#include <QImageReader>
#include <QLocale>
#include <QMimeDatabase>
#include <QMimeType>
#include <QPaintEvent>
#include <QPainter>
#include <QSet>
#include <QSizePolicy>
#include <QStandardPaths>
#include <QtConcurrent>
#include <QVBoxLayout>

// --- PreviewImageWidget (Dolphin-style custom paint viewer) ---

PreviewImageWidget::PreviewImageWidget(QWidget* parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumSize(100, 100);
}

void PreviewImageWidget::setPixmap(const QPixmap& pixmap, bool isIcon)
{
    text_.clear();
    pixmap_ = pixmap;
    isIcon_ = isIcon;
    update();
}

void PreviewImageWidget::setText(const QString& text)
{
    pixmap_ = QPixmap();
    text_ = text;
    isIcon_ = false;
    update();
}

void PreviewImageWidget::clear()
{
    pixmap_ = QPixmap();
    text_.clear();
    isIcon_ = false;
    update();
}

void PreviewImageWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    QPainter painter(this);

    if (!text_.isEmpty() && pixmap_.isNull()) {
        painter.setPen(palette().color(QPalette::PlaceholderText));
        painter.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap, text_);
        return;
    }

    if (pixmap_.isNull()) {
        return;
    }

    const QSize baseSize = pixmap_.deviceIndependentSize().toSize();
    if (baseSize.isEmpty()) {
        return;
    }

    QSize targetSize;
    if (isIcon_) {
        // Fallback file type icons stay at their crisp native resolution
        targetSize = baseSize;
        if (targetSize.width() > width() || targetSize.height() > height()) {
            targetSize = targetSize.scaled(size(), Qt::KeepAspectRatio);
        }
    } else {
        // Previews scale to fill the available area maintaining aspect ratio
        targetSize = baseSize.scaled(size(), Qt::KeepAspectRatio);
    }

    // Centered horizontally and vertically
    const int x = (width() - targetSize.width()) / 2;
    const int y = (height() - targetSize.height()) / 2;
    const QRect targetRect(x, y, targetSize.width(), targetSize.height());

    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawPixmap(targetRect, pixmap_);
}

// --- PreviewPane ---

const QMimeDatabase& PreviewPane::mimeDatabase()
{
    static const QMimeDatabase database;
    return database;
}

bool PreviewPane::isImageFile(const QFileInfo& fileInfo)
{
    static const QSet<QString> imageExtensions = {
        QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("png"),
        QStringLiteral("webp"), QStringLiteral("bmp"), QStringLiteral("gif"),
        QStringLiteral("svg"), QStringLiteral("svgz"), QStringLiteral("tif"),
        QStringLiteral("tiff"), QStringLiteral("ico"), QStringLiteral("avif"),
        QStringLiteral("heic"), QStringLiteral("heif"), QStringLiteral("jxl"),
        QStringLiteral("qoi"), QStringLiteral("tga"), QStringLiteral("ppm"),
        QStringLiteral("cr2"), QStringLiteral("nef"), QStringLiteral("arw"),
        QStringLiteral("dng")
    };

    if (imageExtensions.contains(fileInfo.suffix().toLower())) {
        return true;
    }

    const QMimeType mime = mimeDatabase().mimeTypeForFile(fileInfo);
    return mime.name().startsWith(QStringLiteral("image/"));
}

bool PreviewPane::isVideoFile(const QFileInfo& fileInfo)
{
    static const QSet<QString> videoExtensions = {
        QStringLiteral("mp4"), QStringLiteral("mkv"), QStringLiteral("avi"),
        QStringLiteral("mov"), QStringLiteral("wmv"), QStringLiteral("flv"),
        QStringLiteral("webm"), QStringLiteral("m4v"), QStringLiteral("mpg"),
        QStringLiteral("mpeg"), QStringLiteral("m2v"), QStringLiteral("3gp"),
        QStringLiteral("3g2"), QStringLiteral("ts"), QStringLiteral("mts"),
        QStringLiteral("m2ts"), QStringLiteral("ogv"), QStringLiteral("vob")
    };

    if (videoExtensions.contains(fileInfo.suffix().toLower())) {
        return true;
    }

    const QMimeType mime = mimeDatabase().mimeTypeForFile(fileInfo);
    return mime.name().startsWith(QStringLiteral("video/"));
}

PreviewPane::PreviewPane(QWidget* parent)
    : QFrame(parent)
{
    setFrameStyle(QFrame::NoFrame);
    setMinimumWidth(220);

    memoryCache_.setMaxCost(64 * 1024);

    debounceTimer_.setSingleShot(true);
    debounceTimer_.setInterval(150);
    connect(&debounceTimer_, &QTimer::timeout, this, &PreviewPane::onDebounceTimeout);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(8, 8, 8, 8);
    rootLayout->setSpacing(8);

    // Title label: spans full width and word-wraps long unbroken names
    titleLabel_ = new QLabel(QStringLiteral("Preview"), this);
    QFont titleFont = titleLabel_->font();
    titleFont.setBold(true);
    titleLabel_->setFont(titleFont);
    titleLabel_->setWordWrap(true);
    titleLabel_->setTextFormat(Qt::RichText);
    titleLabel_->setTextInteractionFlags(Qt::NoTextInteraction);
    rootLayout->addWidget(titleLabel_, 0);

    // Custom Dolphin-style preview viewer widget
    previewImageWidget_ = new PreviewImageWidget(this);
    rootLayout->addWidget(previewImageWidget_, 1);

    // Separator line
    auto* separator = new QFrame(this);
    separator->setFrameShape(QFrame::HLine);
    separator->setFrameShadow(QFrame::Sunken);
    rootLayout->addWidget(separator);

    // Metadata details label
    metadataLabel_ = new QLabel(this);
    metadataLabel_->setWordWrap(true);
    metadataLabel_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    metadataLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    metadataLabel_->setContentsMargins(0, 0, 0, 0);
    rootLayout->addWidget(metadataLabel_, 0);

    clearPreview();
}

PreviewPane::~PreviewPane()
{
    cancelCurrentJob();
    cancelImageLoad();
    debounceTimer_.stop();
    memoryCache_.clear();
}

void PreviewPane::clearPreview(const QString& placeholder)
{
    ++previewGeneration_;

    cancelCurrentJob();
    cancelImageLoad();
    debounceTimer_.stop();
    currentUrl_.clear();
    currentMetadataText_.clear();
    currentIndexedMetadata_.reset();

    titleLabel_->setText(QStringLiteral("<span style='word-break: break-all;'>Preview</span>"));
    titleLabel_->setToolTip(QString());

    previewImageWidget_->setText(placeholder);
    metadataLabel_->clear();
}

void PreviewPane::showUnmounted()
{
    ++previewGeneration_;

    cancelCurrentJob();
    cancelImageLoad();
    debounceTimer_.stop();
    currentUrl_.clear();
    currentMetadataText_.clear();
    currentIndexedMetadata_.reset();

    titleLabel_->setText(QStringLiteral("<span style='word-break: break-all;'>Preview (Unmounted)</span>"));
    titleLabel_->setToolTip(QString());

    previewImageWidget_->setText(
        QStringLiteral("This item is on an unmounted device.\nMount the device to view previews.")
    );
    metadataLabel_->clear();
}

void PreviewPane::previewUrl(
    const QUrl& url,
    const std::optional<PreviewMetadata>& indexedMetadata
)
{
    if (currentUrl_ == url && (debounceTimer_.isActive() || currentProcess_ || imageLoadWatcher_)) {
        return;
    }

    ++previewGeneration_;

    cancelCurrentJob();
    cancelImageLoad();
    debounceTimer_.stop();

    currentUrl_ = url;
    currentIndexedMetadata_ = indexedMetadata;

    const QString localFilePath = url.toLocalFile();
    const QFileInfo fileInfo(localFilePath);

    const QString titleText =
        indexedMetadata && !indexedMetadata->fileName.isEmpty()
            ? indexedMetadata->fileName
            : fileInfo.fileName();

    const QString toolTipText =
        indexedMetadata && !indexedMetadata->displayPath.isEmpty()
            ? indexedMetadata->displayPath
            : fileInfo.absoluteFilePath();

    titleLabel_->setText(QStringLiteral("<span style='word-break: break-all;'>%1</span>")
        .arg(titleText.toHtmlEscaped()));
    titleLabel_->setToolTip(toolTipText);

    QString metaText = generateMetadataHtml(fileInfo, currentIndexedMetadata_);
    const bool indexedDirectory =
        currentIndexedMetadata_ && currentIndexedMetadata_->isDirectory;

    if (!indexedDirectory && fileInfo.isFile() && isImageFile(fileInfo)) {
        metaText = metadataHtmlWithDimensionsPlaceholder(metaText);
    }

    currentMetadataText_ = metaText;
    metadataLabel_->setText(metaText);

    const QString cacheKey = previewCacheKey(url);

    // Fast in-memory cache hit
    if (PreviewCacheEntry* cached = memoryCache_.object(cacheKey)) {
        const QString cachedMetadataText = isImageUrl(url)
            ? metadataHtmlWithDimensions(metaText, cached->dimensions)
            : metaText;

        setPreviewContent(cached->pixmap, cachedMetadataText, cached->isIcon);
        return;
    }

    previewImageWidget_->setText(QStringLiteral("Loading preview..."));

    debounceTimer_.start();
}

void PreviewPane::onDebounceTimeout()
{
    if (!currentUrl_.isValid()) {
        return;
    }

    const quint64 generation = previewGeneration_;

#ifdef KERYTHING_WITH_KF6
    cancelCurrentJob();

    KFileItemList items;
    items.append(KFileItem(currentUrl_));

    const QStringList plugins = KIO::PreviewJob::availablePlugins();

    // Request high-resolution preview matching pane width and screen DPI
    const int reqWidth = previewTargetWidth();
    const QSize targetSize(reqWidth, reqWidth);

    auto* job = KIO::filePreview(items, targetSize, &plugins);
    job->setScaleType(KIO::PreviewJob::ScaledAndCached);
    job->setDevicePixelRatio(devicePixelRatioF());
    currentJob_ = job;

    connect(job, &KIO::PreviewJob::gotPreview, this,
            [this, url = currentUrl_, meta = currentMetadataText_, generation](const KFileItem& /*item*/, const QPixmap& preview) {
                if (generation != previewGeneration_ || currentUrl_ != url) {
                    return;
                }

                if (!preview.isNull()) {
                    const bool sourceIsImage = isImageUrl(url);
                    const QSize dimensions = sourceIsImage ? imageDimensions(url) : QSize();
                    const QString metadataText = sourceIsImage
                        ? metadataHtmlWithDimensions(meta, dimensions)
                        : meta;
                    const QString cacheKey = previewCacheKey(url);

                    cachePreview(cacheKey, preview, false, dimensions);
                    setPreviewContent(preview, metadataText, false);
                }
            });

    connect(job, &KIO::PreviewJob::failed, this,
            [this, url = currentUrl_, meta = currentMetadataText_, generation](const KFileItem& /*item*/) {
                if (generation != previewGeneration_ || currentUrl_ != url) {
                    return;
                }

                generateFallbackOrIcon(url, meta, generation);
            });
#else
    generateFallbackOrIcon(currentUrl_, currentMetadataText_, generation);
#endif
}

void PreviewPane::generateFallbackOrIcon(const QUrl& url, const QString& meta, quint64 generation)
{
    if (generation != previewGeneration_ || currentUrl_ != url) {
        return;
    }

    const QString localPath = url.toLocalFile();
    const QFileInfo fileInfo(localPath);

    if ((currentIndexedMetadata_ && currentIndexedMetadata_->isDirectory) || fileInfo.isDir()) {
        showThemeIcon(url, meta, generation);
        return;
    }

    if (isVideoFile(fileInfo)) {
        generateVideoThumbnail(url, meta, generation);
        return;
    }

    if (isImageFile(fileInfo)) {
        cancelImageLoad();

        const QString cacheKey = previewCacheKey(url);
        const int targetW = previewTargetWidth();

        auto* watcher = new QFutureWatcher<ImageLoadResult>(this);
        imageLoadWatcher_ = watcher;

        connect(watcher, &QFutureWatcher<ImageLoadResult>::finished, this, [this, watcher]() {
            const ImageLoadResult result = watcher->result();

            if (imageLoadWatcher_ == watcher) {
                imageLoadWatcher_ = nullptr;
            }

            watcher->deleteLater();

            if (result.generation != previewGeneration_ || currentUrl_ != result.url) {
                return;
            }

            if (!result.image.isNull()) {
                const QPixmap pixmap = QPixmap::fromImage(result.image);
                const QString metadataText = metadataHtmlWithDimensions(result.metadataText, result.dimensions);

                cachePreview(result.cacheKey, pixmap, false, result.dimensions);
                setPreviewContent(pixmap, metadataText, false);
                return;
            }

            showThemeIcon(result.url, metadataHtmlWithDimensionsUnknown(result.metadataText), result.generation);
        });

        watcher->setFuture(QtConcurrent::run([url, localPath, cacheKey, meta, targetW, generation]() {
            QImageReader reader(localPath);
            reader.setAutoTransform(true);
            reader.setDecideFormatFromContent(true);

            const QSize origSize = reader.size();
            if (origSize.isValid()) {
                reader.setScaledSize(origSize.scaled(QSize(targetW, targetW), Qt::KeepAspectRatio));
            }

            return ImageLoadResult{
                .url = url,
                .cacheKey = cacheKey,
                .metadataText = meta,
                .image = reader.read(),
                .dimensions = origSize,
                .generation = generation
            };
        }));

        return;
    }

    showThemeIcon(url, meta, generation);
}

void PreviewPane::generateVideoThumbnail(const QUrl& url, const QString& meta, quint64 generation)
{
    if (generation != previewGeneration_ || currentUrl_ != url) {
        return;
    }

    const QString localPath = url.toLocalFile();
    const int thumbRes = previewTargetWidth();

    QString program = QStandardPaths::findExecutable(QStringLiteral("ffmpegthumbnailer"));
    QStringList args;

    if (!program.isEmpty()) {
        args << QStringLiteral("-i") << localPath
             << QStringLiteral("-o") << QStringLiteral("/dev/stdout")
             << QStringLiteral("-s") << QString::number(thumbRes)
             << QStringLiteral("-c") << QStringLiteral("png");
    } else {
        program = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
        if (!program.isEmpty()) {
            args << QStringLiteral("-loglevel") << QStringLiteral("error")
                 << QStringLiteral("-ss") << QStringLiteral("00:00:01")
                 << QStringLiteral("-i") << localPath
                 << QStringLiteral("-vframes") << QStringLiteral("1")
                 << QStringLiteral("-vf") << QStringLiteral("scale=%1:-1:force_original_aspect_ratio=decrease").arg(thumbRes)
                 << QStringLiteral("-c:v") << QStringLiteral("png")
                 << QStringLiteral("-f") << QStringLiteral("image2pipe")
                 << QStringLiteral("pipe:1");
        }
    }

    if (program.isEmpty()) {
        showThemeIcon(url, meta, generation);
        return;
    }

    cancelCurrentJob();

    auto* proc = new QProcess(this);
    currentProcess_ = proc;

    auto* timeout = new QTimer(proc);
    timeout->setSingleShot(true);
    timeout->setInterval(10000);

    auto cleanedUp = std::make_shared<bool>(false);
    auto timedOut = std::make_shared<bool>(false);
    auto stderrTail = std::make_shared<QByteArray>();

    auto cleanupProcess = [this, proc, timeout, cleanedUp]() {
        if (*cleanedUp) {
            return false;
        }

        *cleanedUp = true;

        timeout->stop();

        if (currentProcess_ == proc) {
            currentProcess_ = nullptr;
        }

        proc->deleteLater();
        return true;
    };

    auto drainStderr = [proc, stderrTail]() {
        stderrTail->append(proc->readAllStandardError());

        static constexpr qsizetype MaxStderrBytes = 16 * 1024;
        if (stderrTail->size() > MaxStderrBytes) {
            stderrTail->remove(0, stderrTail->size() - MaxStderrBytes);
        }
    };

    connect(timeout, &QTimer::timeout, this,
            [proc, timedOut, drainStderr]() {
                if (proc->state() == QProcess::NotRunning) {
                    return;
                }

                *timedOut = true;
                drainStderr();
                proc->terminate();

                QTimer::singleShot(750, proc, [proc]() {
                    if (proc->state() != QProcess::NotRunning) {
                        proc->kill();
                    }
                });
            });

    connect(proc, &QProcess::readyReadStandardError, this, drainStderr);

    connect(proc, &QProcess::finished, this,
            [this, proc, url, meta, generation, cleanupProcess, drainStderr, timedOut](int exitCode, QProcess::ExitStatus exitStatus) {
                const QByteArray data = proc->readAllStandardOutput();
                drainStderr();

                if (!cleanupProcess()) {
                    return;
                }

                if (generation != previewGeneration_ || currentUrl_ != url) {
                    return;
                }

                if (!*timedOut &&
                    exitCode == 0 &&
                    exitStatus == QProcess::NormalExit &&
                    !data.isEmpty()) {
                    QImage img;
                    if (img.loadFromData(data, "PNG")) {
                        const QPixmap pixmap = QPixmap::fromImage(img);
                        cachePreview(previewCacheKey(url), pixmap, false);
                        setPreviewContent(pixmap, meta, false);
                        return;
                    }
                }

                showThemeIcon(url, meta, generation);
            });

    connect(proc, &QProcess::errorOccurred, this,
            [this, proc, url, meta, generation, cleanupProcess, drainStderr](QProcess::ProcessError) {
                if (currentProcess_ != proc) {
                    return;
                }

                drainStderr();

                if (!cleanupProcess()) {
                    return;
                }

                if (generation == previewGeneration_ && currentUrl_ == url) {
                    showThemeIcon(url, meta, generation);
                }
            });

    proc->start(program, args);
    timeout->start();
}

void PreviewPane::showThemeIcon(const QUrl& url, const QString& meta, quint64 generation)
{
    if (generation != previewGeneration_ || currentUrl_ != url) {
        return;
    }

    const QString localPath = url.toLocalFile();
    const QFileInfo fileInfo(localPath);

    const QMimeType mimeType = mimeDatabase().mimeTypeForFile(fileInfo);
    QIcon icon = QIcon::fromTheme(mimeType.iconName());
    if (icon.isNull()) {
        icon = QIcon::fromTheme(mimeType.genericIconName());
    }
    if (icon.isNull()) {
        icon = QIcon::fromTheme(QStringLiteral("unknown"));
    }

    const qreal dpr = devicePixelRatioF();
    const int logicalIconSize = 128;
    QPixmap pixmap = icon.pixmap(
        QSize(
            static_cast<int>(logicalIconSize * dpr),
            static_cast<int>(logicalIconSize * dpr)
        )
    );
    pixmap.setDevicePixelRatio(dpr);

    if (!pixmap.isNull()) {
        cachePreview(previewCacheKey(url), pixmap, true);
        setPreviewContent(pixmap, meta, true);
        return;
    }

    previewImageWidget_->setText(QStringLiteral("No preview available"));
    metadataLabel_->setText(meta);
}

QString PreviewPane::generateMetadataHtml(const QFileInfo& fileInfo)
{
    return generateMetadataHtmlWithOptionalDimensions(fileInfo, std::nullopt, std::nullopt);
}

QString PreviewPane::generateMetadataHtml(
    const QFileInfo& fileInfo,
    const std::optional<PreviewMetadata>& indexedMetadata
)
{
    return generateMetadataHtmlWithOptionalDimensions(fileInfo, std::nullopt, indexedMetadata);
}

QString PreviewPane::generateMetadataHtmlWithOptionalDimensions(
    const QFileInfo& fileInfo,
    const std::optional<QString>& dimensionsText,
    const std::optional<PreviewMetadata>& indexedMetadata
) {
    QString typeStr;
    QString sizeStr;
    QString dateStr;
    QString pathStr;

    const QLocale locale;

    if (indexedMetadata) {
        if (indexedMetadata->isDirectory && indexedMetadata->isSymlink) {
            typeStr = QStringLiteral("Folder symlink");
        } else if (indexedMetadata->isDirectory) {
            typeStr = QStringLiteral("Folder");
        } else if (indexedMetadata->isSymlink) {
            typeStr = QStringLiteral("File symlink");
        } else {
            const QMimeType mime = mimeDatabase().mimeTypeForFile(
                indexedMetadata->fileName,
                QMimeDatabase::MatchExtension
            );
            typeStr = mime.comment().isEmpty() ? mime.name() : mime.comment();
        }

        sizeStr = indexedMetadata->isDirectory
            ? QStringLiteral("—")
            : locale.formattedDataSize(
                static_cast<qint64>(indexedMetadata->size),
                1,
                QLocale::DataSizeIecFormat
            );

        dateStr = indexedMetadata->modificationTime > 0
            ? QDateTime::fromSecsSinceEpoch(static_cast<qint64>(indexedMetadata->modificationTime))
                .toString(QStringLiteral("yyyy-MM-dd hh:mm:ss"))
            : QStringLiteral("Unknown");

        pathStr = indexedMetadata->displayPath.isEmpty()
            ? fileInfo.absoluteFilePath()
            : indexedMetadata->displayPath;
    } else {
        const QMimeType mime = mimeDatabase().mimeTypeForFile(fileInfo);
        typeStr = mime.comment().isEmpty() ? mime.name() : mime.comment();

        sizeStr = fileInfo.isDir()
            ? QStringLiteral("—")
            : locale.formattedDataSize(fileInfo.size(), 1, QLocale::DataSizeIecFormat);

        dateStr = fileInfo.lastModified().isValid()
            ? fileInfo.lastModified().toString(QStringLiteral("yyyy-MM-dd hh:mm:ss"))
            : QStringLiteral("Unknown");

        pathStr = fileInfo.absoluteFilePath();
    }

    QString html = QStringLiteral(
        "<table cellspacing='0' cellpadding='2' style='font-size: 9pt;'>"
        "<tr><td style='padding-right: 8px; color: palette(placeholder-text); white-space: nowrap;'>Type:</td><td>%1</td></tr>"
        "<tr><td style='padding-right: 8px; color: palette(placeholder-text); white-space: nowrap;'>Size:</td><td>%2</td></tr>"
        "<tr><td style='padding-right: 8px; color: palette(placeholder-text); white-space: nowrap;'>Modified:</td><td>%3</td></tr>"
    ).arg(
        typeStr.toHtmlEscaped(),
        sizeStr.toHtmlEscaped(),
        dateStr.toHtmlEscaped()
    );

    if (dimensionsText.has_value()) {
        html += QStringLiteral(
            "<tr data-kerything-dimensions='1'>"
            "<td style='padding-right: 8px; color: palette(placeholder-text); white-space: nowrap;'>Dimensions:</td>"
            "<td>%1</td></tr>"
        ).arg(dimensionsText->toHtmlEscaped());
    }

    html += QStringLiteral(
        "<tr><td style='padding-right: 8px; color: palette(placeholder-text); vertical-align: top; white-space: nowrap;'>Path:</td>"
        "<td style='word-break: break-all;'>%1</td></tr>"
        "</table>"
    ).arg(pathStr.toHtmlEscaped());

    return html;
}

QString PreviewPane::dimensionsTextForSize(const QSize& dimensions)
{
    return dimensions.isValid()
        ? QStringLiteral("%1 × %2").arg(dimensions.width()).arg(dimensions.height())
        : QStringLiteral("Unknown");
}

QString PreviewPane::metadataHtmlWithDimensions(const QString& metadataText, const QSize& dimensions)
{
    return metadataHtmlWithDimensionsText(metadataText, dimensionsTextForSize(dimensions));
}

QString PreviewPane::metadataHtmlWithDimensionsPlaceholder(const QString& metadataText)
{
    return metadataHtmlWithDimensionsText(
        metadataText,
        QStringLiteral("Loading…")
    );
}

QString PreviewPane::metadataHtmlWithDimensionsUnknown(const QString& metadataText)
{
    return metadataHtmlWithDimensionsText(
        metadataText,
        QStringLiteral("Unknown")
    );
}

QString PreviewPane::metadataHtmlWithDimensionsText(const QString& metadataText, const QString& dimensionsText)
{
    static const QString pathMarker = QStringLiteral(
        "<tr><td style='padding-right: 8px; color: palette(placeholder-text); vertical-align: top; white-space: nowrap;'>Path:</td>"
    );

    const qsizetype pathRowIndex = metadataText.indexOf(pathMarker);
    if (pathRowIndex < 0) {
        return metadataText;
    }

    const QString dimensionsRow = QStringLiteral(
        "<tr data-kerything-dimensions='1'>"
        "<td style='padding-right: 8px; color: palette(placeholder-text); white-space: nowrap;'>Dimensions:</td>"
        "<td>%1</td></tr>"
    ).arg(dimensionsText.toHtmlEscaped());

    const qsizetype existingDimensionsIndex =
        metadataText.indexOf(QStringLiteral("<tr data-kerything-dimensions='1'>"));

    if (existingDimensionsIndex >= 0) {
        const qsizetype existingDimensionsEnd =
            metadataText.indexOf(QStringLiteral("</tr>"), existingDimensionsIndex);

        if (existingDimensionsEnd >= 0) {
            QString html = metadataText;
            html.replace(
                existingDimensionsIndex,
                existingDimensionsEnd + 5 - existingDimensionsIndex,
                dimensionsRow
            );
            return html;
        }
    }

    QString html = metadataText;
    html.insert(pathRowIndex, dimensionsRow);
    return html;
}

void PreviewPane::setPreviewContent(const QPixmap& pixmap, const QString& metadataText, bool isIcon)
{
    previewImageWidget_->setPixmap(pixmap, isIcon);
    metadataLabel_->setText(metadataText);
}

void PreviewPane::cancelCurrentJob()
{
#ifdef KERYTHING_WITH_KF6
    if (currentJob_) {
        currentJob_->kill();
        currentJob_ = nullptr;
    }
#endif
    if (currentProcess_) {
        QProcess* process = currentProcess_;
        currentProcess_ = nullptr;

        process->disconnect(this);

        if (process->state() != QProcess::NotRunning) {
            process->terminate();

            if (!process->waitForFinished(100)) {
                process->kill();
            }
        }

        process->deleteLater();
    }
}

void PreviewPane::cancelImageLoad()
{
    if (!imageLoadWatcher_) {
        return;
    }

    auto* watcher = imageLoadWatcher_.data();
    imageLoadWatcher_ = nullptr;

    disconnect(watcher, nullptr, this, nullptr);

    connect(watcher, &QFutureWatcher<ImageLoadResult>::finished,
            watcher, &QObject::deleteLater);
}

void PreviewPane::cachePreview(const QString& cacheKey, const QPixmap& pixmap, bool isIcon, const QSize& dimensions)
{
    if (cacheKey.isEmpty() || pixmap.isNull()) {
        return;
    }

    memoryCache_.insert(
        cacheKey,
        new PreviewCacheEntry{
            .pixmap = pixmap,
            .dimensions = dimensions,
            .isIcon = isIcon
        },
        pixmapCacheCostKiB(pixmap)
    );
}

QString PreviewPane::previewCacheKey(const QUrl& url) const
{
    if (!url.isLocalFile()) {
        return url.toString();
    }

    const QFileInfo fileInfo(url.toLocalFile());

    return QStringLiteral("%1|%2|%3|%4")
        .arg(
            fileInfo.absoluteFilePath(),
            QString::number(fileInfo.size()),
            QString::number(fileInfo.lastModified().toMSecsSinceEpoch()),
            QString::number(previewTargetWidth())
        );
}

int PreviewPane::previewTargetWidth() const
{
    return std::clamp(
        static_cast<int>(previewImageWidget_->width() * devicePixelRatioF()),
        512,
        2048
    );
}

int PreviewPane::pixmapCacheCostKiB(const QPixmap& pixmap)
{
    if (pixmap.isNull()) {
        return 1;
    }

    const qint64 bytes =
        static_cast<qint64>(pixmap.width()) *
        static_cast<qint64>(pixmap.height()) *
        std::max(1, pixmap.depth()) /
        8;

    return static_cast<int>(std::max<qint64>(1, bytes / 1024));
}

bool PreviewPane::isImageUrl(const QUrl& url)
{
    if (!url.isLocalFile()) {
        return false;
    }

    const QFileInfo fileInfo(url.toLocalFile());
    return fileInfo.isFile() && isImageFile(fileInfo);
}

QSize PreviewPane::imageDimensions(const QUrl& url)
{
    if (!url.isLocalFile()) {
        return {};
    }

    const QString localPath = url.toLocalFile();
    const QFileInfo fileInfo(localPath);

    if (!fileInfo.isFile() || !isImageFile(fileInfo)) {
        return {};
    }

    QImageReader reader(localPath);
    reader.setAutoTransform(true);
    reader.setDecideFormatFromContent(true);

    return reader.size();
}