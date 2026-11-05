#include "chrome.h"

#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>

namespace Chrome
{

bool isDark(const QPalette &palette)
{
    return palette.color(QPalette::Window).lightness() < 128;
}

QColor withAlpha(QColor color, int alpha)
{
    color.setAlpha(alpha);
    return color;
}

QColor mix(const QColor &a, const QColor &b, qreal amount)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * amount, a.greenF() + (b.greenF() - a.greenF()) * amount,
                            a.blueF() + (b.blueF() - a.blueF()) * amount, a.alphaF() + (b.alphaF() - a.alphaF()) * amount);
}

QColor mutedText(const QPalette &palette)
{
    return mix(palette.color(QPalette::WindowText), palette.color(QPalette::Window), 0.4);
}

QFont scaledFont(const QFont &base, qreal factor, int weight)
{
    QFont font(base);
    font.setPointSizeF(base.pointSizeF() * factor);
    if (weight >= 0)
        font.setWeight(QFont::Weight(weight));
    return font;
}

QPainterPath roundedPath(const QRectF &rect, qreal radius)
{
    QPainterPath path;
    path.addRoundedRect(rect, radius, radius);
    return path;
}

void drawShadow(QPainter &p, const QRectF &rect, qreal radius, int spread, int strength, qreal offsetY)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(Qt::NoBrush);
    // Nothing may land on the content itself: it is translucent.
    QPainterPath outside;
    outside.addRect(rect.adjusted(-spread, -spread, spread, spread + offsetY));
    p.setClipPath(outside - roundedPath(rect, radius));
    for (int i = 0; i < spread; ++i) {
        const qreal falloff = 1.0 - qreal(i) / spread;
        const qreal grow = i + 0.5;
        p.setPen(QPen(QColor(0, 0, 0, int(strength * falloff * falloff)), 1.0));
        p.drawPath(roundedPath(rect.adjusted(-grow, -grow + offsetY, grow, grow + offsetY), radius + grow));
    }
    p.restore();
}

int chipHeight(const QFont &base)
{
    return QFontMetrics(scaledFont(base, 0.86)).height() + 4;
}

int drawChips(QPainter &p, const QRect &rect, const QStringList &chips, const QPalette &palette, Qt::Alignment align)
{
    constexpr int PadX = 7;
    constexpr int Gap = 6;
    const QFont font = scaledFont(p.font(), 0.86);
    const QFontMetrics fm(font);
    const int height = fm.height() + 4;

    // Measure the chips that fit, then place the row according to align.
    QList<QPair<QString, int>> fitting;
    int total = 0;
    for (const QString &chip : chips) {
        const int width = fm.horizontalAdvance(chip) + 2 * PadX;
        if (total + width + (fitting.isEmpty() ? 0 : Gap) > rect.width())
            break;
        total += width + (fitting.isEmpty() ? 0 : Gap);
        fitting.append({chip, width});
    }
    int x = rect.left();
    if (align & Qt::AlignHCenter)
        x = rect.left() + (rect.width() - total) / 2;
    else if (align & Qt::AlignRight)
        x = rect.right() - total;

    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setFont(font);
    const bool dark = isDark(palette);
    const QColor fill = withAlpha(palette.color(QPalette::WindowText), dark ? 26 : 18);
    for (const auto &[text, width] : fitting) {
        const QRectF chip(x, rect.top(), width, height);
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawRoundedRect(chip, height / 2.0, height / 2.0);
        p.setPen(mutedText(palette));
        p.drawText(chip, Qt::AlignCenter, text);
        x += width + Gap;
    }
    p.restore();
    return height;
}

} // namespace Chrome

// ------------------------------------------------------------- PillButton

namespace
{
constexpr int PillHeight = 30;
constexpr int PillPad = 12;
constexpr int PillIcon = 16;
} // namespace

PillButton::PillButton(Style style, QWidget *parent)
    : QPushButton(parent)
    , m_style(style)
{
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::PointingHandCursor);
    setIconSize(QSize(PillIcon, PillIcon));
    setFont(Chrome::scaledFont(font(), 1.0, style == Accent ? QFont::DemiBold : QFont::Medium));
}

void PillButton::setGlyph(Glyph glyph)
{
    m_glyph = glyph;
    update();
}

QSize PillButton::sizeHint() const
{
    if (text().isEmpty())
        return QSize(PillHeight, PillHeight);
    const int iconWidth = icon().isNull() ? 0 : PillIcon + 6;
    return QSize(2 * PillPad + iconWidth + fontMetrics().horizontalAdvance(text()), PillHeight);
}

void PillButton::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPalette pal = palette();
    const bool dark = Chrome::isDark(pal);
    const bool hover = underMouse() && isEnabled();
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const qreal radius = r.height() / 2;

    QColor fill;
    QColor fg;
    if (m_style == Accent) {
        const QColor accent = pal.color(QPalette::Highlight);
        fill = isDown() ? accent.darker(115) : hover ? accent.lighter(110) : accent;
        fg = pal.color(QPalette::HighlightedText);
    } else {
        const int alpha = isDown() ? (dark ? 70 : 50) : hover ? (dark ? 40 : 28) : 0;
        fill = Chrome::withAlpha(pal.color(QPalette::WindowText), alpha);
        fg = pal.color(QPalette::WindowText);
    }
    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    p.drawRoundedRect(r, radius, radius);

    if (m_glyph == Close) {
        // A crisp "×" drawn by hand, so it matches in every icon theme.
        const QPointF c = r.center();
        const qreal arm = 4.5;
        p.setPen(QPen(fg, 1.8, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(c + QPointF(-arm, -arm), c + QPointF(arm, arm));
        p.drawLine(c + QPointF(-arm, arm), c + QPointF(arm, -arm));
        return;
    }

    qreal x = r.left() + PillPad;
    if (text().isEmpty())
        x = r.center().x() - PillIcon / 2.0;
    if (!icon().isNull()) {
        const QRect iconRect(int(x), int(r.center().y() - PillIcon / 2.0), PillIcon, PillIcon);
        icon().paint(&p, iconRect, Qt::AlignCenter, QIcon::Normal);
        x += PillIcon + 6;
    }
    if (!text().isEmpty()) {
        p.setPen(fg);
        p.drawText(QRectF(x, r.top(), r.right() - PillPad - x, r.height()), Qt::AlignVCenter | Qt::AlignLeft,
                   text());
    }
}
