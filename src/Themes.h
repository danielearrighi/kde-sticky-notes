#pragma once

#include "Models.h"
#include <QList>
#include <QString>

namespace Themes
{
/// Special value for the "default_theme" setting: pick a random theme among the
/// available ones when a new note is created.
inline const QString kRandomId = QStringLiteral("random");

/// Default themes, with the post-it palette (yellow/pink/sky/green/lilac/orange/grey).
QList<Theme> defaults();

/// Builds a palette preview from the body color alone.
Theme fromBodyColor(const QString &id, const QString &name, const QString &body, int sort);

/// Display name of a theme. Builtin names are translated from their stable id
/// (the stored English name is only a fallback); custom theme names are
/// user-provided and returned unchanged.
QString displayName(const Theme &t);

QString darken(const QString &hex, double factor);
QString idFromName(const QString &name);
} // namespace Themes
