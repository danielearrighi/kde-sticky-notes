#pragma once

#include "Models.h"

#include <QObject>
#include <QHash>

class HubWindow;
class NoteWindow;
class QLocalServer;

/// Coordinates the hub, note windows, persistence, single instance and autostart.
class AppController : public QObject
{
    Q_OBJECT
public:
    explicit AppController(QObject *parent = nullptr);
    ~AppController() override;

    /// Opens the database. Returns false and sets errorOut on error.
    bool init(QString *errorOut);

    /// Creates the hub + visible notes and shows everything.
    void start();

    /// Starts the single-instance server (a second launch brings the hub to the front).
    bool listenForSecondInstance(QString *errorOut);

public slots:
    void showHub();

private slots:
    void onNewNote();
    void onOpenNote(qint64 id);
    void onVisibilityToggled(qint64 id, bool visible);
    void onDeleteNote(qint64 id);
    void onRestoreNote(qint64 id);
    void onPurgeNote(qint64 id);
    void onPurgeAll();
    void onSettingsChanged();

private:
    NoteWindow *ensureWindow(const Note &note);
    void removeWindow(qint64 id);
    void createNewNote();
    void applyAutostart();
    QPoint cascadePosition() const;

    HubWindow *m_hub = nullptr;
    QLocalServer *m_server = nullptr;
    QHash<qint64, NoteWindow *> m_windows;
};
