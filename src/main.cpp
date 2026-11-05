// quicklook: macOS-style Quick Look for KDE Plasma.
//
//   quicklook --daemon   Run in the background. In Dolphin, Space opens a
//                        preview of the focused file and Space (or Esc)
//                        closes it; arrow keys keep moving through Dolphin
//                        and the preview follows.
//   quicklook FILE       Preview FILE in a window: Space/Esc close it, arrow
//                        keys step through the folder, Enter opens the file.

#include "focustracker.h"
#include "globalshortcut.h"
#include "panel.h"
#include "viewers.h"

#include <QApplication>
#include <QCollator>
#include <QCommandLineParser>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QTimer>

#include <algorithm>

namespace
{

const QString DBusService = QStringLiteral("io.github.quicklook");

// Background mode: wires Dolphin focus, the Space/Esc shortcuts and the panel.
class Daemon : public QObject
{
public:
    Daemon()
        : m_panel(Panel::Overlay)
        , m_space(QStringLiteral("toggle"), QStringLiteral("Preview the focused file"), Qt::Key_Space)
        , m_escape(QStringLiteral("close"), QStringLiteral("Close the preview"), Qt::Key_Escape)
    {
        // Space is only grabbed while a Dolphin file has focus, so renaming,
        // the location bar and every other app still get it as usual.
        connect(&m_tracker, &FocusTracker::currentChanged, this, [this](const QString &path) {
            m_space.setEnabled(!path.isEmpty());
            if (!m_panel.isVisible())
                return;
            if (path.isEmpty())
                m_panel.hide();
            else
                m_panel.showFile(path); // follow Dolphin's arrow keys
        });
        connect(&m_space, &GlobalShortcut::pressed, this, [this] {
            if (m_panel.isVisible()) {
                m_panel.hide();
                return;
            }
            const QString path = m_tracker.currentPath();
            if (path.isEmpty())
                m_space.setEnabled(false);
            else
                m_panel.showFile(path);
        });
        connect(&m_escape, &GlobalShortcut::pressed, &m_panel, &QWidget::hide);
        QTimer::singleShot(0, this, &prewarmViewers);
        connect(&m_panel, &Panel::visibilityChanged, &m_escape, &GlobalShortcut::setEnabled);
    }

private:
    FocusTracker m_tracker;
    Panel m_panel;
    GlobalShortcut m_space;
    GlobalShortcut m_escape;
};

// Files of a folder in Dolphin's default order: folders first, then natural
// name order, hidden files left out unless one is the starting file.
QStringList siblings(const QString &file)
{
    const QFileInfo info(file);
    QFileInfoList entries = info.dir().entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot);
    if (info.isHidden() && !entries.contains(info))
        entries << info;
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(entries.begin(), entries.end(), [&](const QFileInfo &a, const QFileInfo &b) {
        if (a.isDir() != b.isDir())
            return a.isDir();
        return collator.compare(a.fileName(), b.fileName()) < 0;
    });
    QStringList paths;
    for (const QFileInfo &e : entries)
        paths << e.absoluteFilePath();
    return paths;
}

int runDaemon(QApplication &app)
{
    auto bus = QDBusConnection::sessionBus();
    if (!bus.registerService(DBusService)) {
        qWarning("quicklook: already running");
        return 0;
    }
    app.setQuitOnLastWindowClosed(false);
    // KIO jobs (thumbnails, opening files) hold Qt's quit lock; without this
    // the daemon would exit when one finishes while the panel is hidden.
    app.setQuitLockEnabled(false);
    Daemon daemon;
    return app.exec();
}

int runViewer(QApplication &app, const QString &file)
{
    const QString start = QFileInfo(file).absoluteFilePath();
    if (!QFileInfo::exists(start)) {
        qWarning("quicklook: %s: no such file", qPrintable(file));
        return 1;
    }
    QStringList files = siblings(start);
    qsizetype index = files.indexOf(start);
    if (index < 0) {
        files = {start};
        index = 0;
    }
    Panel panel(Panel::Window);
    QObject::connect(&panel, &Panel::navigateRequested, &panel, [&](int delta) {
        index = (index + delta + files.size()) % files.size();
        panel.showFile(files[index]);
    });
    QObject::connect(&panel, &Panel::visibilityChanged, &app, [&app](bool visible) {
        if (!visible)
            app.quit();
    });
    panel.showFile(start);
    return app.exec();
}

} // namespace

int main(int argc, char **argv)
{
    // Previews decode video in software. Probing for hardware decoders (CUDA,
    // VA-API) loads the whole GPU driver stack, about 140 MB that a resident
    // process would otherwise keep for good. Users can still override this.
    if (!qEnvironmentVariableIsSet("QT_FFMPEG_DECODING_HW_DEVICE_TYPES"))
        qputenv("QT_FFMPEG_DECODING_HW_DEVICE_TYPES", ",");
    if (!qEnvironmentVariableIsSet("QT_FFMPEG_ENCODING_HW_DEVICE_TYPES"))
        qputenv("QT_FFMPEG_ENCODING_HW_DEVICE_TYPES", ",");
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("quicklook"));
    app.setDesktopFileName(QStringLiteral("io.github.quicklook"));
    QIcon::setFallbackThemeName(QStringLiteral("breeze"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("macOS-style Quick Look for KDE Plasma"));
    parser.addHelpOption();
    parser.addOption({QStringLiteral("daemon"), QStringLiteral("Run in the background and preview Dolphin files with Space.")});
    parser.addPositionalArgument(QStringLiteral("file"), QStringLiteral("File to preview."));
    parser.process(app);

    if (parser.isSet(QStringLiteral("daemon")))
        return runDaemon(app);
    if (parser.positionalArguments().size() == 1)
        return runViewer(app, parser.positionalArguments().first());
    parser.showHelp(1);
}
