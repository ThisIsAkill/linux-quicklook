#pragma once

#include <QSize>
#include <QStringList>
#include <QWidget>

// Base class of the per-format preview widgets. Viewers paint no background
// of their own: the panel paints a rounded "well" behind them, toned by the
// viewer's backdrop() so that pages read as paper and video as a screen.
class Viewer : public QWidget
{
    Q_OBJECT
public:
    enum Backdrop {
        Paper,  // text and documents: the palette's base colour
        Stage,  // images, cover art, info cards: a muted neutral
        Screen, // video: black
    };

    using QWidget::QWidget;

    // Natural size of the content (image or video pixels), used to size the
    // panel to the content's shape. Invalid for page-like content, which gets
    // a standard document size.
    QSize naturalSize() const { return m_naturalSize; }

    Backdrop backdrop() const { return m_backdrop; }

    // Short facts about the content ("1600 × 1000", "12 pages"), shown as
    // chips in the panel's header.
    QStringList details() const { return m_details; }

Q_SIGNALS:
    void naturalSizeChanged(const QSize &size);
    void detailsChanged(const QStringList &details);

protected:
    void setNaturalSize(const QSize &size);
    void setBackdrop(Backdrop backdrop) { m_backdrop = backdrop; }
    void setDetails(const QStringList &details);

private:
    QSize m_naturalSize;
    QStringList m_details;
    Backdrop m_backdrop = Paper;
};

// Picks the viewer for a file by its type (and contents, for text).
// Video and audio start playing unless autoplay is false (then a video
// shows its first frame).
Viewer *createViewer(const QString &path, QWidget *parent, bool autoplay = true);

// Loads the media backend, syntax definitions and image plugins ahead of
// time, so that the first preview of each kind opens without a delay.
void prewarmViewers();
