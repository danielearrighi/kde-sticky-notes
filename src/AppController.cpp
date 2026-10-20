#include "AppController.h"
#include "I18n.h"
#include "NoteWindow.h"
#include "HubWindow.h"
#include "Storage.h"
#include "Themes.h"
#include "UiHelpers.h"

#include <QApplication>
#include <QAction>
#include <QEvent>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMenu>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QMessageBox>
#include <QIcon>
#include <QRandomGenerator>
#include <QScreen>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QDateTime>
#include <QDebug>

namespace {
const char *kServerName = "sticky-notes-single-instance";

// Monochrome note glyph for the tray, tinted with the theme foreground so it
// matches the Plasma panel: white on the default dark theme, dark on a light
// one. Falls back to the colored app icon if the SVG cannot be rendered.
QIcon themedTrayIcon()
{
    const QPixmap mask = QIcon(QStringLiteral(":/icons/sticky-notes-symbolic.svg")).pixmap(64, 64);
    if (mask.isNull())
        return QIcon(QStringLiteral(":/icons/sticky-notes.png"));

    QColor fg = QApplication::palette().color(QPalette::WindowText);
    if (!fg.isValid() || fg.alpha() == 0)
        fg = Qt::white;

    QPixmap tinted(mask.size());
    tinted.fill(Qt::transparent);
    QPainter painter(&tinted);
    painter.drawPixmap(0, 0, mask);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(tinted.rect(), fg);
    painter.end();

    return QIcon(tinted);
}
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
            // A normal second launch asks for the hub ("show"); an autostart
            // launch only probes the running instance and sends nothing.
            s->waitForReadyRead(500);
            if (s->readAll() == QByteArrayLiteral("show"))
                showHub();
        }
    });
    return true;
}

void AppController::start(bool startHidden)
{
    m_hub = new HubWindow;
    connect(m_hub, &HubWindow::newNoteRequested, this, &AppController::onNewNote);
    connect(m_hub, &HubWindow::noteVisibilityToggled, this, &AppController::onVisibilityToggled);
    connect(m_hub, &HubWindow::deleteNoteRequested, this, &AppController::onDeleteNote);
    connect(m_hub, &HubWindow::restoreNoteRequested, this, &AppController::onRestoreNote);
    connect(m_hub, &HubWindow::purgeNoteRequested, this, &AppController::onPurgeNote);
    connect(m_hub, &HubWindow::purgeAllRequested, this, &AppController::onPurgeAll);
    connect(m_hub, &HubWindow::settingsChanged, this, &AppController::onSettingsChanged);
    connect(m_hub, &HubWindow::flushPendingContent, this, &AppController::onFlushPendingContent);
    connect(m_hub, &HubWindow::backupRestored, this, &AppController::onBackupRestored);
    connect(m_hub, &HubWindow::quitRequested, this, &AppController::onQuitRequested);

    setupTray();
    updateTrayVisibility();

    // Follow the screen topology: when a monitor is unplugged (or its geometry
    // changes) the notes that lived on it must be pulled back onto the
    // remaining screens, otherwise their saved coordinates stay off-screen and
    // they vanish until a restart. The relayout is deferred to the next event
    // loop turn so every screen update has settled (the new primary included).
    m_screenTimer = new QTimer(this);
    m_screenTimer->setSingleShot(true);
    m_screenTimer->setInterval(0);
    connect(m_screenTimer, &QTimer::timeout, this, &AppController::relayoutNotesOnScreens);
    connect(qApp, &QGuiApplication::screenAdded, this, [this](QScreen *s) {
        connectScreen(s);
        m_screenTimer->start();
    });
    connect(qApp, &QGuiApplication::screenRemoved, this, [this] { m_screenTimer->start(); });
    connect(qApp, &QGuiApplication::primaryScreenChanged, this, [this] { m_screenTimer->start(); });
    for (QScreen *s : QGuiApplication::screens())
        connectScreen(s);

    // The scroll position is not tracked incrementally and the body content is
    // saved with a debounce: flush both for every open note right before the
    // app exits (the event loop no longer runs timers after quit()).
    connect(QApplication::instance(), &QCoreApplication::aboutToQuit, this, [this] {
        for (NoteWindow *w : std::as_const(m_windows)) {
            w->flushPendingSave();
            w->persistScroll();
        }
    });

    for (const Note &n : Storage::instance().notes(false))
        if (n.visible)
            ensureWindow(n);

    if (startHidden) {
        // Autostart at login: only the notes appear on the desktop, the hub
        // stays out of the way. With "close to system tray" it goes to the tray,
        // otherwise it is minimized to the taskbar so it can be reopened.
        if (closeToTrayEnabled())
            m_hub->hide();
        else
            m_hub->showMinimized();
    } else {
        m_hub->show();
        m_hub->raise();
        m_hub->activateWindow();
    }

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
    connect(w, &NoteWindow::geometryEdited, this, [](qint64 id, int x, int y, int ww, int hh,
                                                     const QString &screenKey) {
        Storage::instance().saveNotePlacement(id, x, y, ww, hh, screenKey);
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
    connect(w, &NoteWindow::scrollEdited, this, [](qint64 id, int scroll) {
        Storage::instance().updateScroll(id, scroll);
    });
    connect(w, &NoteWindow::opacityEdited, this, [](qint64 id, int opacity) {
        Storage::instance().updateOpacity(id, opacity);
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

    // Records the first placement (per-screen position + last screen key) now
    // that the signals are connected.
    w->ensureOnScreen();

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

void AppController::connectScreen(QScreen *screen)
{
    connect(screen, &QScreen::geometryChanged, this, [this] { m_screenTimer->start(); });
    connect(screen, &QScreen::availableGeometryChanged, this, [this] { m_screenTimer->start(); });
}

void AppController::relayoutNotesOnScreens()
{
    for (NoteWindow *w : std::as_const(m_windows))
        w->ensureOnScreen();
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

void AppController::onVisibilityToggled(qint64 id, bool visible)
{
    Storage::instance().setVisible(id, visible);
    if (visible) {
        Note n = Storage::instance().note(id);
        NoteWindow *w = ensureWindow(n);
        // Keep the window's own visible flag in sync (ensureWindow only shows an
        // existing one) and bring it back on screen if needed.
        w->setVisibleState(true);
        w->activate();
    } else if (NoteWindow *w = m_windows.value(id)) {
        w->setVisibleState(false);
    }
    if (m_hub)
        m_hub->refresh();
}

void AppController::showAllNotes()
{
    bool changed = false;
    for (const Note &n : Storage::instance().notes(false)) {
        if (n.visible)
            continue;
        Storage::instance().setVisible(n.id, true);
        ensureWindow(n)->setVisibleState(true);
        changed = true;
    }
    if (changed && m_hub)
        m_hub->refresh();
}

void AppController::hideAllNotes()
{
    bool changed = false;
    for (const Note &n : Storage::instance().notes(false)) {
        if (!n.visible)
            continue;
        Storage::instance().setVisible(n.id, false);
        if (NoteWindow *w = m_windows.value(n.id))
            w->setVisibleState(false);
        changed = true;
    }
    if (changed && m_hub)
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

    // Deleting a note also closed its window (onDeleteNote -> removeWindow), and
    // Storage::restore() only flips the row back: without reopening it the note
    // shows up in the hub (with its "Hide" button, i.e. visible=1) but never
    // reappears on screen. Recreate the window for the notes that were visible.
    const Note n = Storage::instance().note(id);
    if (n.id && !n.deleted && n.visible)
        ensureWindow(n)->setVisibleState(true);

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
    updateTrayVisibility();
}

void AppController::onFlushPendingContent()
{
    for (NoteWindow *w : std::as_const(m_windows))
        w->flushPendingSave();
}

void AppController::onBackupRestored()
{
    // A restore replaces every note, so the old windows point to ids that no
    // longer exist: close them all and reopen the visible notes of the backup.
    const QList<qint64> ids = m_windows.keys();
    for (qint64 id : ids)
        removeWindow(id);

    for (const Note &n : Storage::instance().notes(false))
        if (n.visible)
            ensureWindow(n);

    if (m_hub)
        m_hub->refresh();
}

bool AppController::closeToTrayEnabled() const
{
    return Storage::instance().setting(QStringLiteral("close_to_tray"), QStringLiteral("0")) == QLatin1String("1")
           && m_tray && m_tray->isVisible();
}

void AppController::onQuitRequested()
{
    // Closing the hub (X) or the "Quit app" button only hides it when the
    // "close to tray" option is on: the tray menu offers the real quit.
    if (closeToTrayEnabled()) {
        if (m_hub)
            m_hub->hide();
        return;
    }
    QApplication::quit();
}

void AppController::setupTray()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable())
        return;

    m_tray = new QSystemTrayIcon(themedTrayIcon(), this);
    m_tray->setToolTip(QStringLiteral("Sticky Notes"));

    // Follow Plasma color-scheme changes: retint the glyph with the new
    // foreground color (white on dark, dark on light).
    qApp->installEventFilter(this);

    auto *menu = new QMenu(m_hub);
    QAction *show = menu->addAction(I18n::t(QStringLiteral("app.tray_show")));
    connect(show, &QAction::triggered, this, &AppController::showHub);
    menu->addSeparator();
    QAction *showNotes = menu->addAction(I18n::t(QStringLiteral("app.tray_show_notes")));
    connect(showNotes, &QAction::triggered, this, &AppController::showAllNotes);
    QAction *hideNotes = menu->addAction(I18n::t(QStringLiteral("app.tray_hide_notes")));
    connect(hideNotes, &QAction::triggered, this, &AppController::hideAllNotes);
    menu->addSeparator();
    QAction *quit = menu->addAction(I18n::t(QStringLiteral("app.quit")));
    connect(quit, &QAction::triggered, this, [] { QApplication::quit(); });
    m_tray->setContextMenu(menu);

    connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
            showHub();
    });
}

void AppController::updateTrayVisibility()
{
    if (!m_tray)
        return;
    m_tray->setVisible(Storage::instance().setting(QStringLiteral("close_to_tray"), QStringLiteral("0")) == QLatin1String("1"));
}

bool AppController::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::ApplicationPaletteChange && m_tray)
        m_tray->setIcon(themedTrayIcon());
    return QObject::eventFilter(watched, event);
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
        << "Exec=\"" << QCoreApplication::applicationFilePath() << "\" --autostart\n"
        << "Icon=sticky-notes\n"
        << "Terminal=false\n"
        << "X-GNOME-Autostart-enabled=true\n"
        << "OnlyShowIn=KDE;\n";
}
