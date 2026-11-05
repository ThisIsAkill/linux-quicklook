#pragma once

#include <QPointer>
#include <QWidget>

class PillButton;
class FileTitle;
class QVBoxLayout;
class Viewer;

// The floating preview panel: a header (file name, metadata chips, "Open
// with" button) above the file's viewer, sized to the content's shape. It
// paints its own chrome: a soft shadow, a translucent rounded body and an
// inset "well" whose tone depends on the content shown.
class Panel : public QWidget
{
    Q_OBJECT
public:
    enum Mode {
        Overlay, // never takes keyboard focus; the file manager keeps it (daemon mode)
        Window,  // an ordinary focused window that handles its own keys (command-line mode)
        Embedded, // a child widget centred in its parent (Lumen draws it in its own surface)
    };

    explicit Panel(Mode mode, QWidget *parent = nullptr);

    // Shows the panel with path, or switches the visible panel to path.
    void showFile(const QString &path);
    QString currentFile() const { return m_path; }

Q_SIGNALS:
    void visibilityChanged(bool visible);
    void navigateRequested(int delta); // Window mode: arrow keys

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void setupOverlay();
    void openExternally();
    void fitToContent();
    void updateChips();
    QRect bodyRect() const;
    QRect wellRect() const;

    Mode m_mode;
    QString m_path;
    QStringList m_fileChips; // kind, size, date: fixed per file
    FileTitle *m_title;
    PillButton *m_open;
    QVBoxLayout *m_layout;
    QPointer<Viewer> m_viewer;
};
