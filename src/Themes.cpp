#include "Themes.h"

#include <QColor>
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
        fromBodyColor(QStringLiteral("giallo"),  QStringLiteral("Giallo"),  QStringLiteral("#FCFC97"), 0),
        fromBodyColor(QStringLiteral("rosa"),    QStringLiteral("Rosa"),    QStringLiteral("#FD98CA"), 1),
        fromBodyColor(QStringLiteral("azzurro"), QStringLiteral("Azzurro"), QStringLiteral("#CBFEFE"), 2),
        fromBodyColor(QStringLiteral("verde"),   QStringLiteral("Verde"),   QStringLiteral("#C3F5B0"), 3),
        fromBodyColor(QStringLiteral("lilla"),   QStringLiteral("Lilla"),   QStringLiteral("#DCC0F5"), 4),
        fromBodyColor(QStringLiteral("arancio"), QStringLiteral("Arancio"), QStringLiteral("#FFD3A0"), 5),
        fromBodyColor(QStringLiteral("grigio"),  QStringLiteral("Grigio"),  QStringLiteral("#E8E8E8"), 6),
    };
}

QString idFromName(const QString &name)
{
    QString id = name.toLower().trimmed();
    id.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("-"));
    id.remove(QRegularExpression(QStringLiteral("(^-|-$)")));
    return id.isEmpty() ? QStringLiteral("tema") : id;
}

} // namespace Themes
