#include "AppController.h"
#include "I18n.h"
#include "NoteWindow.h"
#include "HubWindow.h"
#include "Storage.h"
#include "Themes.h"
#include "UiHelpers.h"

#include <QApplication>
#include <QLocalServer>
#include <QLocalSocket>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QMessageBox>
#include <QRandomGenerator>
#include <QScreen>
#include <QDateTime>
#include <QDebug>

namespace {
const char *kServerName = "sticky-notes-single-instance";
} // namespace

AppController::AppController(QObject *parent)
    : QObject(parent)
{
}

AppController::~AppController() = default;

bool AppController::init(QString *errorOut)
{
    return Storage::instance().open(errorOut);
}

bool AppController::listenForSecondInstance(QString *errorOut)
{
    QLocalServer::removeServer(QString::fromLatin1(kServerName));
    m_server = new QLocalServer(this);
    if (!m_server->listen(QString::fromLatin1(kServerName))) {
        if (errorOut)
            *errorOut = m_server->errorString();
        return false;
    }
    connect(m_server, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket *s = m_server->nextPendingConnection()) {
            connect(s, &QLocalSocket::disconnected, s, &QLocalSocket::deleteLater);
            s->readAll();
            showHub();
        }
    });
    return true;
}

void AppController::start()
{
    m_hub = new HubWindow;
    connect(m_hub, &HubWindow::newNoteRequested, this, &AppController::onNewNote);
    connect(m_hub, &HubWindow::openNoteRequested, this, &AppController::onOpenNote);
    connect(m_hub, &HubWindow::noteVisibilityToggled, this, &AppController::onVisibilityToggled);
    connect(m_hub, &HubWindow::deleteNoteRequested, this, &AppController::onDeleteNote);
    connect(m_hub, &HubWindow::restoreNoteRequested, this, &AppController::onRestoreNote);
    connect(m_hub, &HubWindow::purgeNoteRequested, this, &AppController::onPurgeNote);
    connect(m_hub, &HubWindow::purgeAllRequested, this, &AppController::onPurgeAll);
    connect(m_hub, &HubWindow::settingsChanged, this, &AppController::onSettingsChanged);
    connect(m_hub, &HubWindow::quitRequested, this, [] { QApplication::quit(); });

    for (const Note &n : Storage::instance().notes(false))
        if (n.visible)
            ensureWindow(n);

    m_hub->show();
    m_hub->raise();
    m_hub->activateWindow();

    applyAutostart();
}

void AppController::showHub()
{
    if (!m_hub)
        return;
    m_hub->show();
    m_hub->setWindowState((m_hub->windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
    m_hub->raise();
    m_hub->activateWindow();
}

NoteWindow *AppController::ensureWindow(const Note &note)
{
    if (NoteWindow *existing = m_windows.value(note.id)) {
        existing->show();
        return existing;
    }

    auto *w = new NoteWindow(note);
    m_windows.insert(note.id, w);

    connect(w, &NoteWindow::contentEdited, this, [](qint64 id, const QString &t, const QString &c) {
        Storage::instance().updateContent(id, t, c);
    });
    connect(w, &NoteWindow::geometryEdited, this, [](qint64 id, int x, int y, int ww, int hh) {
        Storage::instance().updateGeometry(id, x, y, ww, hh);
    });
    connect(w, &NoteWindow::themeEdited, this, [](qint64 id, const QString &themeId) {
        Storage::instance().updateTheme(id, themeId);
    });
    connect(w, &NoteWindow::layerEdited, this, [](qint64 id, const QString &layer) {
        Storage::instance().updateLayer(id, layer);
    });
    connect(w, &NoteWindow::collapsedEdited, this, [](qint64 id, bool collapsed) {
        Storage::instance().updateCollapsed(id, collapsed);
    });
    connect(w, &NoteWindow::deleteRequested, this, &AppController::onDeleteNote);
    connect(w, &NoteWindow::hideRequested, this, [this](qint64 id) { onVisibilityToggled(id, false); });
    connect(w, &NoteWindow::newNoteRequested, this, &AppController::onNewNote);
    connect(w, &NoteWindow::duplicateRequested, this, [this](qint64 id) {
        Note src = Storage::instance().note(id);
        if (!src.id)
            return;
        src.id = -1;
        const QPoint p = cascadePosition();
        src.x = p.x();
        src.y = p.y();
        const qint64 newId = Storage::instance().insertNote(src);
        if (newId > 0) {
            Note n = Storage::instance().note(newId);
            ensureWindow(n)->activate();
            if (m_hub)
                m_hub->refresh();
        }
    });
    connect(w, &NoteWindow::settingsRequested, this, &AppController::showHub);

    w->show();
    return w;
}

void AppController::removeWindow(qint64 id)
{
    if (NoteWindow *w = m_windows.take(id))
        w->deleteLater();
}

QPoint AppController::cascadePosition() const
{
    const int n = m_windows.size();
    const QRect screen = QApplication::primaryScreen()->availableGeometry();
    int x = screen.x() + 120 + (n % 10) * 32;
    int y = screen.y() + 100 + (n % 10) * 28;
    x = qBound(screen.x() + 10, x, screen.right() - 320);
    y = qBound(screen.y() + 10, y, screen.bottom() - 280);
    return QPoint(x, y);
}

void AppController::createNewNote()
{
    Note n;

    // Default title: "Note <date/time>", with the format configurable in the
    // settings ("date_format" key).
    const QString dateFormat = Storage::instance().setting(QStringLiteral("date_format"),
                                                           QStringLiteral("dd/MM/yyyy HH:mm"));
    n.title = I18n::t(QStringLiteral("note.default_title")) + QStringLiteral(" ")
              + QDateTime::currentDateTime().toString(dateFormat.isEmpty()
                                                          ? QStringLiteral("dd/MM/yyyy HH:mm")
                                                          : dateFormat);

    // "Random": picks one of the available themes at random for each new note.
    QString themeId = Storage::instance().setting(QStringLiteral("default_theme"), Themes::kRandomId);
    if (themeId == Themes::kRandomId) {
        const QList<Theme> themes = Storage::instance().themes();
        if (!themes.isEmpty())
            themeId = themes.at(QRandomGenerator::global()->bounded(themes.size())).id;
    }
    n.themeId = themeId;

    n.layer = Storage::instance().setting(QStringLiteral("default_layer"), QStringLiteral("bottom"));
    const QPoint p = cascadePosition();
    n.x = p.x();
    n.y = p.y();
    n.w = qBound(120, Storage::instance().setting(QStringLiteral("default_width"),
                                                  QStringLiteral("230")).toInt(), 4000);
    n.h = qBound(100, Storage::instance().setting(QStringLiteral("default_height"),
                                                  QStringLiteral("190")).toInt(), 4000);
    n.visible = true;

    const qint64 id = Storage::instance().insertNote(n);
    if (id <= 0)
        return;
    Note saved = Storage::instance().note(id);
    ensureWindow(saved)->activate();
    if (m_hub)
        m_hub->refresh();
}

void AppController::onNewNote()
{
    createNewNote();
}

void AppController::onOpenNote(qint64 id)
{
    const Note n = Storage::instance().note(id);
    if (!n.id || n.deleted)
        return;
    if (!n.visible) {
        Storage::instance().setVisible(id, true);
        Note shown = Storage::instance().note(id);
        ensureWindow(shown)->activate();
        if (m_hub)
            m_hub->refresh();
        return;
    }
    ensureWindow(n)->activate();
}

void AppController::onVisibilityToggled(qint64 id, bool visible)
{
    Storage::instance().setVisible(id, visible);
    if (visible) {
        Note n = Storage::instance().note(id);
        ensureWindow(n)->activate();
    } else if (NoteWindow *w = m_windows.value(id)) {
        w->setVisibleState(false);
    }
    if (m_hub)
        m_hub->refresh();
}

void AppController::onDeleteNote(qint64 id)
{
    if (Storage::instance().setting(QStringLiteral("confirm_delete"), QStringLiteral("1")) != QLatin1String("0")) {
        const Note n = Storage::instance().note(id);
        const QString name = n.title.trimmed().isEmpty() ? UiHelpers::untitled() : n.title;
        if (QMessageBox::question(m_hub, I18n::t(QStringLiteral("note.delete")),
                                  I18n::t(QStringLiteral("note.confirm_delete")).arg(name))
            != QMessageBox::Yes)
            return;
    }
    Storage::instance().softDelete(id);
    removeWindow(id);
    if (m_hub)
        m_hub->refresh();
}

void AppController::onRestoreNote(qint64 id)
{
    Storage::instance().restore(id);
    if (m_hub)
        m_hub->refresh();
}

void AppController::onPurgeNote(qint64 id)
{
    Storage::instance().purge(id);
    if (m_hub)
        m_hub->refresh();
}

void AppController::onPurgeAll()
{
    for (const Note &n : Storage::instance().notes(true))
        if (n.deleted)
            removeWindow(n.id);
    Storage::instance().purgeAllDeleted();
    if (m_hub)
        m_hub->refresh();
}

void AppController::onSettingsChanged()
{
    for (NoteWindow *w : std::as_const(m_windows))
        w->applyTheme(Storage::instance().note(w->noteId()).themeId);
    applyAutostart();
}

void AppController::applyAutostart()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
                        + QStringLiteral("/autostart");
    QDir().mkpath(dir);
    const QString file = dir + QStringLiteral("/sticky-notes.desktop");

    const bool enabled = Storage::instance().setting(QStringLiteral("autostart"), QStringLiteral("0")) == QLatin1String("1");
    if (!enabled) {
        QFile::remove(file);
        return;
    }

    QFile f(file);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return;
    QTextStream out(&f);
    out << "[Desktop Entry]\n"
        << "Type=Application\n"
        << "Name=Sticky Notes\n"
        << "Comment=" << I18n::t(QStringLiteral("app.autostart_comment")) << "\n"
        << "Exec=" << QCoreApplication::applicationFilePath() << "\n"
        << "Icon=sticky-notes\n"
        << "Terminal=false\n"
        << "X-GNOME-Autostart-enabled=true\n"
        << "OnlyShowIn=KDE;\n";
}
