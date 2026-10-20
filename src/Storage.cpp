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
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

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
        " scroll INTEGER NOT NULL DEFAULT 0,"
        " opacity INTEGER NOT NULL DEFAULT 100,"
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

    // Per-screen note positions: where a note sits on each monitor, keyed by the
    // screen key ("<W>x<H>|<connector>", built by screenKeyOf()). Separate from
    // notes.x/y, which stays as the fallback / last known position.
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS note_positions ("
        " note_id INTEGER NOT NULL REFERENCES notes(id) ON DELETE CASCADE,"
        " screen_key TEXT NOT NULL,"
        " x INTEGER NOT NULL,"
        " y INTEGER NOT NULL,"
        " w INTEGER NOT NULL,"
        " h INTEGER NOT NULL,"
        " PRIMARY KEY (note_id, screen_key))"));

    // Lightweight migration for DBs created by older versions (the project has
    // no migration framework): adds the missing columns.
    QStringList cols;
    QSqlQuery info(db());
    if (info.exec(QStringLiteral("PRAGMA table_info(notes)")))
        while (info.next())
            cols << info.value(1).toString();
    if (!cols.contains(QStringLiteral("collapsed")))
        q.exec(QStringLiteral("ALTER TABLE notes ADD COLUMN collapsed INTEGER NOT NULL DEFAULT 0"));
    if (!cols.contains(QStringLiteral("scroll")))
        q.exec(QStringLiteral("ALTER TABLE notes ADD COLUMN scroll INTEGER NOT NULL DEFAULT 0"));
    if (!cols.contains(QStringLiteral("opacity")))
        q.exec(QStringLiteral("ALTER TABLE notes ADD COLUMN opacity INTEGER NOT NULL DEFAULT 100"));
    if (!cols.contains(QStringLiteral("last_screen_key")))
        q.exec(QStringLiteral("ALTER TABLE notes ADD COLUMN last_screen_key TEXT NOT NULL DEFAULT ''"));
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
    n.scroll = q.value(11).toInt();
    n.opacity = qBound(10, q.value(12).toInt(), 100);
    n.deleted = q.value(13).toInt() != 0;
    n.createdAt = q.value(14).toString();
    n.updatedAt = q.value(15).toString();
    n.deletedAt = q.value(16).toString();
    n.lastScreenKey = q.value(17).toString();
    return n;
}

const char *kNoteCols =
    "id,title,content,theme,x,y,w,h,layer,visible,collapsed,scroll,opacity,deleted,created_at,updated_at,deleted_at,last_screen_key";
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
        "INSERT INTO notes (title,content,theme,x,y,w,h,layer,visible,collapsed,scroll,opacity,deleted,last_screen_key,created_at,updated_at)"
        " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,0,?,?,?)"));
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
    q.addBindValue(n.scroll);
    q.addBindValue(qBound(10, n.opacity, 100));
    q.addBindValue(bindText(n.lastScreenKey));
    q.addBindValue(now);
    q.addBindValue(now);
    if (!q.exec()) {
        qWarning() << "Storage::insertNote:" << q.lastError().text();
        return -1;
    }
    return q.lastInsertId().toLongLong();
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

bool Storage::updateGeometry(qint64 id, int x, int y, int w, int h)
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral("UPDATE notes SET x=?,y=?,w=?,h=? WHERE id=?"));
    q.addBindValue(x);
    q.addBindValue(y);
    q.addBindValue(w);
    q.addBindValue(h);
    q.addBindValue(id);
    if (!q.exec()) {
        qWarning() << "Storage::updateGeometry:" << q.lastError().text();
        return false;
    }
    return true;
}

void Storage::saveNotePlacement(qint64 id, int x, int y, int w, int h, const QString &screenKey)
{
    // The geometry, the last screen and the per-screen position describe one
    // placement: write them in a single transaction so a failure cannot leave
    // them out of sync.
    QSqlDatabase database = db();
    const bool inTransaction = database.transaction();
    if (!inTransaction)
        qWarning() << "Storage::saveNotePlacement: cannot start a transaction:"
                   << database.lastError().text();

    bool ok = updateGeometry(id, x, y, w, h);
    {
        QSqlQuery q(database);
        q.prepare(QStringLiteral("UPDATE notes SET last_screen_key=? WHERE id=?"));
        q.addBindValue(bindText(screenKey));
        q.addBindValue(id);
        if (!q.exec()) {
            ok = false;
            qWarning() << "Storage::saveNotePlacement:" << q.lastError().text();
        }
    }

    // The per-screen position is only meaningful for a real screen key (an
    // empty key means "no screen": keep the fallback only).
    if (!screenKey.isEmpty()) {
        QSqlQuery p(database);
        p.prepare(QStringLiteral(
            "INSERT OR REPLACE INTO note_positions (note_id,screen_key,x,y,w,h) VALUES (?,?,?,?,?,?)"));
        p.addBindValue(id);
        p.addBindValue(screenKey);
        p.addBindValue(x);
        p.addBindValue(y);
        p.addBindValue(w);
        p.addBindValue(h);
        if (!p.exec()) {
            ok = false;
            qWarning() << "Storage::saveNotePlacement:" << p.lastError().text();
        }
    }

    if (inTransaction) {
        if (ok)
            database.commit();
        else
            database.rollback();
    }
}

QRect Storage::notePosition(qint64 id, const QString &screenKey) const
{
    if (screenKey.isEmpty())
        return {};
    QSqlQuery q(db());
    q.prepare(QStringLiteral(
        "SELECT x,y,w,h FROM note_positions WHERE note_id=? AND screen_key=?"));
    q.addBindValue(id);
    q.addBindValue(screenKey);
    if (q.exec() && q.next())
        return QRect(q.value(0).toInt(), q.value(1).toInt(), q.value(2).toInt(), q.value(3).toInt());
    return {};
}

QList<QPair<QString, QRect>> Storage::notePositions(qint64 id) const
{
    QList<QPair<QString, QRect>> out;
    QSqlQuery q(db());
    q.prepare(QStringLiteral(
        "SELECT screen_key,x,y,w,h FROM note_positions WHERE note_id=? ORDER BY screen_key"));
    q.addBindValue(id);
    if (!q.exec())
        return out;
    while (q.next())
        out.append({q.value(0).toString(),
                    QRect(q.value(1).toInt(), q.value(2).toInt(), q.value(3).toInt(), q.value(4).toInt())});
    return out;
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

void Storage::updateScroll(qint64 id, int scroll)
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral("UPDATE notes SET scroll=? WHERE id=?"));
    q.addBindValue(qMax(0, scroll));
    q.addBindValue(id);
    q.exec();
}

void Storage::updateOpacity(qint64 id, int opacity)
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral("UPDATE notes SET opacity=? WHERE id=?"));
    q.addBindValue(qBound(10, opacity, 100));
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

// ---------------------------------------------------------------- backup

namespace {
QJsonObject noteToJson(const Note &n, const QList<QPair<QString, QRect>> &positions)
{
    QJsonObject o;
    o[QStringLiteral("title")] = n.title;
    o[QStringLiteral("content")] = n.content;
    o[QStringLiteral("theme")] = n.themeId;
    o[QStringLiteral("x")] = n.x;
    o[QStringLiteral("y")] = n.y;
    o[QStringLiteral("w")] = n.w;
    o[QStringLiteral("h")] = n.h;
    o[QStringLiteral("layer")] = n.layer;
    o[QStringLiteral("visible")] = n.visible;
    o[QStringLiteral("collapsed")] = n.collapsed;
    o[QStringLiteral("scroll")] = n.scroll;
    o[QStringLiteral("opacity")] = n.opacity;
    o[QStringLiteral("deleted")] = n.deleted;
    o[QStringLiteral("created_at")] = n.createdAt;
    o[QStringLiteral("updated_at")] = n.updatedAt;
    o[QStringLiteral("deleted_at")] = n.deletedAt;
    o[QStringLiteral("last_screen_key")] = n.lastScreenKey;

    // Per-screen positions (screen key -> rect), so a restored backup keeps the
    // exact placement on every monitor the note has been on.
    QJsonArray posArr;
    for (const QPair<QString, QRect> &p : positions) {
        QJsonObject po;
        po[QStringLiteral("screen_key")] = p.first;
        po[QStringLiteral("x")] = p.second.x();
        po[QStringLiteral("y")] = p.second.y();
        po[QStringLiteral("w")] = p.second.width();
        po[QStringLiteral("h")] = p.second.height();
        posArr.append(po);
    }
    o[QStringLiteral("positions")] = posArr;
    return o;
}

QJsonObject themeToJson(const Theme &t)
{
    QJsonObject o;
    o[QStringLiteral("id")] = t.id;
    o[QStringLiteral("name")] = t.name;
    o[QStringLiteral("body")] = t.body;
    o[QStringLiteral("title_bar")] = t.titleBar;
    o[QStringLiteral("text_color")] = t.text;
    o[QStringLiteral("sort")] = t.sort;
    o[QStringLiteral("builtin")] = t.builtin;
    return o;
}
} // namespace

bool Storage::exportBackup(const QString &path, QString *errorOut) const
{
    QJsonArray noteArr;
    for (const Note &n : notes(true))
        noteArr.append(noteToJson(n, notePositions(n.id)));

    QJsonArray themeArr;
    for (const Theme &t : themes())
        themeArr.append(themeToJson(t));

    QJsonObject root;
    root[QStringLiteral("app")] = QStringLiteral("sticky-notes");
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("exported_at")] = nowIso();
    root[QStringLiteral("notes")] = noteArr;
    root[QStringLiteral("themes")] = themeArr;

    // QSaveFile writes to a temporary file and replaces the target atomically on
    // commit: a failed backup never leaves a half-written file behind.
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (errorOut)
            *errorOut = f.errorString();
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        if (errorOut)
            *errorOut = f.errorString();
        return false;
    }
    return true;
}

bool Storage::importBackup(const QString &path, bool replaceExisting, QString *errorOut)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorOut)
            *errorOut = f.errorString();
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parseError);
    const QJsonObject root = doc.object();
    if (doc.isNull() || !doc.isObject()
        || root.value(QStringLiteral("app")).toString() != QStringLiteral("sticky-notes")
        || !root.value(QStringLiteral("notes")).isArray()) {
        if (errorOut)
            *errorOut = parseError.errorString().isEmpty()
                            ? QStringLiteral("not a Sticky Notes backup")
                            : parseError.errorString();
        return false;
    }

    QSqlDatabase database = db();
    database.transaction();

    // Themes first: the imported notes reference them by id.
    for (const QJsonValue &v : root.value(QStringLiteral("themes")).toArray()) {
        const QJsonObject o = v.toObject();
        Theme t;
        t.id = o.value(QStringLiteral("id")).toString();
        if (t.id.isEmpty())
            continue;
        t.name = o.value(QStringLiteral("name")).toString();
        t.body = o.value(QStringLiteral("body")).toString();
        t.titleBar = o.value(QStringLiteral("title_bar")).toString();
        t.text = o.value(QStringLiteral("text_color")).toString();
        t.sort = o.value(QStringLiteral("sort")).toInt();
        t.builtin = o.value(QStringLiteral("builtin")).toBool(true);
        upsertTheme(t);
    }

    if (replaceExisting) {
        QSqlQuery del(database);
        del.exec(QStringLiteral("DELETE FROM notes"));
    }

    QSqlQuery ins(database);
    ins.prepare(QStringLiteral(
        "INSERT INTO notes (title,content,theme,x,y,w,h,layer,visible,collapsed,scroll,opacity,"
        "deleted,last_screen_key,created_at,updated_at,deleted_at) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
    QSqlQuery posIns(database);
    posIns.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO note_positions (note_id,screen_key,x,y,w,h) VALUES (?,?,?,?,?,?)"));
    const QString now = nowIso();
    for (const QJsonValue &v : root.value(QStringLiteral("notes")).toArray()) {
        const QJsonObject o = v.toObject();
        const QString created = o.value(QStringLiteral("created_at")).toString();
        const QString updated = o.value(QStringLiteral("updated_at")).toString();
        ins.addBindValue(bindText(o.value(QStringLiteral("title")).toString()));
        ins.addBindValue(bindText(o.value(QStringLiteral("content")).toString()));
        ins.addBindValue(bindText(o.value(QStringLiteral("theme")).toString(QStringLiteral("giallo"))));
        ins.addBindValue(o.value(QStringLiteral("x")).toInt(220));
        ins.addBindValue(o.value(QStringLiteral("y")).toInt(160));
        ins.addBindValue(o.value(QStringLiteral("w")).toInt(230));
        ins.addBindValue(o.value(QStringLiteral("h")).toInt(190));
        ins.addBindValue(bindText(o.value(QStringLiteral("layer")).toString(QStringLiteral("top"))));
        ins.addBindValue(o.value(QStringLiteral("visible")).toBool(true) ? 1 : 0);
        ins.addBindValue(o.value(QStringLiteral("collapsed")).toBool(false) ? 1 : 0);
        ins.addBindValue(o.value(QStringLiteral("scroll")).toInt());
        ins.addBindValue(qBound(10, o.value(QStringLiteral("opacity")).toInt(100), 100));
        ins.addBindValue(o.value(QStringLiteral("deleted")).toBool(false) ? 1 : 0);
        ins.addBindValue(bindText(o.value(QStringLiteral("last_screen_key")).toString()));
        ins.addBindValue(created.isEmpty() ? now : created);
        ins.addBindValue(updated.isEmpty() ? now : updated);
        ins.addBindValue(bindText(o.value(QStringLiteral("deleted_at")).toString()));
        if (!ins.exec()) {
            qWarning() << "Storage::importBackup:" << ins.lastError().text();
            database.rollback();
            if (errorOut)
                *errorOut = ins.lastError().text();
            return false;
        }

        // Per-screen positions of this note (absent in old backups). They are
        // keyed by the note id just inserted.
        const qint64 newId = ins.lastInsertId().toLongLong();
        for (const QJsonValue &pv : o.value(QStringLiteral("positions")).toArray()) {
            const QJsonObject po = pv.toObject();
            const QString key = po.value(QStringLiteral("screen_key")).toString();
            if (key.isEmpty())
                continue;
            posIns.addBindValue(newId);
            posIns.addBindValue(key);
            posIns.addBindValue(po.value(QStringLiteral("x")).toInt());
            posIns.addBindValue(po.value(QStringLiteral("y")).toInt());
            posIns.addBindValue(po.value(QStringLiteral("w")).toInt(230));
            posIns.addBindValue(po.value(QStringLiteral("h")).toInt(190));
            if (!posIns.exec()) {
                qWarning() << "Storage::importBackup (positions):" << posIns.lastError().text();
                database.rollback();
                if (errorOut)
                    *errorOut = posIns.lastError().text();
                return false;
            }
        }
    }

    if (!database.commit()) {
        if (errorOut)
            *errorOut = database.lastError().text();
        database.rollback();
        return false;
    }
    return true;
}
