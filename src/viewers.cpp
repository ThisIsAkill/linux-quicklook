#include "viewers.h"
#include "chrome.h"

#include <QAudioOutput>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFutureWatcher>
#include <QIcon>
#include <QImageReader>
#include <QLabel>
#include <QLocale>
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QMimeDatabase>
#include <QMouseEvent>
#include <QMovie>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QProcess>
#include <QRadialGradient>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedLayout>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTimer>
#include <QVBoxLayout>
#include <QVideoSink>
#include <QVideoWidget>
#include <QtConcurrent>

#include <KIO/PreviewJob>
#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/SyntaxHighlighter>
#include <KSyntaxHighlighting/Theme>

#include <archive.h>
#include <archive_entry.h>
#include <poppler-qt6.h>

#include <memory>

namespace
{

constexpr qint64 TextLimit = 1024 * 1024;   // bytes of text shown
constexpr int ListingLimit = 2000;          // archive entries shown
constexpr int PdfPageLimit = 200;           // pages rendered
constexpr int MaxImageEdge = 4096;          // larger images are decoded downscaled
constexpr int VectorEdge = 640;             // display size of SVG images
constexpr int PageMargin = 18;              // around PDF pages, including their shadow
constexpr int MediaBarHeight = 44;

QMimeDatabase &mimeDb()
{
    static QMimeDatabase db;
    return db;
}

KSyntaxHighlighting::Repository &syntaxRepository()
{
    static KSyntaxHighlighting::Repository repository;
    return repository;
}

QString dimensions(const QSize &size)
{
    return QStringLiteral("%1 × %2").arg(size.width()).arg(size.height());
}

QString countOf(qsizetype count, const QString &singular, const QString &plural)
{
    return QStringLiteral("%1 %2").arg(count).arg(count == 1 ? singular : plural);
}

QString formatTime(qint64 ms)
{
    const qint64 s = ms / 1000;
    return s >= 3600 ? QStringLiteral("%1:%2:%3").arg(s / 3600).arg(s / 60 % 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'))
                     : QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
}

// Scrolling text widgets draw on the panel's well instead of their own
// background, so the well's rounded corners stay visible.
template<typename T>
T *onWell(T *w)
{
    w->setFocusPolicy(Qt::NoFocus);
    w->setFrameShape(QFrame::NoFrame);
    w->viewport()->setAutoFillBackground(false);
    return w;
}

QVBoxLayout *bareLayout(QWidget *w)
{
    auto *layout = new QVBoxLayout(w);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    return layout;
}

// A centred message in the secondary text colour ("Preparing preview…").
QLabel *noteLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setAlignment(Qt::AlignCenter);
    label->setWordWrap(true);
    QPalette pal = label->palette();
    pal.setColor(QPalette::WindowText, Chrome::mutedText(pal));
    label->setPalette(pal);
    label->setFont(Chrome::scaledFont(label->font(), 1.1));
    return label;
}

bool looksLikeText(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray head = f.read(8192);
    if (head.contains('\0'))
        return false;
    // Accept valid UTF-8, allowing a character cut off at the end of the sample.
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString text = decoder(head.left(qMax<qsizetype>(0, head.size() - 4)));
    return !decoder.hasError() && !text.contains(QChar::ReplacementCharacter);
}

// ------------------------------------------------------------------ image

class ImageViewer : public Viewer
{
public:
    ImageViewer(const QString &path, QWidget *parent)
        : Viewer(parent)
    {
        setBackdrop(Stage);
        QImageReader reader(path);
        reader.setAutoTransform(true);
        QSize size = reader.size();
        if (reader.transformation() & QImageIOHandler::TransformationRotate90)
            size.transpose();
        // Vector images have no real pixel size; draw them large and sharp.
        const bool vector = reader.format().startsWith("svg");
        if (vector && size.isValid())
            size = size.scaled(VectorEdge, VectorEdge, Qt::KeepAspectRatio);
        setNaturalSize(size);
        m_vector = vector;

        if (reader.supportsAnimation() && reader.imageCount() != 1) {
            m_movie = new QMovie(path, {}, this);
            connect(m_movie, &QMovie::frameChanged, this, [this] {
                m_pixmap = m_movie->currentPixmap();
                if (!naturalSize().isValid())
                    setNaturalSize(m_pixmap.size());
                if (details().isEmpty())
                    setDetails({dimensions(m_pixmap.size()), tr("animated")});
                m_scaled = {};
                update();
            });
            m_movie->start();
            return;
        }

        // Decode off the UI thread: large photos and camera RAW files take a while.
        auto *watcher = new QFutureWatcher<QImage>(this);
        connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, watcher, size] {
            m_image = watcher->result();
            m_failed = m_image.isNull();
            if (!m_failed && !m_vector)
                setNaturalSize(m_image.size());
            if (!m_failed)
                setDetails({dimensions(m_vector ? size : m_image.size())});
            m_scaled = {};
            update();
        });
        const QSize vectorSize = vector ? size * devicePixelRatioF() : QSize();
        watcher->setFuture(QtConcurrent::run([path, vectorSize] {
            QImageReader r(path);
            r.setAutoTransform(true);
            const QSize s = r.size();
            if (vectorSize.isValid())
                r.setScaledSize(vectorSize);
            else if (s.isValid() && qMax(s.width(), s.height()) > MaxImageEdge)
                r.setScaledSize(s.scaled(MaxImageEdge, MaxImageEdge, Qt::KeepAspectRatio));
            return r.read();
        }));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        if (m_failed) {
            p.setPen(Chrome::mutedText(palette()));
            p.drawText(rect(), Qt::AlignCenter, tr("This image could not be read."));
            return;
        }
        const QSize source = m_movie ? m_pixmap.size() : m_image.size();
        if (source.isEmpty())
            return;
        const qreal dpr = devicePixelRatioF();
        QSize target = source;
        if (source.width() > width() * dpr || source.height() > height() * dpr)
            target = source.scaled((QSizeF(size()) * dpr).toSize(), Qt::KeepAspectRatio);
        if (m_scaled.size() != target) {
            const QImage img = m_movie ? m_pixmap.toImage() : m_image;
            m_hasAlpha = img.hasAlphaChannel();
            m_scaled = QPixmap::fromImage(img.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            m_scaled.setDevicePixelRatio(dpr);
        }
        const QSizeF logical = QSizeF(m_scaled.size()) / dpr;
        const QPointF origin((width() - logical.width()) / 2, (height() - logical.height()) / 2);
        p.setClipPath(Chrome::roundedPath(rect(), Chrome::WellRadius));
        if (m_hasAlpha)
            p.fillRect(QRectF(origin, logical), checkerboard());
        p.drawPixmap(origin, m_scaled);
    }

private:
    // A faint checkerboard reveals transparent areas without shouting.
    QBrush checkerboard() const
    {
        const bool dark = Chrome::isDark(palette());
        const QColor a = dark ? QColor(0x3a, 0x3d, 0x41) : QColor(0xff, 0xff, 0xff);
        const QColor b = dark ? QColor(0x2f, 0x32, 0x35) : QColor(0xe4, 0xe6, 0xe8);
        QPixmap tile(16, 16);
        tile.fill(a);
        QPainter tp(&tile);
        tp.fillRect(0, 0, 8, 8, b);
        tp.fillRect(8, 8, 8, 8, b);
        return QBrush(tile);
    }

    QImage m_image;
    QPixmap m_pixmap; // current frame of an animation
    QPixmap m_scaled; // cache for the current widget size
    QMovie *m_movie = nullptr;
    bool m_failed = false;
    bool m_vector = false;
    bool m_hasAlpha = false;
};

// ------------------------------------------------------------ audio/video

// Custom-painted transport controls: a round play/pause button, a slim
// progress track filled in the accent colour, and the time. Under video
// the glyphs are white on black; elsewhere they use the palette.
class MediaBar : public QWidget
{
public:
    MediaBar(QMediaPlayer *player, bool onScreen, QWidget *parent)
        : QWidget(parent)
        , m_player(player)
        , m_onScreen(onScreen)
    {
        setFixedHeight(MediaBarHeight);
        setFocusPolicy(Qt::NoFocus);
        setMouseTracking(true);
        setCursor(Qt::PointingHandCursor);
        const auto refresh = [this] { update(); };
        connect(player, &QMediaPlayer::positionChanged, this, refresh);
        connect(player, &QMediaPlayer::durationChanged, this, refresh);
        connect(player, &QMediaPlayer::playbackStateChanged, this, refresh);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QColor fg = m_onScreen ? QColor(Qt::white) : palette().color(QPalette::WindowText);
        const QColor accent = m_onScreen ? QColor(Qt::white) : palette().color(QPalette::Highlight);
        const bool playing = m_player->playbackState() == QMediaPlayer::PlayingState;

        // Play/pause button
        const QRectF button = buttonRect();
        p.setPen(Qt::NoPen);
        p.setBrush(Chrome::withAlpha(fg, m_hoverButton ? 60 : 36));
        p.drawEllipse(button);
        p.setBrush(fg);
        const QPointF c = button.center();
        if (playing) {
            p.drawRoundedRect(QRectF(c.x() - 5, c.y() - 5.5, 3.5, 11), 1, 1);
            p.drawRoundedRect(QRectF(c.x() + 1.5, c.y() - 5.5, 3.5, 11), 1, 1);
        } else {
            QPainterPath play;
            play.moveTo(c.x() - 4, c.y() - 6);
            play.lineTo(c.x() + 6.5, c.y());
            play.lineTo(c.x() - 4, c.y() + 6);
            play.closeSubpath();
            p.drawPath(play);
        }

        // Time, then the track fills the space between.
        p.setFont(Chrome::scaledFont(font(), 0.9));
        const QString time = formatTime(m_player->position()) + QStringLiteral(" / ") + formatTime(m_player->duration());
        const int timeWidth = p.fontMetrics().horizontalAdvance(QStringLiteral("00:00 / 00:00"));
        p.setPen(Chrome::withAlpha(fg, 200));
        p.drawText(QRect(width() - 14 - timeWidth, 0, timeWidth, height()), Qt::AlignVCenter | Qt::AlignRight, time);

        const QRectF track = trackRect(timeWidth);
        p.setPen(Qt::NoPen);
        p.setBrush(Chrome::withAlpha(fg, 60));
        p.drawRoundedRect(track, 2, 2);
        const qreal progress = m_player->duration() > 0 ? qreal(m_player->position()) / m_player->duration() : 0;
        const qreal x = track.left() + track.width() * progress;
        p.setBrush(accent);
        p.drawRoundedRect(QRectF(track.left(), track.top(), x - track.left(), track.height()), 2, 2);
        p.drawEllipse(QPointF(x, track.center().y()), 6, 6);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (buttonRect().contains(event->position())) {
            m_player->playbackState() == QMediaPlayer::PlayingState ? m_player->pause() : m_player->play();
        } else {
            m_scrubbing = true;
            seekTo(event->position().x());
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const bool hover = buttonRect().contains(event->position());
        if (hover != m_hoverButton) {
            m_hoverButton = hover;
            update();
        }
        if (m_scrubbing)
            seekTo(event->position().x());
    }

    void mouseReleaseEvent(QMouseEvent *) override { m_scrubbing = false; }

    void leaveEvent(QEvent *) override
    {
        m_hoverButton = false;
        update();
    }

private:
    QRectF buttonRect() const { return QRectF(10, (height() - 30) / 2.0, 30, 30); }

    QRectF trackRect(int timeWidth) const
    {
        const qreal left = buttonRect().right() + 14;
        return QRectF(left, height() / 2.0 - 2, width() - 14 - timeWidth - 14 - left, 4);
    }

    void seekTo(qreal x)
    {
        const int timeWidth = QFontMetrics(Chrome::scaledFont(font(), 0.9)).horizontalAdvance(QStringLiteral("00:00 / 00:00"));
        const QRectF track = trackRect(timeWidth);
        const qreal fraction = qBound<qreal>(0, (x - track.left()) / track.width(), 1);
        m_player->setPosition(qint64(fraction * m_player->duration()));
    }

    QMediaPlayer *m_player;
    bool m_onScreen;
    bool m_hoverButton = false;
    bool m_scrubbing = false;
};

// Cover art (or a generic icon) on a soft shadow, with the title and artist
// from the file's tags beneath.
class AudioCard : public QWidget
{
public:
    explicit AudioCard(QWidget *parent)
        : QWidget(parent)
    {
    }

    void setCover(const QPixmap &cover, bool isArt)
    {
        m_cover = cover;
        m_isArt = isArt;
        update();
    }

    void setText(const QString &title, const QString &subtitle)
    {
        m_title = title;
        m_subtitle = subtitle;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        const QFont titleFont = Chrome::scaledFont(font(), 1.25, QFont::DemiBold);
        const int textHeight = QFontMetrics(titleFont).height() + fontMetrics().height() + 6;
        const int art = qMin(240, height() - textHeight - 48);
        const QRectF artRect((width() - art) / 2.0, (height() - art - textHeight - 16) / 2.0, art, art);

        if (m_isArt) {
            Chrome::drawShadow(p, artRect, 12, 14, 90, 4);
            p.setClipPath(Chrome::roundedPath(artRect, 12));
            p.drawPixmap(artRect.toRect(), m_cover);
            p.setClipping(false);
        } else {
            QRadialGradient glow(artRect.center(), art * 0.7);
            glow.setColorAt(0, Chrome::withAlpha(palette().color(QPalette::Highlight), 70));
            glow.setColorAt(1, Qt::transparent);
            p.fillRect(artRect.adjusted(-art * 0.4, -art * 0.4, art * 0.4, art * 0.4), glow);
            const int icon = art * 2 / 3;
            p.drawPixmap(QRect(int(artRect.center().x() - icon / 2), int(artRect.center().y() - icon / 2), icon, icon), m_cover);
        }

        const int y = int(artRect.bottom()) + 16;
        p.setFont(titleFont);
        p.setPen(palette().color(QPalette::WindowText));
        const QRect titleRect(16, y, width() - 32, QFontMetrics(titleFont).height());
        p.drawText(titleRect, Qt::AlignCenter, QFontMetrics(titleFont).elidedText(m_title, Qt::ElideMiddle, titleRect.width()));
        p.setFont(font());
        p.setPen(Chrome::mutedText(palette()));
        const QRect subRect(16, titleRect.bottom() + 6, width() - 32, fontMetrics().height());
        p.drawText(subRect, Qt::AlignCenter, fontMetrics().elidedText(m_subtitle, Qt::ElideRight, subRect.width()));
    }

private:
    QPixmap m_cover;
    QString m_title;
    QString m_subtitle;
    bool m_isArt = false;
};

class MediaViewer : public Viewer
{
public:
    MediaViewer(const QString &path, bool video, bool autoplay, QWidget *parent)
        : Viewer(parent)
        , m_player(new QMediaPlayer(this))
    {
        auto *layout = bareLayout(this);
        m_player->setAudioOutput(new QAudioOutput(this));

        if (video) {
            // The video widget has square corners, so it sits inside the
            // well's rounded black frame rather than over its corners.
            setBackdrop(Screen);
            layout->setContentsMargins(Chrome::WellRadius, Chrome::WellRadius, Chrome::WellRadius, 0);
            auto *screen = new QVideoWidget(this);
            m_player->setVideoOutput(screen);
            connect(screen->videoSink(), &QVideoSink::videoSizeChanged, this, [this, screen] {
                setVideoSize(screen->videoSink()->videoSize());
            });
            layout->addWidget(screen, 1);
        } else {
            setBackdrop(Stage);
            auto *card = new AudioCard(this);
            const QMimeType mime = mimeDb().mimeTypeForFile(path);
            card->setCover(QIcon::fromTheme(mime.iconName(), QIcon::fromTheme(QStringLiteral("audio-x-generic"))).pixmap(160), false);
            card->setText(QFileInfo(path).completeBaseName(), mime.comment());
            connect(m_player, &QMediaPlayer::metaDataChanged, this, [this, card, path] {
                const QMediaMetaData meta = m_player->metaData();
                const QImage img = meta.value(QMediaMetaData::CoverArtImage).value<QImage>();
                if (!img.isNull())
                    card->setCover(QPixmap::fromImage(img.scaled(480, 480, Qt::KeepAspectRatio, Qt::SmoothTransformation)), true);
                const QString title = meta.stringValue(QMediaMetaData::Title);
                QStringList by;
                for (auto key : {QMediaMetaData::ContributingArtist, QMediaMetaData::AlbumArtist, QMediaMetaData::AlbumTitle}) {
                    const QString value = meta.stringValue(key);
                    if (!value.isEmpty() && !by.contains(value))
                        by << value;
                }
                if (!title.isEmpty() || !by.isEmpty())
                    card->setText(title.isEmpty() ? QFileInfo(path).completeBaseName() : title, by.join(QStringLiteral("  ·  ")));
            });
            layout->addWidget(card, 1);
            setNaturalSize({560, 420});
        }
        layout->addWidget(new MediaBar(m_player, video, this));

        connect(m_player, &QMediaPlayer::metaDataChanged, this, [this, video] {
            if (video)
                setVideoSize(m_player->metaData().value(QMediaMetaData::Resolution).toSize());
            updateDetails();
        });
        connect(m_player, &QMediaPlayer::durationChanged, this, &MediaViewer::updateDetails);
        m_player->setSource(QUrl::fromLocalFile(path));
        // Paused at the start shows the first frame without playing.
        autoplay ? m_player->play() : m_player->pause();
    }

    ~MediaViewer() override { m_player->stop(); }

private:
    // The panel is shaped to the video plus the frame and controls around it.
    void setVideoSize(const QSize &size)
    {
        if (!size.isValid() || size == m_videoSize)
            return;
        m_videoSize = size;
        setNaturalSize(size + QSize(2 * Chrome::WellRadius, Chrome::WellRadius + MediaBarHeight));
        updateDetails();
    }

    void updateDetails()
    {
        QStringList details;
        if (m_videoSize.isValid())
            details << dimensions(m_videoSize);
        if (m_player->duration() > 0)
            details << formatTime(m_player->duration());
        setDetails(details);
    }

    QMediaPlayer *m_player;
    QSize m_videoSize;
};

// -------------------------------------------------------------------- PDF

// One rendered page on a soft shadow, as on a desk.
class PageWidget : public QWidget
{
public:
    PageWidget(const QPixmap &page, QWidget *parent)
        : QWidget(parent)
        , m_page(page)
    {
        const QSize logical = (QSizeF(page.size()) / page.devicePixelRatio()).toSize();
        setFixedSize(logical + QSize(2 * Spread, 2 * Spread + Drop));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const QRectF page(Spread, Spread, width() - 2 * Spread, height() - 2 * Spread - Drop);
        Chrome::drawShadow(p, page, 2, Spread, Chrome::isDark(palette()) ? 120 : 55, Drop);
        p.setRenderHint(QPainter::Antialiasing);
        p.setClipPath(Chrome::roundedPath(page, 2));
        p.drawPixmap(page.topLeft(), m_page);
    }

private:
    static constexpr int Spread = 10;
    static constexpr int Drop = 2;
    QPixmap m_page;
};

class PdfViewer : public Viewer
{
public:
    PdfViewer(const QString &path, QWidget *parent)
        : Viewer(parent)
        , m_doc(Poppler::Document::load(path))
        , m_area(new QScrollArea(this))
        , m_pages(new QWidget)
    {
        bareLayout(this)->addWidget(onWell(m_area));
        m_area->setWidgetResizable(true);
        m_area->setWidget(m_pages);
        m_area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto *column = new QVBoxLayout(m_pages);
        column->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
        column->setContentsMargins(PageMargin, PageMargin - 6, PageMargin, PageMargin);
        column->setSpacing(4);
        if (!m_doc || m_doc->isLocked()) {
            column->addWidget(noteLabel(m_doc ? tr("This PDF is password protected.") : tr("This PDF could not be read."), m_pages));
            m_doc.reset();
            return;
        }
        m_doc->setRenderHint(Poppler::Document::Antialiasing);
        m_doc->setRenderHint(Poppler::Document::TextAntialiasing);
        m_count = qMin(m_doc->numPages(), PdfPageLimit);
        setDetails({countOf(m_doc->numPages(), tr("page"), tr("pages"))});
    }

protected:
    void showEvent(QShowEvent *event) override
    {
        Viewer::showEvent(event);
        if (m_doc && m_next == 0)
            QTimer::singleShot(0, this, [this] { renderNext(); });
    }

private:
    // Renders one page per event-loop pass so the first page appears at once
    // and navigation stays responsive while long documents fill in.
    void renderNext()
    {
        if (m_next >= m_count)
            return;
        std::unique_ptr<Poppler::Page> page(m_doc->page(m_next++));
        if (page) {
            const qreal dpr = devicePixelRatioF();
            // Leave room for the scroll bar that appears once pages overflow.
            const int width = m_area->width() - m_area->verticalScrollBar()->sizeHint().width() - 2 * PageMargin - 2 * 10;
            const qreal dpi = 72.0 * width / page->pageSizeF().width() * dpr;
            QImage img = page->renderToImage(dpi, dpi);
            img.setDevicePixelRatio(dpr);
            m_pages->layout()->addWidget(new PageWidget(QPixmap::fromImage(img), m_pages));
        }
        QTimer::singleShot(0, this, [this] { renderNext(); });
    }

    std::unique_ptr<Poppler::Document> m_doc;
    QScrollArea *m_area;
    QWidget *m_pages;
    int m_count = 0;
    int m_next = 0;
};

// ------------------------------------------------------------ office docs

// Converts to PDF with LibreOffice (in its own profile, so it works while
// LibreOffice is open) and then shows the PDF.
class OfficeViewer : public Viewer
{
public:
    OfficeViewer(const QString &path, const QString &office, QWidget *parent)
        : Viewer(parent)
        , m_stack(new QStackedLayout(this))
    {
        auto *wait = noteLabel(tr("Preparing preview…"), this);
        m_stack->addWidget(wait);
        if (!m_tmp.isValid())
            return;
        auto *proc = new QProcess(this);
        connect(proc, &QProcess::finished, this, [this, proc, path, wait] {
            const QString pdf = QDir(m_tmp.path()).filePath(QFileInfo(path).completeBaseName() + QStringLiteral(".pdf"));
            if (proc->exitCode() == 0 && QFileInfo::exists(pdf)) {
                auto *viewer = new PdfViewer(pdf, this);
                connect(viewer, &Viewer::detailsChanged, this, &OfficeViewer::setDetails);
                m_stack->addWidget(viewer);
                m_stack->setCurrentWidget(viewer);
                setDetails(viewer->details());
            } else {
                wait->setText(tr("This document could not be converted for preview."));
            }
        });
        const QString profile = QUrl::fromLocalFile(QDir::temp().filePath(QStringLiteral("quicklook-libreoffice"))).toString();
        proc->start(office, {QStringLiteral("-env:UserInstallation=") + profile, QStringLiteral("--headless"),
                             QStringLiteral("--convert-to"), QStringLiteral("pdf"), QStringLiteral("--outdir"),
                             m_tmp.path(), path});
    }

    ~OfficeViewer() override
    {
        for (auto *proc : findChildren<QProcess *>()) {
            proc->kill();
            proc->waitForFinished(500);
        }
    }

private:
    QStackedLayout *m_stack;
    QTemporaryDir m_tmp;
};

// ------------------------------------------------------------------- text

class TextViewer : public Viewer
{
public:
    enum Mode { Plain, Markdown, Html };

    TextViewer(const QString &path, Mode mode, QWidget *parent)
        : Viewer(parent)
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            bareLayout(this)->addWidget(noteLabel(tr("This file could not be read."), this));
            return;
        }
        const QByteArray data = f.read(TextLimit);
        const bool truncated = !f.atEnd();
        const QString text = QString::fromUtf8(data);

        if (mode != Plain) {
            auto *browser = new QTextBrowser(this);
            browser->setOpenExternalLinks(true);
            browser->document()->setDocumentMargin(20);
            mode == Markdown ? browser->setMarkdown(text) : browser->setHtml(text);
            bareLayout(this)->addWidget(onWell(browser));
            return;
        }

        auto *edit = new QPlainTextEdit(this);
        edit->setReadOnly(true);
        edit->setLineWrapMode(QPlainTextEdit::NoWrap);
        edit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        edit->document()->setDocumentMargin(14);
        edit->setPlainText(truncated ? text + tr("\n\n… (preview shows the first %1)").arg(QLocale().formattedDataSize(TextLimit))
                                     : text);
        bareLayout(this)->addWidget(onWell(edit));

        QStringList details;
        KSyntaxHighlighting::Repository &repository = syntaxRepository();
        const auto definition = repository.definitionForFileName(path);
        if (definition.isValid()) {
            auto *highlighter = new KSyntaxHighlighting::SyntaxHighlighter(edit->document());
            highlighter->setTheme(repository.defaultTheme(Chrome::isDark(palette()) ? KSyntaxHighlighting::Repository::DarkTheme
                                                                                   : KSyntaxHighlighting::Repository::LightTheme));
            highlighter->setDefinition(definition);
            details << definition.translatedName();
        }
        const qsizetype lines = text.count(QLatin1Char('\n')) + (text.isEmpty() || text.endsWith(QLatin1Char('\n')) ? 0 : 1);
        details << countOf(lines, tr("line"), tr("lines")) + (truncated ? tr(" shown") : QString());
        setDetails(details);
    }
};

// --------------------------------------------------------------- archives

class ArchiveViewer : public Viewer
{
public:
    ArchiveViewer(const QString &path, QWidget *parent)
        : Viewer(parent)
    {
        QStringList lines;
        int total = 0;
        qint64 bytes = 0;
        archive *a = archive_read_new();
        archive_read_support_filter_all(a);
        archive_read_support_format_all(a);
        bool ok = archive_read_open_filename(a, QFile::encodeName(path).constData(), 64 * 1024) == ARCHIVE_OK;
        archive_entry *entry = nullptr;
        while (ok && archive_read_next_header(a, &entry) == ARCHIVE_OK) {
            ++total;
            bytes += archive_entry_size(entry);
            if (lines.size() < ListingLimit) {
                const char *name = archive_entry_pathname_utf8(entry);
                lines << QString::fromUtf8(name ? name : archive_entry_pathname(entry));
            }
            archive_read_data_skip(a);
        }
        archive_read_free(a);

        if (!ok) {
            bareLayout(this)->addWidget(noteLabel(tr("This archive could not be read."), this));
            return;
        }
        setDetails({countOf(total, tr("item"), tr("items")), tr("%1 unpacked").arg(QLocale().formattedDataSize(bytes))});
        if (total > lines.size())
            lines << tr("… and %1 more").arg(total - lines.size());
        auto *edit = new QPlainTextEdit(this);
        edit->setReadOnly(true);
        edit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        edit->document()->setDocumentMargin(14);
        edit->setPlainText(lines.join(QLatin1Char('\n')));
        bareLayout(this)->addWidget(onWell(edit));
    }
};

// ------------------------------------------------------------------ fonts

class FontViewer : public Viewer
{
public:
    FontViewer(const QString &path, QWidget *parent)
        : Viewer(parent)
    {
        const int id = QFontDatabase::addApplicationFont(path);
        const QStringList families = id >= 0 ? QFontDatabase::applicationFontFamilies(id) : QStringList();
        if (families.isEmpty()) {
            bareLayout(this)->addWidget(noteLabel(tr("This font could not be read."), this));
            return;
        }
        const QString family = families.first().toHtmlEscaped();
        const QStringList styles = QFontDatabase::styles(families.first());
        setDetails({families.first(), countOf(styles.size(), tr("style"), tr("styles"))});

        // The family name set in the font itself, then the pangram at
        // display, text and caption sizes, and the character set.
        const QString muted = Chrome::mutedText(palette()).name();
        QString html = QStringLiteral("<div style=\"font-family:'%1'; font-size:30pt\">%1</div>"
                                      "<div style=\"color:%2; font-size:9pt; margin-bottom:18px\">%3</div>")
                           .arg(family, muted, styles.join(QStringLiteral(" · ")).toHtmlEscaped());
        for (int pt : {24, 15, 10})
            html += QStringLiteral("<p style=\"font-family:'%1'; font-size:%2pt; margin-top:10px\">"
                                   "Sphinx of black quartz, judge my vow. The quick brown fox jumps over the lazy dog.</p>")
                        .arg(family)
                        .arg(pt);
        html += QStringLiteral("<p style=\"font-family:'%1'; font-size:16pt; margin-top:22px; color:%2\">"
                               "ABCDEFGHIJKLMNOPQRSTUVWXYZ<br>abcdefghijklmnopqrstuvwxyz<br>0123456789 ?!&amp;@#%()[]{}</p>")
                    .arg(family, muted);
        auto *browser = new QTextBrowser(this);
        browser->document()->setDocumentMargin(24);
        browser->setHtml(html);
        bareLayout(this)->addWidget(onWell(browser));
    }
};

// --------------------------------------------------- folders, everything else

// An info card: a large icon in an accent glow (replaced by a thumbnail when
// any installed KDE thumbnailer can make one, which covers formats such as
// e-books, 3D models and application bundles), the name, and a short table
// of facts.
class InfoViewer : public Viewer
{
public:
    InfoViewer(const QString &path, QWidget *parent)
        : Viewer(parent)
    {
        setBackdrop(Stage);
        const QFileInfo info(path);
        const QMimeType mime = mimeDb().mimeTypeForFile(info);
        m_name = info.fileName();
        m_pixmap = QIcon::fromTheme(mime.iconName(), QIcon::fromTheme(mime.genericIconName())).pixmap(128);

        QString contents;
        if (info.isDir()) {
            const auto count = QDir(path).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).size();
            contents = countOf(count, tr("item"), tr("items"));
            setDetails({contents});
        } else {
            contents = QLocale().formattedDataSize(info.size());
        }
        QString where = info.absolutePath();
        if (where.startsWith(QDir::homePath()))
            where.replace(0, QDir::homePath().size(), QStringLiteral("~"));
        m_rows = {{tr("Kind"), mime.comment()},
                  {info.isDir() ? tr("Contains") : tr("Size"), contents},
                  {tr("Modified"), QLocale().toString(info.lastModified(), QStringLiteral("d MMMM yyyy, ")) + QLocale().toString(info.lastModified().time(), QLocale::ShortFormat)},
                  {tr("Where"), where}};
        setNaturalSize({560, 440});

        KFileItem item(QUrl::fromLocalFile(path), mime.name());
        auto *job = KIO::filePreview(KFileItemList{item}, QSize(256, 256), nullptr);
        job->setIgnoreMaximumSize(true);
        job->setScaleType(KIO::PreviewJob::Scaled);
        connect(job, &KIO::PreviewJob::gotPreview, this, [this](const KFileItem &, const QPixmap &pix) {
            m_pixmap = pix;
            m_thumbnail = true;
            update();
        });
        connect(this, &QObject::destroyed, job, [job] { job->kill(); });
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QPalette pal = palette();
        const QFont nameFont = Chrome::scaledFont(font(), 1.35, QFont::DemiBold);
        const QFont labelFont = Chrome::scaledFont(font(), 0.92, QFont::Medium);
        const QFontMetrics nameFm(nameFont), labelFm(labelFont), valueFm(font());
        const int rowHeight = valueFm.height() + 6;
        const int artHeight = int(m_pixmap.height() / m_pixmap.devicePixelRatio());
        const int artWidth = int(m_pixmap.width() / m_pixmap.devicePixelRatio());
        const int block = artHeight + 20 + nameFm.height() + 18 + int(m_rows.size()) * rowHeight;
        int y = qMax(16, (height() - block) / 2);

        // Icon in a glow, or a thumbnail on a shadow.
        const QRect art((width() - artWidth) / 2, y, artWidth, artHeight);
        if (m_thumbnail) {
            Chrome::drawShadow(p, art, 3, 12, Chrome::isDark(pal) ? 120 : 60, 3);
            p.setClipPath(Chrome::roundedPath(art, 3));
            p.drawPixmap(art, m_pixmap);
            p.setClipping(false);
        } else {
            QRadialGradient glow(art.center(), artHeight * 0.95);
            glow.setColorAt(0, Chrome::withAlpha(pal.color(QPalette::Highlight), 64));
            glow.setColorAt(1, Qt::transparent);
            p.fillRect(art.adjusted(-artWidth, -artHeight, artWidth, artHeight), glow);
            p.drawPixmap(art, m_pixmap);
        }
        y = art.bottom() + 20;

        p.setFont(nameFont);
        p.setPen(pal.color(QPalette::WindowText));
        const QRect nameRect(24, y, width() - 48, nameFm.height());
        p.drawText(nameRect, Qt::AlignCenter, nameFm.elidedText(m_name, Qt::ElideMiddle, nameRect.width()));
        y = nameRect.bottom() + 18;

        // Two columns meeting at the centre: muted labels, plain values.
        int labelWidth = 0;
        for (const auto &row : m_rows)
            labelWidth = qMax(labelWidth, labelFm.horizontalAdvance(row.first));
        const int gap = 14;
        const int centre = width() / 2 - 30;
        for (const auto &[label, value] : m_rows) {
            p.setFont(labelFont);
            p.setPen(Chrome::mutedText(pal));
            p.drawText(QRect(centre - gap / 2 - labelWidth, y, labelWidth, rowHeight), Qt::AlignRight | Qt::AlignVCenter, label);
            p.setFont(font());
            p.setPen(pal.color(QPalette::WindowText));
            const QRect valueRect(centre + gap / 2, y, width() - 24 - centre - gap / 2, rowHeight);
            p.drawText(valueRect, Qt::AlignLeft | Qt::AlignVCenter, valueFm.elidedText(value, Qt::ElideMiddle, valueRect.width()));
            y += rowHeight;
        }
    }

private:
    QPixmap m_pixmap;
    QString m_name;
    QList<QPair<QString, QString>> m_rows;
    bool m_thumbnail = false;
};

bool inheritsAny(const QMimeType &mime, std::initializer_list<const char *> names)
{
    for (const char *name : names)
        if (mime.inherits(QLatin1String(name)))
            return true;
    return false;
}

} // namespace

void Viewer::setNaturalSize(const QSize &size)
{
    if (size.isValid() && size != m_naturalSize) {
        m_naturalSize = size;
        Q_EMIT naturalSizeChanged(size);
    }
}

void Viewer::setDetails(const QStringList &details)
{
    if (details != m_details) {
        m_details = details;
        Q_EMIT detailsChanged(details);
    }
}

Viewer *createViewer(const QString &path, QWidget *parent, bool autoplay)
{
    const QFileInfo info(path);
    const QMimeType mime = mimeDb().mimeTypeForFile(info);
    const QString name = mime.name();

    if (!info.isReadable() || info.isDir())
        return new InfoViewer(path, parent);
    if (mime.inherits(QStringLiteral("application/pdf")))
        return new PdfViewer(path, parent);
    if (name.startsWith(QLatin1String("image/"))
        || QImageReader::supportedImageFormats().contains(info.suffix().toLower().toLatin1())) {
        if (!QImageReader(path).format().isEmpty())
            return new ImageViewer(path, parent);
    }
    if (name.startsWith(QLatin1String("video/")) || name == QLatin1String("application/vnd.rn-realmedia"))
        return new MediaViewer(path, true, autoplay, parent);
    if (name.startsWith(QLatin1String("audio/")))
        return new MediaViewer(path, false, autoplay, parent);
    if (name.startsWith(QLatin1String("font/")) || inheritsAny(mime, {"application/x-font-ttf", "application/x-font-otf"}))
        return new FontViewer(path, parent);
    if (inheritsAny(mime, {"application/vnd.oasis.opendocument.text", "application/vnd.oasis.opendocument.spreadsheet",
                           "application/vnd.oasis.opendocument.presentation", "application/msword",
                           "application/vnd.ms-excel", "application/vnd.ms-powerpoint",
                           "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
                           "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
                           "application/vnd.openxmlformats-officedocument.presentationml.presentation",
                           "application/rtf"})) {
        const QString office = QStandardPaths::findExecutable(QStringLiteral("libreoffice"));
        if (!office.isEmpty())
            return new OfficeViewer(path, office, parent);
    }
    if (inheritsAny(mime, {"application/zip", "application/x-tar", "application/x-compressed-tar",
                           "application/x-bzip-compressed-tar", "application/x-xz-compressed-tar",
                           "application/x-zstd-compressed-tar", "application/x-7z-compressed", "application/vnd.rar",
                           "application/x-rar", "application/x-cd-image", "application/x-cpio",
                           "application/vnd.debian.binary-package", "application/x-rpm", "application/x-archive",
                           "application/x-xar", "application/vnd.ms-cab-compressed"})
        && !inheritsAny(mime, {"application/vnd.oasis.opendocument.text", "application/epub+zip"}))
        return new ArchiveViewer(path, parent);
    if (mime.inherits(QStringLiteral("text/markdown")))
        return new TextViewer(path, TextViewer::Markdown, parent);
    if (mime.inherits(QStringLiteral("text/html")))
        return new TextViewer(path, TextViewer::Html, parent);
    if (mime.inherits(QStringLiteral("text/plain")) || looksLikeText(path))
        return new TextViewer(path, TextViewer::Plain, parent);
    return new InfoViewer(path, parent);
}

void prewarmViewers()
{
    // The first QMediaPlayer loads the FFmpeg backend and audio devices
    // (over a second); later ones are created in a few milliseconds.
    QMediaPlayer player;
    QAudioOutput audio;
    player.setAudioOutput(&audio);
    syntaxRepository();
    QImageReader::supportedImageFormats();
}
