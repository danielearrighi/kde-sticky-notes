#include "Themes.h"
#include "I18n.h"

#include <QColor>
#include <QHash>
#include <QRegularExpression>

namespace Themes
{

QString darken(const QString &hex, double factor)
{
    QColor c(hex);
    if (!c.isValid())
        return hex;
    c = c.darker(int(100.0 / factor));
    return c.name();
}

Theme fromBodyColor(const QString &id, const QString &name, const QString &body, int sort)
{
    Theme t;
    t.id = id;
    t.name = name;
    t.body = body;
    t.titleBar = darken(body, 0.92);
    t.text = QStringLiteral("#45353B");
    t.sort = sort;
    t.builtin = true;
    return t;
}

QList<Theme> defaults()
{
    return {
        fromBodyColor(QStringLiteral("giallo"),  QStringLiteral("Yellow"),  QStringLiteral("#FCFC97"), 0),
        fromBodyColor(QStringLiteral("rosa"),    QStringLiteral("Pink"),    QStringLiteral("#FD98CA"), 1),
        fromBodyColor(QStringLiteral("azzurro"), QStringLiteral("Sky blue"),QStringLiteral("#CBFEFE"), 2),
        fromBodyColor(QStringLiteral("verde"),   QStringLiteral("Green"),   QStringLiteral("#C3F5B0"), 3),
        fromBodyColor(QStringLiteral("lilla"),   QStringLiteral("Lilac"),   QStringLiteral("#DCC0F5"), 4),
        fromBodyColor(QStringLiteral("arancio"), QStringLiteral("Orange"),  QStringLiteral("#FFD3A0"), 5),
        fromBodyColor(QStringLiteral("grigio"),  QStringLiteral("Grey"),    QStringLiteral("#E8E8E8"), 6),
    };
}

QString displayName(const Theme &t)
{
    // Custom themes carry a user-chosen name: never translate it. Builtin names
    // are translated through their stable id (the stored English name is only a
    // fallback, e.g. for an unknown id).
    if (!t.builtin)
        return t.name;

    static const QHash<QString, QString> keys = {
        {QStringLiteral("giallo"),  QStringLiteral("colors.yellow")},
        {QStringLiteral("rosa"),    QStringLiteral("colors.pink")},
        {QStringLiteral("azzurro"), QStringLiteral("colors.sky_blue")},
        {QStringLiteral("verde"),   QStringLiteral("colors.green")},
        {QStringLiteral("lilla"),   QStringLiteral("colors.lilac")},
        {QStringLiteral("arancio"), QStringLiteral("colors.orange")},
        {QStringLiteral("grigio"),  QStringLiteral("colors.grey")},
    };
    const auto it = keys.constFind(t.id);
    if (it == keys.constEnd())
        return t.name;
    return I18n::t(it.value());
}

QString idFromName(const QString &name)
{
    QString id = name.toLower().trimmed();
    id.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("-"));
    id.remove(QRegularExpression(QStringLiteral("(^-|-$)")));
    return id.isEmpty() ? QStringLiteral("tema") : id;
}

} // namespace Themes
