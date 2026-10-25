#pragma once

#include "Models.h"

#include <QList>
#include <QPair>
#include <QRect>
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
    void updateContent(qint64 id, const QString &title, const QString &content);
    /// Updates the note geometry only. Returns false (and logs the SQL error)
    /// when the update fails.
    bool updateGeometry(qint64 id, int x, int y, int w, int h);
    /// Persists the geometry and records it under \a screenKey (per-screen
    /// positions) and as the note's last screen. x,y are screen-relative.
    void saveNotePlacement(qint64 id, int x, int y, int w, int h, const QString &screenKey);
    /// Saved screen-relative position of a note on a given screen, or an invalid
    /// QRect when the note has never been placed there.
    QRect notePosition(qint64 id, const QString &screenKey) const;
    void updateTheme(qint64 id, const QString &themeId);
    void updateLayer(qint64 id, const QString &layer);
    void updateCollapsed(qint64 id, bool collapsed);
    void updateLocked(qint64 id, bool locked);
    void updateScroll(qint64 id, int scroll);
    void updateOpacity(qint64 id, int opacity);
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

    // --- Backup ---
    /// Writes a self-contained copy of the database (notes, themes, per-screen
    /// positions) to \a path, replacing an existing file. Returns false and sets
    /// errorOut on write failure.
    bool exportBackup(const QString &path, QString *errorOut = nullptr) const;
    /// Replaces the live database with the one at \a path (a file written by
    /// exportBackup()). The file is validated first, then the connection is
    /// closed, the database file swapped and reopened; the current preferences
    /// are kept. Returns false and sets errorOut if the file is missing or is not
    /// a Sticky Notes database.
    bool importBackup(const QString &path, QString *errorOut = nullptr);

private:
    Storage() = default;
    QSqlDatabase db() const;
    void ensureSchema();

    QString m_path;
};
