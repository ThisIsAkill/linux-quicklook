#pragma once

#include <QPushButton>
#include <QWidget>

class QPainter;
class QPainterPath;

// The panel's look: palette-derived colours, soft painted shadows, metadata
// chips and the pill-shaped buttons. Everything is drawn with QPainter from
// the current palette, so it follows light and dark colour schemes.
namespace Chrome
{
constexpr int WellRadius = 10; // corner radius of the content area

bool isDark(const QPalette &palette);
QColor withAlpha(QColor color, int alpha);
QColor mix(const QColor &a, const QColor &b, qreal amount); // 0 → a, 1 → b
QColor mutedText(const QPalette &palette);                  // secondary text
QFont scaledFont(const QFont &base, qreal factor, int weight = -1);
QPainterPath roundedPath(const QRectF &rect, qreal radius);

// A soft shadow outside rect, `spread` px wide and `strength` (0–255) dark
// at the edge, shifted down by `offsetY`. Drawn as thin rings, so it costs
// a few strokes rather than a blur.
void drawShadow(QPainter &p, const QRectF &rect, qreal radius, int spread, int strength, qreal offsetY);

// Small rounded chips for metadata ("JPEG image", "1600 × 1000"). Chips
// that do not fit in rect are left out. Returns the height used.
int drawChips(QPainter &p, const QRect &rect, const QStringList &chips, const QPalette &palette,
              Qt::Alignment align = Qt::AlignLeft);
int chipHeight(const QFont &base);
} // namespace Chrome

// A pill-shaped, keyboard-focus-free button: Accent (filled with the
// highlight colour) for the main action, Ghost (transparent, tinted on
// hover) for secondary ones. Without text it is a round icon button.
class PillButton : public QPushButton
{
    Q_OBJECT
public:
    enum Style { Ghost, Accent };
    enum Glyph { ThemeIcon, Close };

    explicit PillButton(Style style, QWidget *parent = nullptr);
    void setGlyph(Glyph glyph);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    Style m_style;
    Glyph m_glyph = ThemeIcon;
};
