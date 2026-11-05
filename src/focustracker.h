#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

typedef struct _AtspiAccessible AtspiAccessible;
typedef struct _AtspiEvent AtspiEvent;
typedef struct _AtspiEventListener AtspiEventListener;

// Follows keyboard focus over the accessibility bus (AT-SPI) and reports
// which file, if any, is focused in a Dolphin file view.
//
// Dolphin publishes each view as a list described as "in location <url>"
// whose items are named after the files. It only does so when Qt
// accessibility is on (QT_LINUX_ACCESSIBILITY_ALWAYS_ON=1).
class FocusTracker : public QObject
{
    Q_OBJECT
public:
    explicit FocusTracker(QObject *parent = nullptr);
    ~FocusTracker() override;

    // Path of the focused file, or empty when focus is not on a local file
    // in a Dolphin view. Re-checks that the item still has focus.
    QString currentPath();

Q_SIGNALS:
    // Focus moved; path is empty when it is no longer on a Dolphin file.
    void currentChanged(const QString &path);

private:
    static void onEvent(AtspiEvent *event, void *self);
    void handle(AtspiEvent *event);
    void setItem(AtspiAccessible *item);

    AtspiEventListener *m_listener = nullptr;
    AtspiAccessible *m_item = nullptr;
    QTimer m_lossCheck; // see handle()
    QString m_path;
};
