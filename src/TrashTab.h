#pragma once

#include <QWidget>

class QLabel;
class QPushButton;
class QTableWidget;

/// "Trash" tab: table of deleted notes with restore / delete actions.
class TrashTab : public QWidget
{
    Q_OBJECT
public:
    explicit TrashTab(QWidget *parent = nullptr);

    /// Reloads the table from Storage.
    void refresh();

signals:
    void restoreNoteRequested(qint64 id);
    void purgeNoteRequested(qint64 id);
    void purgeAllRequested();

private:
    qint64 selectedId() const;

    QTableWidget *m_table = nullptr;
    QLabel *m_empty = nullptr;
    QPushButton *m_restore = nullptr;
    QPushButton *m_purge = nullptr;
    QPushButton *m_purgeAll = nullptr;
};
