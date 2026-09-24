// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  derickso <https://github.com/derickso>

#include "PreviewPane.h"

#include <QDateTime>
#include <QDir>
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
#include <QSettings>
#include <QStandardPaths>
#include <QVBoxLayout>

#ifdef KERYTHING_WITH_KF6
#include <KConfigGroup>
#include <KSharedConfig>
#endif

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

bool PreviewPane::userWantsFilmstrip()
{
#ifdef KERYTHING_WITH_KF6
    KConfigGroup config(KSharedConfig::openConfig(QStringLiteral("ffmpegthumbsrc")), QStringLiteral("General"));
    return config.readEntry("filmstrip", true);
#else
    QSettings config(QDir::homePath() + QStringLiteral("/.config/ffmpegthumbsrc"), QSettings::IniFormat);
    return config.value(QStringLiteral("General/filmstrip"), true).toBool();
#endif
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

    return imageExtensions.contains(fileInfo.suffix().toLower());
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

    return videoExtensions.contains(fileInfo.suffix().toLower());
}

PreviewPane::PreviewPane(QWidget* parent)
    : QFrame(parent)
{
    setFrameStyle(QFrame::NoFrame);
    setMinimumWidth(220);

    memoryCache_.setMaxCost(100);

    debounceTimer_.setSingleShot(true);
    debounceTimer_.setInterval(50);
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
    debounceTimer_.stop();
    memoryCache_.clear();
}

void PreviewPane::clearPreview(const QString& placeholder)
{
    cancelCurrentJob();
    debounceTimer_.stop();
    currentUrl_.clear();
    currentMetadataText_.clear();
    isThemeIcon_ = false;

    titleLabel_->setText(QStringLiteral("<span style='word-break: break-all;'>Preview</span>"));
    titleLabel_->setToolTip(QString());

    previewImageWidget_->setText(placeholder);
    metadataLabel_->clear();
}

void PreviewPane::showUnmounted()
{
    cancelCurrentJob();
    debounceTimer_.stop();
    currentUrl_.clear();
    currentMetadataText_.clear();
    isThemeIcon_ = false;

    titleLabel_->setText(QStringLiteral("<span style='word-break: break-all;'>Preview (Unmounted)</span>"));
    titleLabel_->setToolTip(QString());

    previewImageWidget_->setText(
        QStringLiteral("This item is on an unmounted device.\nMount the device to view previews.")
    );
    metadataLabel_->clear();
}

void PreviewPane::previewUrl(const QUrl& url)
{
    if (currentUrl_ == url && (debounceTimer_.isActive() || currentProcess_)) {
        return;
    }

    cancelCurrentJob();
    debounceTimer_.stop();

    currentUrl_ = url;
    isThemeIcon_ = false;

    const QString localFilePath = url.toLocalFile();
    const QFileInfo fileInfo(localFilePath);

    titleLabel_->setText(QStringLiteral("<span style='word-break: break-all;'>%1</span>")
        .arg(fileInfo.fileName().toHtmlEscaped()));
    titleLabel_->setToolTip(fileInfo.absoluteFilePath());

    const QString metaText = generateMetadataHtml(fileInfo);
    currentMetadataText_ = metaText;
    metadataLabel_->setText(metaText);

    // Fast in-memory cache hit
    if (PreviewCacheEntry* cached = memoryCache_.object(url.toString())) {
        isThemeIcon_ = cached->isIcon;
        setPreviewContent(cached->pixmap, cached->metadataText);
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

#ifdef KERYTHING_WITH_KF6
    cancelCurrentJob();

    KFileItemList items;
    items.append(KFileItem(currentUrl_));

    const QStringList plugins = KIO::PreviewJob::availablePlugins();

    // Request high-resolution preview matching pane width and screen DPI
    const int reqWidth = std::max(1024, static_cast<int>(previewImageWidget_->width() * devicePixelRatioF()));
    const QSize targetSize(reqWidth, reqWidth);

    auto* job = KIO::filePreview(items, targetSize, &plugins);
    job->setScaleType(KIO::PreviewJob::ScaledAndCached);
    job->setDevicePixelRatio(devicePixelRatioF());
    currentJob_ = job;

    connect(job, &KIO::PreviewJob::gotPreview, this,
            [this, url = currentUrl_, meta = currentMetadataText_](const KFileItem& /*item*/, const QPixmap& preview) {
                if (!preview.isNull()) {
                    isThemeIcon_ = false;
                    memoryCache_.insert(url.toString(), new PreviewCacheEntry{preview, meta, false});
                    if (currentUrl_ == url) {
                        setPreviewContent(preview, meta);
                    }
                }
            });

    connect(job, &KIO::PreviewJob::failed, this,
            [this, url = currentUrl_, meta = currentMetadataText_](const KFileItem& /*item*/) {
                generateFallbackOrIcon(url, meta);
            });
#else
    generateFallbackOrIcon(currentUrl_, currentMetadataText_);
#endif
}

void PreviewPane::generateFallbackOrIcon(const QUrl& url, const QString& meta)
{
    const QString localPath = url.toLocalFile();
    const QFileInfo fileInfo(localPath);

    if (isVideoFile(fileInfo)) {
        generateVideoThumbnail(url, meta);
        return;
    }

    if (isImageFile(fileInfo)) {
        QImageReader reader(localPath);
        reader.setAutoTransform(true);
        const QSize origSize = reader.size();
        if (origSize.isValid()) {
            const int targetW = std::max(1024, static_cast<int>(previewImageWidget_->width() * devicePixelRatioF()));
            reader.setScaledSize(origSize.scaled(QSize(targetW, targetW), Qt::KeepAspectRatio));
        }

        const QImage img = reader.read();
        if (!img.isNull()) {
            const QPixmap pixmap = QPixmap::fromImage(img);
            isThemeIcon_ = false;
            memoryCache_.insert(url.toString(), new PreviewCacheEntry{pixmap, meta, false});
            if (currentUrl_ == url) {
                setPreviewContent(pixmap, meta);
            }
            return;
        }
    }

    showThemeIcon(url, meta);
}

void PreviewPane::generateVideoThumbnail(const QUrl& url, const QString& meta)
{
    const QString localPath = url.toLocalFile();
    const bool showFilmstrip = userWantsFilmstrip();
    const int thumbRes = std::max(1024, static_cast<int>(previewImageWidget_->width() * devicePixelRatioF()));

    QString program = QStandardPaths::findExecutable(QStringLiteral("ffmpegthumbnailer"));
    QStringList args;

    if (!program.isEmpty()) {
        args << QStringLiteral("-i") << localPath
             << QStringLiteral("-o") << QStringLiteral("/dev/stdout")
             << QStringLiteral("-s") << QString::number(thumbRes)
             << QStringLiteral("-c") << QStringLiteral("png");

        if (showFilmstrip) {
            args << QStringLiteral("-f");
        }
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
        showThemeIcon(url, meta);
        return;
    }

    cancelCurrentJob();

    auto* proc = new QProcess(this);
    currentProcess_ = proc;

    connect(proc, &QProcess::finished, this,
            [this, proc, url, meta](int exitCode, QProcess::ExitStatus exitStatus) {
                const QByteArray data = proc->readAllStandardOutput();
                proc->deleteLater();

                if (currentProcess_ == proc) {
                    currentProcess_ = nullptr;
                }

                if (exitCode == 0 && exitStatus == QProcess::NormalExit && !data.isEmpty()) {
                    QImage img;
                    if (img.loadFromData(data, "PNG")) {
                        const QPixmap pixmap = QPixmap::fromImage(img);
                        isThemeIcon_ = false;
                        memoryCache_.insert(url.toString(), new PreviewCacheEntry{pixmap, meta, false});
                        if (currentUrl_ == url) {
                            setPreviewContent(pixmap, meta);
                        }
                        return;
                    }
                }

                if (currentUrl_ == url) {
                    showThemeIcon(url, meta);
                }
            });

    proc->start(program, args);
}

void PreviewPane::showThemeIcon(const QUrl& url, const QString& meta)
{
    const QString localPath = url.toLocalFile();
    const QFileInfo fileInfo(localPath);

    QMimeDatabase mimeDb;
    const QMimeType mimeType = mimeDb.mimeTypeForFile(fileInfo);
    QIcon icon = QIcon::fromTheme(mimeType.iconName());
    if (icon.isNull()) {
        icon = QIcon::fromTheme(mimeType.genericIconName());
    }
    if (icon.isNull()) {
        icon = QIcon::fromTheme(QStringLiteral("unknown"));
    }

    const QPixmap pixmap = icon.pixmap(128, 128);
    if (!pixmap.isNull()) {
        isThemeIcon_ = true;
        memoryCache_.insert(url.toString(), new PreviewCacheEntry{pixmap, meta, true});
        if (currentUrl_ == url) {
            setPreviewContent(pixmap, meta);
        }
        return;
    }

    if (currentUrl_ == url) {
        previewImageWidget_->setText(QStringLiteral("No preview available"));
        metadataLabel_->setText(meta);
    }
}

QString PreviewPane::generateMetadataHtml(const QFileInfo& fileInfo) const
{
    QMimeDatabase mimeDb;
    const QMimeType mime = mimeDb.mimeTypeForFile(fileInfo);
    const QString typeStr = mime.comment().isEmpty() ? mime.name() : mime.comment();

    const QLocale locale;
    const QString sizeStr = fileInfo.isDir()
        ? QStringLiteral("—")
        : locale.formattedDataSize(fileInfo.size(), 1, QLocale::DataSizeIecFormat);

    const QString dateStr = fileInfo.lastModified().isValid()
        ? fileInfo.lastModified().toString(QStringLiteral("yyyy-MM-dd hh:mm:ss"))
        : QStringLiteral("Unknown");

    QString dimensionsRow;
    QImageReader reader(fileInfo.absoluteFilePath());
    if (reader.canRead()) {
        const QSize dims = reader.size();
        if (dims.isValid()) {
            dimensionsRow = QStringLiteral(
                "<tr><td style='padding-right: 8px; color: palette(placeholder-text); white-space: nowrap;'>Dimensions:</td>"
                "<td>%1 × %2</td></tr>"
            ).arg(dims.width()).arg(dims.height());
        }
    }

    return QStringLiteral(
        "<table cellspacing='0' cellpadding='2' style='font-size: 9pt;'>"
        "<tr><td style='padding-right: 8px; color: palette(placeholder-text); white-space: nowrap;'>Type:</td><td>%1</td></tr>"
        "<tr><td style='padding-right: 8px; color: palette(placeholder-text); white-space: nowrap;'>Size:</td><td>%2</td></tr>"
        "<tr><td style='padding-right: 8px; color: palette(placeholder-text); white-space: nowrap;'>Modified:</td><td>%3</td></tr>"
        "%4"
        "<tr><td style='padding-right: 8px; color: palette(placeholder-text); vertical-align: top; white-space: nowrap;'>Path:</td>"
        "<td style='word-break: break-all;'>%5</td></tr>"
        "</table>"
    ).arg(
        typeStr.toHtmlEscaped(),
        sizeStr.toHtmlEscaped(),
        dateStr.toHtmlEscaped(),
        dimensionsRow,
        fileInfo.absoluteFilePath().toHtmlEscaped()
    );
}

void PreviewPane::setPreviewContent(const QPixmap& pixmap, const QString& metadataText)
{
    previewImageWidget_->setPixmap(pixmap, isThemeIcon_);
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
        currentProcess_->kill();
        currentProcess_->deleteLater();
        currentProcess_ = nullptr;
    }
}