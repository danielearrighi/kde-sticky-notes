#pragma once

#include <QMainWindow>

class QTabWidget;
class QCloseEvent;
class NotesTab;
class TrashTab;
class SettingsTab;
class InfoTab;

/// Main window (taskbar icon): hosts the tabs and forwards their signals to
/// AppController. Each tab's logic lives in its own class.
class HubWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit HubWindow(QWidget *parent = nullptr);

    /// Reloads contents from Storage (forwarded to the three tabs).
    void refresh();

signals:
    void newNoteRequested();
    void noteVisibilityToggled(qint64 id, bool visible);
    void deleteNoteRequested(qint64 id);
    void restoreNoteRequested(qint64 id);
    void purgeNoteRequested(qint64 id);
    void purgeAllRequested();
    void settingsChanged();
    /// A backup is about to be restored: open notes must flush pending edits.
    void flushPendingContent();
    /// A backup was restored: the current notes were replaced.
    void backupRestored();
    void quitRequested();

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    QTabWidget *m_tabs = nullptr;
    NotesTab *m_notes = nullptr;
    TrashTab *m_trash = nullptr;
    SettingsTab *m_settings = nullptr;
    InfoTab *m_info = nullptr;
};
