#include "globalshortcut.h"

#include <QDBusConnection>
#include <QDBusMessage>

namespace
{
const QString Service = QStringLiteral("org.kde.kglobalaccel");
const QString Component = QStringLiteral("quicklook");
const QString ComponentLabel = QStringLiteral("Quick Look");

// kglobalaccel's KGlobalAccel::SetShortcutFlag values.
constexpr uint SetPresent = 0x2;
constexpr uint NoAutoloading = 0x4;
} // namespace

GlobalShortcut::GlobalShortcut(const QString &action, const QString &label, int key, QObject *parent)
    : QObject(parent)
    , m_action(action)
    , m_label(label)
    , m_key(key)
{
    QDBusConnection::sessionBus().connect(Service, QStringLiteral("/component/") + Component,
                                          QStringLiteral("org.kde.kglobalaccel.Component"),
                                          QStringLiteral("globalShortcutPressed"), this,
                                          SLOT(onPressed(QString, QString, qlonglong)));
    // Release the key even if an earlier run died while holding it.
    call(QStringLiteral("setInactive"), {actionId()});
}

GlobalShortcut::~GlobalShortcut()
{
    setEnabled(false);
}

QStringList GlobalShortcut::actionId() const
{
    return {Component, m_action, ComponentLabel, m_label};
}

void GlobalShortcut::call(const QString &method, const QList<QVariant> &args)
{
    auto msg = QDBusMessage::createMethodCall(Service, QStringLiteral("/kglobalaccel"),
                                              QStringLiteral("org.kde.KGlobalAccel"), method);
    msg.setArguments(args);
    // Blocking keeps enable/disable strictly ordered; kglobalaccel lives in
    // KWin and answers within a millisecond or two.
    QDBusConnection::sessionBus().call(msg, QDBus::Block, 1000);
}

void GlobalShortcut::setEnabled(bool enabled)
{
    if (enabled == m_enabled)
        return;
    m_enabled = enabled;
    if (enabled) {
        call(QStringLiteral("doRegister"), {actionId()});
        call(QStringLiteral("setShortcut"),
             {actionId(), QVariant::fromValue(QList<int>{m_key}), SetPresent | NoAutoloading});
    } else {
        call(QStringLiteral("setInactive"), {actionId()});
    }
}

void GlobalShortcut::onPressed(const QString &component, const QString &action, qlonglong)
{
    if (component == Component && action == m_action && m_enabled)
        Q_EMIT pressed();
}
