#pragma once

#include <QWidget>

class QGridLayout;
class QLabel;

/// "Notes" tab: grid of cards with open / show-hide / delete actions.
class NotesTab : public QWidget
{
    Q_OBJECT
public:
    explicit NotesTab(QWidget *parent = nullptr);

    /// Reloads the grid from Storage.
    void refresh();

signals:
    void newNoteRequested();
    void openNoteRequested(qint64 id);
    void noteVisibilityToggled(qint64 id, bool visible);
    void deleteNoteRequested(qint64 id);
    void quitRequested();

private:
    QGridLayout *m_grid = nullptr;
    QLabel *m_empty = nullptr;
};
