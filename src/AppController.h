#pragma once

#include "Models.h"

#include <QObject>
#include <QHash>

class HubWindow;
class NoteWindow;
class QEvent;
class QLocalServer;
class QScreen;
class QSystemTrayIcon;
class QTimer;

/// Coordinates the hub, note windows, persistence, single instance and autostart.
class AppController : public QObject
{
    Q_OBJECT
public:
    explicit AppController(QObject *parent = nullptr);
    ~AppController() override;

    /// Opens the database. Returns false and sets errorOut on error.
    bool init(QString *errorOut);

    /// Creates the hub + visible notes. With startHidden (an autostart launch)
    /// only the notes are shown and the hub stays out of the way (in the tray
    /// when "close to system tray" is active, otherwise minimized).
    void start(bool startHidden = false);

    /// Starts the single-instance server (a second launch brings the hub to the front).
    bool listenForSecondInstance(QString *errorOut);

public slots:
    void showHub();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void onNewNote();
    void onVisibilityToggled(qint64 id, bool visible);
    void onDeleteNote(qint64 id);
    void onRestoreNote(qint64 id);
    void onPurgeNote(qint64 id);
    void onPurgeAll();
    void onSettingsChanged();
    void onFlushPendingContent();
    void onBackupRestored();
    void onQuitRequested();
    void relayoutNotesOnScreens();

private:
    NoteWindow *ensureWindow(const Note &note);
    void removeWindow(qint64 id);
    void createNewNote();
    void showAllNotes();
    void hideAllNotes();
    void applyAutostart();
    void setupTray();
    void updateTrayVisibility();
    bool closeToTrayEnabled() const;
    QPoint cascadePosition() const;
    void connectScreen(QScreen *screen);

    HubWindow *m_hub = nullptr;
    QLocalServer *m_server = nullptr;
    QSystemTrayIcon *m_tray = nullptr;
    QTimer *m_screenTimer = nullptr;
    QHash<qint64, NoteWindow *> m_windows;
};
