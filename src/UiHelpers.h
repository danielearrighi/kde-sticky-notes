#pragma once

#include <QIcon>
#include <QString>

class QPushButton;

/// Formatting utilities and widgets shared between the hub tabs.
namespace UiHelpers
{

/// Shared label shown for a note without a title.
QString untitled();

/// Converts Markdown into a single plain-text line for previews.
QString plainSnippet(const QString &markdown, int maxLen = 90);

/// Compact "hand" cursor button used in cards and bars.
QPushButton *makeCardButton(const QString &text);

/// Colored swatch used as a theme preview (instead of a full background or the
/// hex code): shows the note body color.
QIcon swatchIcon(const QString &hex);

/// Multicolor variant for the "Random" entry, which has no single body color.
QIcon randomSwatchIcon();

/// Values stored in the "tray_icon" setting.
inline QString trayIconAuto() { return QStringLiteral("auto"); }
inline QString trayIconApp() { return QStringLiteral("app"); }
inline QString trayIconCustom() { return QStringLiteral("custom"); }

/// Icon for the system tray. "auto" is the bundled monochrome glyph tinted
/// with the theme foreground; "app" is the colored application icon; "custom"
/// loads \a path and falls back to "auto" when the file is missing or
/// unreadable.
QIcon trayIcon(const QString &mode, const QString &path);

/// Default date/time format, centralized as a setting for a future i18n.
QString defaultDateFormat();

/// Dates are stored as ISO UTC: we show them in the local timezone.
QString formatWhen(const QString &iso);

} // namespace UiHelpers
