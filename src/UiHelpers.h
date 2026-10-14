#pragma once

#include <QIcon>
#include <QString>

class QPushButton;

/// Formatting utilities and widgets shared between the hub tabs.
namespace UiHelpers
{

/// Converts Markdown into a single plain-text line for previews.
QString plainSnippet(const QString &markdown, int maxLen = 90);

/// Compact "hand" cursor button used in cards and bars.
QPushButton *makeCardButton(const QString &text);

/// Colored swatch used as a theme preview (instead of a full background or the
/// hex code): shows the note body color.
QIcon swatchIcon(const QString &hex);

/// Multicolor variant for the "Random" entry, which has no single body color.
QIcon randomSwatchIcon();

/// Default date/time format, centralized as a setting for a future i18n.
QString defaultDateFormat();

/// Dates are stored as ISO UTC: we show them in the local timezone.
QString formatWhen(const QString &iso);

} // namespace UiHelpers
