#include "Storage.h"
#include "Themes.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QStandardPaths>
#include <QDir>
#include <QDateTime>
#include <QDebug>
#include <QStringList>
#include <QVariant>

namespace {
const char *kConn = "sticky-notes";

QString nowIso()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
}

/// "null" QStrings would be bound as NULL: normalize them to an empty string.
QVariant bindText(const QString &s)
{
    return QVariant(s.isNull() ? QString::fromLatin1("") : s);
}
} // namespace

Storage &Storage::instance()
{
    static Storage s;
    return s;
}

QSqlDatabase Storage::db() const
{
    return QSqlDatabase::database(QString::fromLatin1(kConn));
}

bool Storage::open(QString *errorOut)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                        + QStringLiteral("/StickyNotes");
    QDir().mkpath(dir);
    m_path = dir + QStringLiteral("/notes.db");

    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                      QString::fromLatin1(kConn));
    database.setDatabaseName(m_path);
    if (!database.open()) {
        if (errorOut)
            *errorOut = database.lastError().text();
        return false;
    }

    QSqlQuery q(database);
    q.exec(QStringLiteral("PRAGMA foreign_keys = ON"));
    q.exec(QStringLiteral("PRAGMA journal_mode = WAL"));

    ensureSchema();

    // Default themes (inserted only if missing).
    for (const Theme &t : Themes::defaults())
        if (!theme(t.id).isValid())
            upsertTheme(t);

    return true;
}

void Storage::ensureSchema()
{
    QSqlQuery q(db());
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS notes ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " title TEXT NOT NULL DEFAULT '',"
        " content TEXT NOT NULL DEFAULT '',"
        " theme TEXT NOT NULL DEFAULT 'giallo',"
        " x INTEGER NOT NULL DEFAULT 220,"
        " y INTEGER NOT NULL DEFAULT 160,"
        " w INTEGER NOT NULL DEFAULT 230,"
        " h INTEGER NOT NULL DEFAULT 190,"
        " layer TEXT NOT NULL DEFAULT 'top',"
        " visible INTEGER NOT NULL DEFAULT 1,"
        " collapsed INTEGER NOT NULL DEFAULT 0,"
        " deleted INTEGER NOT NULL DEFAULT 0,"
        " created_at TEXT, updated_at TEXT, deleted_at TEXT)"));

    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS themes ("
        " id TEXT PRIMARY KEY,"
        " name TEXT NOT NULL,"
        " body TEXT NOT NULL,"
        " title_bar TEXT NOT NULL,"
        " text_color TEXT NOT NULL,"
        " sort INTEGER NOT NULL DEFAULT 0,"
        " builtin INTEGER NOT NULL DEFAULT 1)"));

    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS settings ("
        " key TEXT PRIMARY KEY,"
        " value TEXT)"));

    // Lightweight migration for DBs created by older versions (the project has
    // no migration framework): adds the missing columns.
    QStringList cols;
    QSqlQuery info(db());
    if (info.exec(QStringLiteral("PRAGMA table_info(notes)")))
        while (info.next())
            cols << info.value(1).toString();
    if (!cols.contains(QStringLiteral("collapsed")))
        q.exec(QStringLiteral("ALTER TABLE notes ADD COLUMN collapsed INTEGER NOT NULL DEFAULT 0"));
}

// ---------------------------------------------------------------- note

namespace {
Note noteFromQuery(const QSqlQuery &q)
{
    Note n;
    n.id = q.value(0).toLongLong();
    n.title = q.value(1).toString();
    n.content = q.value(2).toString();
    n.themeId = q.value(3).toString();
    n.x = q.value(4).toInt();
    n.y = q.value(5).toInt();
    n.w = q.value(6).toInt();
    n.h = q.value(7).toInt();
    n.layer = q.value(8).toString();
    n.visible = q.value(9).toInt() != 0;
    n.collapsed = q.value(10).toInt() != 0;
    n.deleted = q.value(11).toInt() != 0;
    n.createdAt = q.value(12).toString();
    n.updatedAt = q.value(13).toString();
    n.deletedAt = q.value(14).toString();
    return n;
}

const char *kNoteCols =
    "id,title,content,theme,x,y,w,h,layer,visible,collapsed,deleted,created_at,updated_at,deleted_at";
} // namespace

QList<Note> Storage::notes(bool includeDeleted) const
{
    QList<Note> out;
    QSqlQuery q(db());
    const QString sql = QStringLiteral("SELECT %1 FROM notes %2 ORDER BY deleted DESC, id ASC")
                            .arg(QString::fromLatin1(kNoteCols),
                                 includeDeleted ? QString() : QStringLiteral("WHERE deleted = 0"));
    if (!q.exec(sql)) {
        qWarning() << "Storage::notes:" << q.lastError().text();
        return out;
    }
    while (q.next())
        out << noteFromQuery(q);
    return out;
}

Note Storage::note(qint64 id) const
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral("SELECT %1 FROM notes WHERE id = ?").arg(QString::fromLatin1(kNoteCols)));
    q.addBindValue(id);
    if (q.exec() && q.next())
        return noteFromQuery(q);
    return {};
}

qint64 Storage::insertNote(Note n)
{
    const QString now = nowIso();
    QSqlQuery q(db());
    q.prepare(QStringLiteral(
        "INSERT INTO notes (title,content,theme,x,y,w,h,layer,visible,collapsed,deleted,created_at,updated_at)"
        " VALUES (?,?,?,?,?,?,?,?,?,?,0,?,?)"));
    q.addBindValue(bindText(n.title));
    q.addBindValue(bindText(n.content));
    q.addBindValue(bindText(n.themeId));
    q.addBindValue(n.x);
    q.addBindValue(n.y);
    q.addBindValue(n.w);
    q.addBindValue(n.h);
    q.addBindValue(bindText(n.layer));
    q.addBindValue(n.visible ? 1 : 0);
    q.addBindValue(n.collapsed ? 1 : 0);
    q.addBindValue(now);
    q.addBindValue(now);
    if (!q.exec()) {
        qWarning() << "Storage::insertNote:" << q.lastError().text();
        return -1;
    }
    return q.lastInsertId().toLongLong();
}

void Storage::updateNote(const Note &n)
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral(
        "UPDATE notes SET title=?,content=?,theme=?,x=?,y=?,w=?,h=?,layer=?,visible=?,collapsed=?,updated_at=?"
        " WHERE id=?"));
    q.addBindValue(bindText(n.title));
    q.addBindValue(bindText(n.content));
    q.addBindValue(bindText(n.themeId));
    q.addBindValue(n.x);
    q.addBindValue(n.y);
    q.addBindValue(n.w);
    q.addBindValue(n.h);
    q.addBindValue(bindText(n.layer));
    q.addBindValue(n.visible ? 1 : 0);
    q.addBindValue(n.collapsed ? 1 : 0);
    q.addBindValue(nowIso());
    q.addBindValue(n.id);
    q.exec();
}

void Storage::updateContent(qint64 id, const QString &title, const QString &content)
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral("UPDATE notes SET title=?,content=?,updated_at=? WHERE id=?"));
    q.addBindValue(bindText(title));
    q.addBindValue(bindText(content));
    q.addBindValue(nowIso());
    q.addBindValue(id);
    q.exec();
}

void Storage::updateGeometry(qint64 id, int x, int y, int w, int h)
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral("UPDATE notes SET x=?,y=?,w=?,h=? WHERE id=?"));
    q.addBindValue(x);
    q.addBindValue(y);
    q.addBindValue(w);
    q.addBindValue(h);
    q.addBindValue(id);
    q.exec();
}

void Storage::updateTheme(qint64 id, const QString &themeId)
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral("UPDATE notes SET theme=? WHERE id=?"));
    q.addBindValue(themeId);
    q.addBindValue(id);
    q.exec();
}

void Storage::updateLayer(qint64 id, const QString &layer)
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral("UPDATE notes SET layer=? WHERE id=?"));
    q.addBindValue(layer);
    q.addBindValue(id);
    q.exec();
}

void Storage::updateCollapsed(qint64 id, bool collapsed)
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral("UPDATE notes SET collapsed=? WHERE id=?"));
    q.addBindValue(collapsed ? 1 : 0);
    q.addBindValue(id);
    q.exec();
}

void Storage::setVisible(qint64 id, bool visible)
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral("UPDATE notes SET visible=? WHERE id=?"));
    q.addBindValue(visible ? 1 : 0);
    q.addBindValue(id);
    q.exec();
}

void Storage::softDelete(qint64 id)
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral("UPDATE notes SET deleted=1, deleted_at=?, updated_at=? WHERE id=?"));
    const QString now = nowIso();
    q.addBindValue(now);
    q.addBindValue(now);
    q.addBindValue(id);
    q.exec();
}

void Storage::restore(qint64 id)
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral("UPDATE notes SET deleted=0, deleted_at=NULL, updated_at=? WHERE id=?"));
    q.addBindValue(nowIso());
    q.addBindValue(id);
    q.exec();
}

void Storage::purge(qint64 id)
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral("DELETE FROM notes WHERE id=?"));
    q.addBindValue(id);
    q.exec();
}

void Storage::purgeAllDeleted()
{
    QSqlQuery q(db());
    q.exec(QStringLiteral("DELETE FROM notes WHERE deleted=1"));
}

int Storage::deletedCount() const
{
    QSqlQuery q(db());
    if (q.exec(QStringLiteral("SELECT COUNT(*) FROM notes WHERE deleted=1")) && q.next())
        return q.value(0).toInt();
    return 0;
}

// ---------------------------------------------------------------- themes

namespace {
Theme themeFromQuery(const QSqlQuery &q)
{
    Theme t;
    t.id = q.value(0).toString();
    t.name = q.value(1).toString();
    t.body = q.value(2).toString();
    t.titleBar = q.value(3).toString();
    t.text = q.value(4).toString();
    t.sort = q.value(5).toInt();
    t.builtin = q.value(6).toInt() != 0;
    return t;
}
} // namespace

QList<Theme> Storage::themes() const
{
    QList<Theme> out;
    QSqlQuery q(db());
    if (!q.exec(QStringLiteral("SELECT id,name,body,title_bar,text_color,sort,builtin"
                               " FROM themes ORDER BY sort, name")))
        return out;
    while (q.next())
        out << themeFromQuery(q);
    return out;
}

Theme Storage::theme(const QString &id) const
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral("SELECT id,name,body,title_bar,text_color,sort,builtin"
                             " FROM themes WHERE id=?"));
    q.addBindValue(id);
    if (q.exec() && q.next())
        return themeFromQuery(q);
    return {};
}

void Storage::upsertTheme(const Theme &t)
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral(
        "INSERT INTO themes (id,name,body,title_bar,text_color,sort,builtin)"
        " VALUES (?,?,?,?,?,?,?)"
        " ON CONFLICT(id) DO UPDATE SET name=excluded.name, body=excluded.body,"
        " title_bar=excluded.title_bar, text_color=excluded.text_color,"
        " sort=excluded.sort"));
    q.addBindValue(t.id);
    q.addBindValue(t.name);
    q.addBindValue(t.body);
    q.addBindValue(t.titleBar);
    q.addBindValue(t.text);
    q.addBindValue(t.sort);
    q.addBindValue(t.builtin ? 1 : 0);
    if (!q.exec())
        qWarning() << "Storage::upsertTheme:" << q.lastError().text();
}

void Storage::deleteTheme(const QString &id)
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral("DELETE FROM themes WHERE id=? AND builtin=0"));
    q.addBindValue(id);
    q.exec();
}

// ------------------------------------------------------------ settings

QString Storage::setting(const QString &key, const QString &def) const
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral("SELECT value FROM settings WHERE key=?"));
    q.addBindValue(key);
    if (q.exec() && q.next())
        return q.value(0).toString();
    return def;
}

void Storage::setSetting(const QString &key, const QString &value)
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral("INSERT INTO settings (key,value) VALUES (?,?)"
                             " ON CONFLICT(key) DO UPDATE SET value=excluded.value"));
    q.addBindValue(key);
    q.addBindValue(value);
    q.exec();
}
