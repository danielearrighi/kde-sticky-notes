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
    void updateGeometry(qint64 id, int x, int y, int w, int h);
    /// Persists the geometry and records it under \a screenKey (per-screen
    /// positions) and as the note's last screen. x,y are screen-relative.
    void saveNotePlacement(qint64 id, int x, int y, int w, int h, const QString &screenKey);
    /// Saved screen-relative position of a note on a given screen, or an invalid
    /// QRect when the note has never been placed there.
    QRect notePosition(qint64 id, const QString &screenKey) const;
    /// Every saved per-screen position of a note (screen key -> rect), used by
    /// the backup export.
    QList<QPair<QString, QRect>> notePositions(qint64 id) const;
    void updateTheme(qint64 id, const QString &themeId);
    void updateLayer(qint64 id, const QString &layer);
    void updateCollapsed(qint64 id, bool collapsed);
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
    /// Writes the notes (and their themes) to a JSON backup file. Returns false
    /// and sets errorOut on write failure.
    bool exportBackup(const QString &path, QString *errorOut = nullptr) const;
    /// Reads a JSON backup written by exportBackup(). With replaceExisting the
    /// current notes are deleted before the backup ones are inserted; themes are
    /// always upserted. Returns false and sets errorOut if the file is missing or
    /// is not a Sticky Notes backup.
    bool importBackup(const QString &path, bool replaceExisting, QString *errorOut = nullptr);

private:
    Storage() = default;
    QSqlDatabase db() const;
    void ensureSchema();

    QString m_path;
};
