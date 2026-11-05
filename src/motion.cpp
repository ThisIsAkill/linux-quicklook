#include "motion.h"

#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QPainter>
#include <QStandardPaths>
#include <QWindow>

#include <LayerShellQt/Window>

namespace Motion
{

namespace
{
// kdeglobals is small and the setting changes rarely: re-read it at most
// once a second, so a change in System Settings is picked up soon without
// reading the file for every motion.
qreal readFactor()
{
    const QString path = QStandardPaths::locate(QStandardPaths::GenericConfigLocation, QStringLiteral("kdeglobals"));
    QFile file(path);
    if (path.isEmpty() || !file.open(QIODevice::ReadOnly | QIODevice::Text))
        return 1.0;
    bool inKde = false;
    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (line.startsWith(u'['))
            inKde = line == QLatin1String("[KDE]");
        else if (inKde && line.startsWith(QLatin1String("AnimationDurationFactor=")))
            return qMax(0.0, line.section(u'=', 1).trimmed().toDouble());
    }
    return 1.0;
}
} // namespace

qreal factor()
{
    static qreal cached = 1.0;
    static QElapsedTimer age;
    if (!age.isValid() || age.elapsed() > 1000) {
        cached = readFactor();
        age.start();
    }
    return cached;
}

int ms(int base)
{
    return qRound(base * factor());
}

QEasingCurve entrance()
{
    return QEasingCurve(QEasingCurve::OutQuint);
}

QEasingCurve exit()
{
    return QEasingCurve(QEasingCurve::OutCubic);
}

QEasingCurve spring()
{
    QEasingCurve curve(QEasingCurve::OutBack);
    curve.setOvershoot(1.1); // about 4% past the target, then back
    return curve;
}

QEasingCurve glide()
{
    return QEasingCurve(QEasingCurve::OutQuart);
}

Pose appearing(qreal t, qreal fromScale, qreal fromDy)
{
    const qreal sprung = spring().valueForProgress(t);
    Pose pose;
    pose.scale = lerp(fromScale, 1.0, sprung);
    pose.dy = lerp(fromDy, 0.0, sprung);
    pose.opacity = exit().valueForProgress(qMin(1.0, t / 0.6));
    return pose;
}

} // namespace Motion

// ------------------------------------------------------------------ Tween

Tween::Tween(QObject *parent)
    : QObject(parent)
{
    QObject::connect(&m_animation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        m_value = value.toReal();
        if (onStep)
            onStep(m_value);
    });
    QObject::connect(&m_animation, &QVariantAnimation::finished, this, [this] {
        if (onDone)
            onDone();
    });
}

void Tween::start(qreal from, qreal to, int baseMs, const QEasingCurve &curve)
{
    m_animation.stop();
    const int duration = Motion::ms(baseMs);
    if (duration <= 0) {
        jump(to);
        if (onDone)
            onDone();
        return;
    }
    m_value = from;
    m_animation.setStartValue(from);
    m_animation.setEndValue(to);
    m_animation.setDuration(duration);
    m_animation.setEasingCurve(curve);
    m_animation.start();
}

void Tween::to(qreal target, int baseMs, const QEasingCurve &curve)
{
    start(m_value, target, baseMs, curve);
}

void Tween::jump(qreal value)
{
    m_animation.stop();
    m_value = value;
    if (onStep)
        onStep(m_value);
}

void Tween::stop()
{
    m_animation.stop();
}

// ------------------------------------------------------------- PoseEffect

void PoseEffect::setPose(const Motion::Pose &pose)
{
    m_pose = pose;
    updateBoundingRect();
    update();
}

QRectF PoseEffect::boundingRectFor(const QRectF &rect) const
{
    // Room for the overshoot of a spring and the shift, whatever the pose.
    const qreal margin = 0.04 * qMax(rect.width(), rect.height()) + 24;
    return rect.adjusted(-margin, -margin, margin, margin);
}

void PoseEffect::draw(QPainter *painter)
{
    QPoint offset;
    const QPixmap source = sourcePixmap(Qt::LogicalCoordinates, &offset, QGraphicsEffect::NoPad);
    if (source.isNull())
        return;
    const QPointF centre = sourceBoundingRect(Qt::LogicalCoordinates).center();
    painter->save();
    painter->setRenderHint(QPainter::SmoothPixmapTransform);
    painter->setOpacity(painter->opacity() * m_pose.opacity);
    painter->translate(centre.x(), centre.y() + m_pose.dy);
    painter->scale(m_pose.scale, m_pose.scale);
    painter->translate(-centre);
    painter->drawPixmap(offset, source);
    painter->restore();
}

// ------------------------------------------------------------------ Ghost

namespace
{
bool onWayland()
{
    return QGuiApplication::platformName() == QLatin1String("wayland");
}
} // namespace

void Ghost::dismiss(QWidget *widget, const QRect &region, int baseMs)
{
    if (!widget->isVisible() || !Motion::enabled() || region.isEmpty())
        return;
    auto *ghost = new Ghost(widget, region);
    ghost->show();
    ghost->m_tween.onStep = [ghost](qreal) { ghost->update(ghost->m_region.adjusted(-4, -4, 4, 8)); };
    ghost->m_tween.onDone = [ghost] {
        ghost->hide();
        ghost->deleteLater();
    };
    ghost->m_tween.start(0, 1, baseMs, Motion::exit());
}

Ghost::Ghost(QWidget *widget, const QRect &region)
    : QWidget(widget->isWindow() ? nullptr : widget->parentWidget())
    , m_picture(widget->grab(region))
    , m_region(region)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    if (!widget->isWindow()) {
        // A sibling in the same parent, over the place the widget had.
        setGeometry(widget->geometry());
        raise();
        return;
    }
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput);
    setAttribute(Qt::WA_ShowWithoutActivating);
    const QWindow *source = widget->windowHandle();
    const auto *layer = source ? source->findChild<LayerShellQt::Window *>() : nullptr;
    if (onWayland() && layer) {
        // A layer surface like the widget's, so it lands in the same place,
        // but one that never takes keys or clicks. A widget that covers the
        // screen (the launcher's surface) gets a ghost the size of the
        // region alone, pinned where the region was.
        create();
        auto *mine = LayerShellQt::Window::get(windowHandle());
        constexpr auto AllSides = LayerShellQt::Window::AnchorTop | LayerShellQt::Window::AnchorBottom | LayerShellQt::Window::AnchorLeft
            | LayerShellQt::Window::AnchorRight;
        if (layer->anchors() == AllSides) {
            mine->setAnchors(LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop | LayerShellQt::Window::AnchorLeft));
            mine->setMargins(QMargins(region.left(), region.top(), 0, 0));
            resize(region.size());
            m_region.moveTopLeft(QPoint(0, 0));
        } else {
            mine->setAnchors(layer->anchors());
            mine->setMargins(layer->margins());
            resize(widget->size());
        }
        mine->setLayer(layer->layer());
        mine->setExclusiveZone(-1);
        mine->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
        mine->setWantsToBeOnActiveScreen(layer->wantsToBeOnActiveScreen());
        mine->setScope(layer->scope() + QStringLiteral("-ghost"));
    } else {
        setGeometry(QRect(widget->mapToGlobal(region.topLeft()), region.size()));
        m_region.moveTopLeft(QPoint(0, 0));
    }
}

void Ghost::paintEvent(QPaintEvent *)
{
    // Shrinks a little and fades, sinking by a pixel or two: the card is
    // put down rather than switched off.
    const qreal t = m_tween.value();
    const qreal scale = Motion::lerp(1.0, 0.96, t);
    QPainter p(this);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setOpacity(1.0 - t);
    const QPointF centre = QRectF(m_region).center();
    p.translate(centre.x(), centre.y() + 3 * t);
    p.scale(scale, scale);
    p.translate(-centre);
    p.drawPixmap(m_region.topLeft(), m_picture);
}

// ---------------------------------------------------------------- FadeOut

FadeOut *FadeOut::begin(QWidget *parent, const QRect &area, int baseMs)
{
    if (!parent->isVisible() || !Motion::enabled() || area.isEmpty())
        return nullptr;
    auto *fade = new FadeOut(parent, area);
    fade->show();
    fade->raise();
    // Content created after the fade would stack above it: keep it on top.
    fade->m_tween.onStep = [fade](qreal) {
        fade->raise();
        fade->update();
    };
    fade->m_tween.onDone = [fade] {
        fade->hide();
        fade->deleteLater();
    };
    fade->m_tween.start(1, 0, baseMs, Motion::exit());
    return fade;
}

FadeOut::FadeOut(QWidget *parent, const QRect &area)
    : QWidget(parent)
    , m_picture(parent->grab(area))
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setGeometry(area);
}

void FadeOut::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setOpacity(m_tween.value());
    p.drawPixmap(0, 0, m_picture);
}
