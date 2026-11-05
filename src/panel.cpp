#include "panel.h"
#include "chrome.h"
#include "viewers.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QLocale>
#include <QLoggingCategory>
#include <QMimeDatabase>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QVBoxLayout>
#include <QWindow>

#include <KApplicationTrader>
#include <KIO/OpenUrlJob>
#include <LayerShellQt/Window>

Q_LOGGING_CATEGORY(lcPanel, "quicklook.panel", QtWarningMsg)

namespace
{
constexpr int Radius = 14;       // body corner radius
constexpr int Shadow = 24;       // transparent margin around the body for the shadow
constexpr int ShadowDrop = 4;    // how far the shadow sits below the body
constexpr int Pad = 10;          // body edge to well
constexpr int HeaderHeight = 58; // name and chips rows, including the top padding
constexpr int IconSize = 26;
const QSize MinContent(440, 260);

bool onWayland()
{
    return QGuiApplication::platformName() == QLatin1String("wayland");
}

// "Modified today, 08:31" rather than a full timestamp in a chip.
QString modifiedChip(const QDateTime &when)
{
    const QLocale locale;
    const QDate today = QDate::currentDate();
    const QString time = locale.toString(when.time(), QLocale::ShortFormat);
    if (when.date() == today)
        return Panel::tr("Modified today, %1").arg(time);
    if (when.date() == today.addDays(-1))
        return Panel::tr("Modified yesterday, %1").arg(time);
    return Panel::tr("Modified %1").arg(locale.toString(when.date(), QStringLiteral("d MMM yyyy")));
}
} // namespace

// The header's text block: icon, file name (elided in the middle when it is
// too long) and a row of metadata chips beneath it.
class FileTitle : public QWidget
{
public:
    using QWidget::QWidget;

    void setFile(const QIcon &icon, const QString &name)
    {
        m_icon = icon;
        m_name = name;
        update();
    }

    void setChips(const QStringList &chips)
    {
        m_chips = chips;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const QFont nameFont = Chrome::scaledFont(font(), 1.06, QFont::DemiBold);
        const int nameHeight = QFontMetrics(nameFont).height();
        const int chipsHeight = Chrome::chipHeight(font());
        const int top = (height() - nameHeight - 3 - chipsHeight) / 2;

        const QRect iconRect(0, (height() - IconSize) / 2, IconSize, IconSize);
        m_icon.paint(&p, iconRect);
        const int textLeft = IconSize + 10;

        p.setFont(nameFont);
        p.setPen(palette().color(QPalette::WindowText));
        const QRect nameRect(textLeft, top, width() - textLeft, nameHeight);
        p.drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                   QFontMetrics(nameFont).elidedText(m_name, Qt::ElideMiddle, nameRect.width()));

        p.setFont(font());
        Chrome::drawChips(p, QRect(textLeft, top + nameHeight + 3, width() - textLeft, chipsHeight), m_chips,
                          palette());
    }

private:
    QIcon m_icon;
    QString m_name;
    QStringList m_chips;
};

Panel::Panel(Mode mode, QWidget *parent)
    : QWidget(parent)
    , m_mode(mode)
    , m_title(new FileTitle(this))
    , m_open(new PillButton(PillButton::Accent, this))
    , m_layout(new QVBoxLayout(this))
{
    setWindowTitle(tr("Quick Look"));
    setAttribute(Qt::WA_TranslucentBackground);
    if (m_mode != Embedded)
        setWindowFlag(Qt::FramelessWindowHint);

    auto *close = new PillButton(PillButton::Ghost, this);
    close->setGlyph(PillButton::Close);
    close->setToolTip(tr("Close (Space)"));
    connect(close, &QPushButton::clicked, this, &QWidget::hide);
    connect(m_open, &QPushButton::clicked, this, &Panel::openExternally);
    m_title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    auto *header = new QWidget(this);
    header->setFixedHeight(HeaderHeight);
    auto *headerRow = new QHBoxLayout(header);
    headerRow->setContentsMargins(0, Pad, 0, 0);
    headerRow->setSpacing(8);
    headerRow->addWidget(close);
    headerRow->addWidget(m_title, 1);
    headerRow->addWidget(m_open);

    m_layout->setContentsMargins(Shadow + Pad, Shadow, Shadow + Pad, Shadow + Pad);
    m_layout->setSpacing(0);
    m_layout->addWidget(header);

    if (m_mode == Overlay)
        setupOverlay();
}

// The panel floats above other windows without taking keyboard focus, so
// the file manager keeps handling arrow keys and the panel follows along.
// It uses the overlay layer so it also appears above the Lumen launcher,
// which previews files with it.
void Panel::setupOverlay()
{
    if (onWayland()) {
        create();
        auto *layer = LayerShellQt::Window::get(windowHandle());
        layer->setLayer(LayerShellQt::Window::LayerOverlay);
        layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
        layer->setAnchors({}); // centred
        layer->setWantsToBeOnActiveScreen(true);
        layer->setScope(QStringLiteral("quicklook"));
    } else {
        setWindowFlags(windowFlags() | Qt::Tool | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_X11DoNotAcceptFocus);
    }
}

void Panel::showFile(const QString &path)
{
    if (path == m_path && m_viewer && isVisible())
        return;
    m_path = path;

    const QFileInfo info(path);
    const QMimeType mime = QMimeDatabase().mimeTypeForFile(info);
    m_title->setFile(QIcon::fromTheme(mime.iconName(), QIcon::fromTheme(mime.genericIconName())), info.fileName());
    m_title->setToolTip(path);
    m_fileChips = {mime.comment()};
    if (!info.isDir())
        m_fileChips << QLocale().formattedDataSize(info.size());
    m_fileChips << modifiedChip(info.lastModified());

    const KService::Ptr app = KApplicationTrader::preferredService(mime.name());
    m_open->setText(app ? tr("Open with %1").arg(app->name()) : tr("Open"));
    m_open->setIcon(app ? QIcon::fromTheme(app->icon()) : QIcon::fromTheme(QStringLiteral("document-open")));
    m_open->setVisible(!info.isDir() || app);

    QElapsedTimer timer;
    timer.start();
    delete m_viewer; // stops any playing media
    m_viewer = createViewer(path, this);
    connect(m_viewer, &Viewer::naturalSizeChanged, this, &Panel::fitToContent);
    connect(m_viewer, &Viewer::detailsChanged, this, &Panel::updateChips);
    m_layout->addWidget(m_viewer, 1);
    fitToContent();
    updateChips();
    qCDebug(lcPanel) << "showing" << path << "built in" << timer.elapsed() << "ms";
    if (!isVisible()) {
        show();
        if (m_mode == Window) {
            raise();
            activateWindow();
        }
    }
}

// Kind, then what the viewer knows (dimensions, duration, pages…), then size and date.
void Panel::updateChips()
{
    QStringList chips = m_fileChips.mid(0, 1);
    if (m_viewer)
        chips += m_viewer->details();
    m_title->setChips(chips + m_fileChips.mid(1));
}

// Images and videos get a panel of their own shape; documents get a tall
// page-shaped panel. Both stay within most of the screen.
void Panel::fitToContent()
{
    const QScreen *screen = this->screen() ? this->screen() : QGuiApplication::primaryScreen();
    const QSize avail = screen->availableGeometry().size();
    const QSize chrome(2 * (Shadow + Pad), 2 * Shadow + HeaderHeight + Pad);
    const QSize maxContent = QSize(avail.width() * 7 / 10, avail.height() * 8 / 10) - chrome;

    QSize content;
    const QSize natural = m_viewer ? m_viewer->naturalSize() : QSize();
    if (natural.isValid()) {
        content = natural.width() > maxContent.width() || natural.height() > maxContent.height()
            ? natural.scaled(maxContent, Qt::KeepAspectRatio)
            : natural;
        content = content.expandedTo(MinContent);
    } else {
        content = QSize(qMin(maxContent.width(), qMax(avail.width() * 45 / 100, 720)), maxContent.height());
    }
    const QSize target = content + chrome;
    if (target != size()) {
        resize(target);
        if (m_mode == Overlay && !onWayland())
            move(screen->availableGeometry().center() - rect().center());
    }
    if (m_mode == Embedded && parentWidget())
        move(parentWidget()->rect().center() - rect().center());
}

void Panel::openExternally()
{
    if (m_path.isEmpty())
        return;
    auto *job = new KIO::OpenUrlJob(QUrl::fromLocalFile(m_path));
    job->start();
    hide();
}

QRect Panel::bodyRect() const
{
    return rect().adjusted(Shadow, Shadow, -Shadow, -Shadow);
}

QRect Panel::wellRect() const
{
    const QRect body = bodyRect();
    return QRect(body.left() + Pad, body.top() + HeaderHeight, body.width() - 2 * Pad,
                 body.height() - HeaderHeight - Pad);
}

void Panel::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPalette pal = palette();
    const bool dark = Chrome::isDark(pal);
    const QRectF body = QRectF(bodyRect()).adjusted(0.5, 0.5, -0.5, -0.5);

    Chrome::drawShadow(p, body, Radius, Shadow - ShadowDrop, dark ? 120 : 85, ShadowDrop);

    // Translucent body with a faint top-to-bottom gradient, like tinted glass.
    const QColor base = Chrome::withAlpha(Chrome::mix(pal.color(QPalette::Window), pal.color(QPalette::Base), dark ? 0.0 : 0.3), 244);
    QLinearGradient glass(body.topLeft(), body.bottomLeft());
    glass.setColorAt(0.0, Chrome::mix(base, Qt::white, dark ? 0.07 : 0.3));
    glass.setColorAt(1.0, base);
    const QPainterPath shape = Chrome::roundedPath(body, Radius);
    p.fillPath(shape, glass);

    // A soft accent wash behind the header ties the chrome to the system colour.
    QLinearGradient wash(body.topLeft(), QPointF(body.left(), body.top() + HeaderHeight + Pad));
    wash.setColorAt(0.0, Chrome::withAlpha(pal.color(QPalette::Highlight), dark ? 34 : 22));
    wash.setColorAt(1.0, Qt::transparent);
    p.fillPath(shape, wash);

    // Content well: its tone depends on the viewer (paper, stage or screen).
    QColor well = pal.color(QPalette::Base);
    switch (m_viewer ? m_viewer->backdrop() : Viewer::Paper) {
    case Viewer::Paper:
        break;
    case Viewer::Stage:
        well = Chrome::mix(pal.color(QPalette::Window), Qt::black, dark ? 0.28 : 0.05);
        break;
    case Viewer::Screen:
        well = Qt::black;
        break;
    }
    const QRectF wellF = QRectF(wellRect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(Chrome::withAlpha(Qt::black, dark ? 90 : 22));
    p.setBrush(well);
    p.drawRoundedRect(wellF, Chrome::WellRadius, Chrome::WellRadius);

    // Outer border and a hairline highlight along the top edge.
    p.setBrush(Qt::NoBrush);
    p.setPen(Chrome::withAlpha(dark ? Qt::white : Qt::black, dark ? 36 : 40));
    p.drawPath(shape);
    QLinearGradient rim(body.topLeft(), QPointF(body.left(), body.top() + body.height() * 0.4));
    rim.setColorAt(0.0, Chrome::withAlpha(Qt::white, dark ? 30 : 160));
    rim.setColorAt(1.0, Qt::transparent);
    p.setPen(QPen(QBrush(rim), 1.0));
    p.drawPath(Chrome::roundedPath(body.adjusted(1, 1, -1, -1), Radius - 1));
}

void Panel::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // Clicks in the shadow margin should reach the window below, not the
    // panel. The QWindow mask only limits input on Wayland; QWidget::setMask
    // would also clip the painting.
    if (m_mode == Overlay && onWayland() && windowHandle())
        windowHandle()->setMask(QRegion(bodyRect()));
}

void Panel::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Space:
    case Qt::Key_Escape:
        hide();
        return;
    case Qt::Key_Right:
    case Qt::Key_Down:
        Q_EMIT navigateRequested(1);
        return;
    case Qt::Key_Left:
    case Qt::Key_Up:
        Q_EMIT navigateRequested(-1);
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        openExternally();
        return;
    default:
        QWidget::keyPressEvent(event);
    }
}

void Panel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    Q_EMIT visibilityChanged(true);
}

void Panel::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    delete m_viewer; // stop playback and free memory while hidden
    m_path.clear();
    Q_EMIT visibilityChanged(false);
}
