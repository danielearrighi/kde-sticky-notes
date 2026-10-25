#include "UiHelpers.h"

#include "I18n.h"
#include "Storage.h"

#include <QApplication>
#include <QColor>
#include <QDateTime>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>

namespace UiHelpers
{

QString untitled()
{
    return I18n::t(QStringLiteral("untitled"));
}

QString plainSnippet(const QString &markdown, int maxLen)
{
    QString s = markdown;
    // Images are embedded as data URLs (see NoteWindow::insertImage()): drop
    // them before the cleanup below, otherwise the preview would show the
    // base64 payload.
    s.remove(QRegularExpression(QStringLiteral("!\\[[^\\]]*\\]\\([^)]*\\)")));
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

QIcon trayIcon(const QString &mode, const QString &path)
{
    if (mode == trayIconApp())
        return QIcon(QStringLiteral(":/icons/sticky-notes.png"));

    if (mode == trayIconCustom() && !path.isEmpty()) {
        const QIcon custom(path);
        // Unreadable/missing file: fall through to the automatic icon instead of
        // leaving an empty slot in the panel.
        if (!custom.isNull() && !custom.pixmap(QSize(32, 32)).isNull())
            return custom;
    }

    // Automatic: the bundled monochrome glyph, tinted with the theme foreground
    // (white on a dark panel, dark on a light one).
    QImage mask = QIcon(QStringLiteral(":/icons/sticky-notes-symbolic.svg"))
                      .pixmap(QSize(64, 64))
                      .toImage();
    if (mask.isNull())
        return QIcon(QStringLiteral(":/icons/sticky-notes.png"));

    QColor fg = QApplication::palette().color(QPalette::WindowText);
    if (!fg.isValid() || fg.alpha() == 0)
        fg = Qt::white;

    QImage tinted(mask.size(), QImage::Format_ARGB32_Premultiplied);
    tinted.fill(Qt::transparent);
    QPainter painter(&tinted);
    painter.drawImage(0, 0, mask);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(tinted.rect(), fg);
    painter.end();

    // One 64 px pixmap marked as 1x on purpose: the QSystemTrayIcon D-Bus
    // exporter publishes a single entry, so multiple sizes can make it pick the
    // smallest (16 px) and the panel then draws the icon tiny. Inheriting the
    // application device pixel ratio is equally wrong: on HiDPI it advertises
    // 128 px "logical" entries. A 64 px raster is scaled down by every host.
    QPixmap pm = QPixmap::fromImage(tinted);
    pm.setDevicePixelRatio(1.0);
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
