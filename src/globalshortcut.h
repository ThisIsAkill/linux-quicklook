#pragma once

#include <QHash>
#include <QObject>

// A KDE global shortcut (through kglobalaccel's D-Bus API) that can be
// switched on and off at runtime. While off, the key is not grabbed and
// reaches applications normally.
class GlobalShortcut : public QObject
{
    Q_OBJECT
public:
    // key is a Qt key code (Qt::Key_Space, ...).
    GlobalShortcut(const QString &action, const QString &label, int key, QObject *parent = nullptr);
    ~GlobalShortcut() override;

    void setEnabled(bool enabled);
    bool isEnabled() const { return m_enabled; }

Q_SIGNALS:
    void pressed();

private Q_SLOTS:
    void onPressed(const QString &component, const QString &action, qlonglong timestamp);

private:
    QStringList actionId() const;
    void call(const QString &method, const QList<QVariant> &args);

    QString m_action;
    QString m_label;
    int m_key;
    bool m_enabled = false;
};
