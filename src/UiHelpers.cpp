#include "UiHelpers.h"

#include "Storage.h"

#include <QColor>
#include <QDateTime>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>

namespace UiHelpers
{

QString plainSnippet(const QString &markdown, int maxLen)
{
    QString s = markdown;
    s.remove(QRegularExpression(QStringLiteral("[#*_>`~\\[\\]()!-]")));
    s.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    s = s.trimmed();
    if (s.size() > maxLen)
        s = s.left(maxLen) + QStringLiteral("\u2026");
    return s;
}

QPushButton *makeCardButton(const QString &text)
{
    auto *b = new QPushButton(text);
    b->setCursor(Qt::PointingHandCursor);
    b->setFixedHeight(24);
    return b;
}

QIcon swatchIcon(const QString &hex)
{
    QPixmap pm(14, 14);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(QColor(hex));
    p.setPen(QPen(QColor("#00000055")));
    p.drawRoundedRect(QRectF(1, 1, 12, 12), 3, 3);
    p.end();
    return QIcon(pm);
}

QIcon randomSwatchIcon()
{
    QPixmap pm(14, 14);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor("#00000055")));
    const QColor colors[] = {QColor(QStringLiteral("#FCFC97")), QColor(QStringLiteral("#FFC7D9")),
                             QColor(QStringLiteral("#BEE7FF")), QColor(QStringLiteral("#C8F7C5"))};
    for (int i = 0; i < 4; ++i) {
        p.setBrush(colors[i]);
        p.drawRoundedRect(QRectF(1 + (i % 2) * 6, 1 + (i / 2) * 6, 6, 6), 2, 2);
    }
    p.end();
    return QIcon(pm);
}

QString defaultDateFormat()
{
    return QStringLiteral("dd/MM/yyyy HH:mm");
}

QString formatWhen(const QString &iso)
{
    if (iso.isEmpty())
        return QStringLiteral("\u2014");
    const QDateTime dt = QDateTime::fromString(iso, Qt::ISODate);
    if (!dt.isValid())
        return iso;
    const QString fmt = Storage::instance().setting(QStringLiteral("date_format"), defaultDateFormat());
    return dt.toLocalTime().toString(fmt.isEmpty() ? defaultDateFormat() : fmt);
}

} // namespace UiHelpers
