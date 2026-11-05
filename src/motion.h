#pragma once

#include <QEasingCurve>
#include <QGraphicsEffect>
#include <QPixmap>
#include <QVariantAnimation>
#include <QWidget>

#include <functional>

// The motion shared by Quick Look and Lumen: one set of durations and
// easings, so a card, a panel and a highlight all move with the same
// character (quick to start, soft to land, a little springy), plus the
// small helpers that animate them.
//
// Every duration is scaled by KDE's animation speed setting (kdeglobals
// [KDE] AnimationDurationFactor): the slider in System Settings → Workspace
// Behavior → General Behavior. At 0 ("instant") nothing animates and every
// change lands at once. Nothing here runs a timer while nothing moves.
namespace Motion
{
// Base durations in milliseconds at the default speed (factor 1).
constexpr int Open = 200;          // a card or panel appearing
constexpr int Close = 120;         // one disappearing: quicker than appearing, so closing feels instant
constexpr int Glide = 150;         // the selection highlight moving to another row
constexpr int Settle = 170;        // a new row settling into the list
constexpr int SettleStagger = 14;  // the delay from one new row to the next
constexpr int Slide = 190;         // a view's content sliding in
constexpr int Fade = 160;          // a cross-fade between two contents
constexpr int Toast = 190;         // a message pill sliding in or out
constexpr int Press = 170;         // a key cap released after a press
constexpr int Resize = 230;        // a card changing width
constexpr int Pulse = 380;         // a highlight flashing to confirm something

// KDE's AnimationDurationFactor (1 at the default speed, 0 when animations
// are turned off). Read from kdeglobals and cached for a moment.
qreal factor();
inline bool enabled()
{
    return factor() > 0;
}
// A base duration at the current speed.
int ms(int base);

// Easings. An entrance covers most of its distance early and lands softly;
// an exit is the same shape (a short exit that eased in would look like a
// delay). The spring overshoots a little before settling, for things that
// pop in. The glide is gentler than the entrance, so a highlight is seen
// travelling.
QEasingCurve entrance(); // OutQuint
QEasingCurve exit();     // OutCubic
QEasingCurve spring();   // OutBack with a small overshoot
QEasingCurve glide();    // OutQuart

inline qreal lerp(qreal from, qreal to, qreal t)
{
    return from + (to - from) * t;
}

// How something looks part-way through appearing: it springs from a
// smaller size (and from a little above, when `fromDy` is negative) to its
// place while fading in over the first part of the motion, so it is fully
// visible by the time it overshoots and settles.
struct Pose {
    qreal scale = 1;
    qreal opacity = 1;
    qreal dy = 0;
};
Pose appearing(qreal t, qreal fromScale = 0.94, qreal fromDy = 0);
} // namespace Motion

// One animated value. start() runs it from one value to another over a
// base duration (scaled by the speed setting; with animations off it jumps
// straight to the end), calling onStep with each value and onDone at the
// end. Starting again while running takes over from the current value, so
// a repeated key never makes the motion lag behind.
class Tween : public QObject
{
public:
    explicit Tween(QObject *parent = nullptr);

    void start(qreal from, qreal to, int baseMs, const QEasingCurve &curve = Motion::entrance());
    // From wherever the value is now.
    void to(qreal target, int baseMs, const QEasingCurve &curve = Motion::entrance());
    // Lands on a value without animating (onStep is called, onDone is not).
    void jump(qreal value);
    void stop();

    qreal value() const { return m_value; }
    bool isRunning() const { return m_animation.state() == QAbstractAnimation::Running; }

    std::function<void(qreal)> onStep;
    std::function<void()> onDone;

private:
    QVariantAnimation m_animation;
    qreal m_value = 0;
};

// Draws a widget scaled about its centre, shifted vertically and faded:
// the pose of a card while it appears. Installed only for the duration of
// that animation (effects make every repaint go through a pixmap).
class PoseEffect : public QGraphicsEffect
{
public:
    using QGraphicsEffect::QGraphicsEffect;

    void setPose(const Motion::Pose &pose);
    QRectF boundingRectFor(const QRectF &rect) const override;

protected:
    void draw(QPainter *painter) override;

private:
    Motion::Pose m_pose;
};

// A picture of a widget that outlives it for a moment: taken just before
// the widget hides, shown in its place, and shrunk and faded away. The
// widget itself can hide at once (its window gives up focus and input
// immediately); only its image lingers. A top-level widget gets a ghost
// window of its own (a layer-shell surface of the same shape on Wayland)
// that takes no input; a child widget gets a sibling in its parent.
class Ghost : public QWidget
{
public:
    // `region` is the part of `widget` worth keeping (a card without the
    // empty surface around it); it shrinks towards its own centre.
    static void dismiss(QWidget *widget, const QRect &region, int baseMs = Motion::Close);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    Ghost(QWidget *widget, const QRect &region);

    QPixmap m_picture;
    QRect m_region;
    Tween m_tween;
};

// A cross-fade for content that is replaced: a picture of `area` of the
// parent as it looks now, laid over it and faded out while the new content
// paints underneath. Call it before replacing the content; when the area
// then moves or changes size, setGeometry() puts the fade over the new
// place (the picture stays anchored at the top left). Returns nullptr
// when there is nothing to fade (animations off, parent hidden).
class FadeOut : public QWidget
{
public:
    static FadeOut *begin(QWidget *parent, const QRect &area, int baseMs = Motion::Fade);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    explicit FadeOut(QWidget *parent, const QRect &area);

    QPixmap m_picture;
    Tween m_tween;
};
