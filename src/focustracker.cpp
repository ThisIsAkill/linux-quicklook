#include "focustracker.h"

#include <QDir>
#include <QLoggingCategory>
#include <QFileInfo>
#include <QRegularExpression>
#include <QUrl>

#include <atspi/atspi.h>

Q_LOGGING_CATEGORY(lcFocus, "quicklook.focus", QtWarningMsg)

namespace
{

// Takes ownership of a g_malloc'ed string.
QString takeString(gchar *s)
{
    QString result = QString::fromUtf8(s ? s : "");
    g_free(s);
    return result;
}

QString appName(AtspiAccessible *acc)
{
    AtspiAccessible *app = atspi_accessible_get_application(acc, nullptr);
    if (!app)
        return {};
    QString name = takeString(atspi_accessible_get_name(app, nullptr)).toLower();
    g_object_unref(app);
    return name;
}

bool hasFocus(AtspiAccessible *acc)
{
    AtspiStateSet *states = atspi_accessible_get_state_set(acc);
    if (!states)
        return false;
    bool focused = atspi_state_set_contains(states, ATSPI_STATE_FOCUSED);
    g_object_unref(states);
    return focused;
}

bool isItemRole(AtspiRole role)
{
    switch (role) {
    case ATSPI_ROLE_LIST_ITEM:
    case ATSPI_ROLE_TABLE_CELL:
    case ATSPI_ROLE_TREE_ITEM:
    case ATSPI_ROLE_TABLE_ROW:
    case ATSPI_ROLE_ICON:
        return true;
    default:
        return false;
    }
}

// The file name an item stands for. In the details view a row's first cell
// carries the name.
QString itemName(AtspiAccessible *item)
{
    if (atspi_accessible_get_role(item, nullptr) == ATSPI_ROLE_TABLE_ROW
        && atspi_accessible_get_child_count(item, nullptr) > 0) {
        AtspiAccessible *cell = atspi_accessible_get_child_at_index(item, 0, nullptr);
        if (cell) {
            QString name = takeString(atspi_accessible_get_name(cell, nullptr));
            g_object_unref(cell);
            return name;
        }
    }
    return takeString(atspi_accessible_get_name(item, nullptr));
}

// The view (list/table) that holds item: its closest ancestor whose
// description ends in a URL. Returns a new reference or nullptr.
AtspiAccessible *viewOf(AtspiAccessible *item, QString *folder)
{
    static const QRegularExpression urlAtEnd(QStringLiteral("([a-z][a-z0-9+.-]*:/.*)$"));
    AtspiAccessible *acc = atspi_accessible_get_parent(item, nullptr);
    for (int depth = 0; acc && depth < 4; ++depth) {
        const auto match = urlAtEnd.match(takeString(atspi_accessible_get_description(acc, nullptr)));
        if (match.hasMatch()) {
            // Dolphin shows the URL for display: usually decoded, but cope with either form.
            const QString text = match.captured(1);
            QUrl url(text, QUrl::TolerantMode);
            if (!url.isLocalFile()) {
                g_object_unref(acc);
                return nullptr;
            }
            QString path = url.toLocalFile();
            const QString raw = text.mid(text.indexOf(QLatin1String(":/")) + 1);
            if (!QFileInfo::exists(path) && raw.startsWith(QLatin1String("///")))
                path = raw.mid(2);
            *folder = path;
            return acc;
        }
        AtspiAccessible *parent = atspi_accessible_get_parent(acc, nullptr);
        g_object_unref(acc);
        acc = parent;
    }
    if (acc)
        g_object_unref(acc);
    return nullptr;
}

QString pathOf(AtspiAccessible *item)
{
    QString folder;
    AtspiAccessible *view = viewOf(item, &folder);
    if (!view)
        return {};
    g_object_unref(view);
    const QString name = itemName(item);
    return name.isEmpty() ? QString() : QDir(folder).filePath(name);
}

} // namespace

FocusTracker::FocusTracker(QObject *parent)
    : QObject(parent)
{
    atspi_init();
    // Qt's event loop runs on GLib's default main context, which libatspi uses.
    m_listener = atspi_event_listener_new(&FocusTracker::onEvent, this, nullptr);
    m_lossCheck.setSingleShot(true);
    m_lossCheck.setInterval(150);
    connect(&m_lossCheck, &QTimer::timeout, this, [this] {
        if (m_item && !hasFocus(m_item))
            setItem(nullptr);
    });
    for (const char *kind : {"object:state-changed:focused", "window:deactivate", "window:activate"})
        atspi_event_listener_register(m_listener, kind, nullptr);
}

FocusTracker::~FocusTracker()
{
    setItem(nullptr);
    if (m_listener) {
        atspi_event_listener_deregister(m_listener, "object:state-changed:focused", nullptr);
        atspi_event_listener_deregister(m_listener, "window:deactivate", nullptr);
        atspi_event_listener_deregister(m_listener, "window:activate", nullptr);
        g_object_unref(m_listener);
    }
}

void FocusTracker::onEvent(AtspiEvent *event, void *self)
{
    static_cast<FocusTracker *>(self)->handle(event);
    g_boxed_free(ATSPI_TYPE_EVENT, event);
}

void FocusTracker::handle(AtspiEvent *event)
{
    AtspiAccessible *src = event->source;
    if (!src)
        return;
    const QByteArray type(event->type);
    qCDebug(lcFocus) << type << event->detail1 << appName(src)
                     << atspi_accessible_get_role(src, nullptr) << takeString(atspi_accessible_get_name(src, nullptr));

    if (type.startsWith("window:")) {
        if (appName(src) != QLatin1String("dolphin"))
            return;
        if (type == "window:deactivate")
            setItem(nullptr);
        return; // on activation the focused item sends its own focus event
    }

    if (!event->detail1) {
        // Moving between files sends "old item lost focus" before "new item
        // gained focus". Clearing right away would close the panel on every
        // arrow key, so only clear if nothing has gained focus shortly after.
        if (m_item && src == m_item)
            m_lossCheck.start();
        return;
    }
    m_lossCheck.stop();

    if (appName(src) == QLatin1String("dolphin") && isItemRole(atspi_accessible_get_role(src, nullptr)))
        setItem(src);
    else
        setItem(nullptr);
}

void FocusTracker::setItem(AtspiAccessible *item)
{
    QString path = item ? pathOf(item) : QString();
    if (path.isEmpty())
        item = nullptr;
    if (item)
        g_object_ref(item);
    if (m_item)
        g_object_unref(m_item);
    m_item = item;
    if (path != m_path) {
        m_path = path;
        Q_EMIT currentChanged(path);
    }
}

QString FocusTracker::currentPath()
{
    if (m_item && !hasFocus(m_item))
        setItem(nullptr);
    return m_path;
}
