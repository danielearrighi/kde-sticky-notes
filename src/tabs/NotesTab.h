#pragma once

#include <QWidget>

class QGridLayout;
class QLabel;
class QPushButton;

/// "Notes" tab: grid of cards with open / show-hide / delete actions.
class NotesTab : public QWidget
{
    Q_OBJECT
public:
    explicit NotesTab(QWidget *parent = nullptr);

    /// Reloads the grid from Storage.
    void refresh();

    /// Updates the top-right button label to match the "close to tray" setting.
    void updateQuitButton();

signals:
    void newNoteRequested();
    void openNoteRequested(qint64 id);
    void noteVisibilityToggled(qint64 id, bool visible);
    void deleteNoteRequested(qint64 id);
    void quitRequested();

private:
    QGridLayout *m_grid = nullptr;
    QLabel *m_empty = nullptr;
    QPushButton *m_quit = nullptr;
};
