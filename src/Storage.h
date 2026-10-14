#pragma once

#include "Models.h"

#include <QList>
#include <QString>

class QSqlDatabase;

/// Access to the local SQLite database (notes, themes, settings).
class Storage
{
public:
    static Storage &instance();

    /// Opens (creating if needed) the database in the app data path.
    bool open(QString *errorOut = nullptr);
    QString databasePath() const { return m_path; }

    // --- Note ---
    QList<Note> notes(bool includeDeleted = false) const;
    Note note(qint64 id) const;
    qint64 insertNote(Note n);
    void updateNote(const Note &n);
    void updateContent(qint64 id, const QString &title, const QString &content);
    void updateGeometry(qint64 id, int x, int y, int w, int h);
    void updateTheme(qint64 id, const QString &themeId);
    void updateLayer(qint64 id, const QString &layer);
    void updateCollapsed(qint64 id, bool collapsed);
    void setVisible(qint64 id, bool visible);
    void softDelete(qint64 id);
    void restore(qint64 id);
    void purge(qint64 id);
    void purgeAllDeleted();
    int deletedCount() const;

    // --- Themes ---
    QList<Theme> themes() const;
    Theme theme(const QString &id) const;
    void upsertTheme(const Theme &t);
    void deleteTheme(const QString &id);

    // --- Settings ---
    QString setting(const QString &key, const QString &def = QString()) const;
    void setSetting(const QString &key, const QString &value);

private:
    Storage() = default;
    QSqlDatabase db() const;
    void ensureSchema();

    QString m_path;
};
