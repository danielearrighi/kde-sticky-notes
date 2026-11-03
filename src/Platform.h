#pragma once

#include <QFileInfo>
#include <QString>

/// Runtime detection of the Flatpak sandbox.
///
/// Unlike the AppImage, Flatpak exports no variable with the bundle path: the
/// app lives in the read-only /app prefix and the sandbox only writes to
/// ~/.var/app/<id>. `/.flatpak-info` exists for as long as the sandbox runs and
/// $FLATPAK_ID carries the application id (also usable to restart the app from
/// the host, e.g. in the autostart entry).
namespace Platform
{

/// Application id used by the Flatpak manifest (packaging/*.yml).
inline QString flatpakAppId()
{
    const QString id = qEnvironmentVariable("FLATPAK_ID");
    return id.isEmpty() ? QStringLiteral("io.github.danielearrighi.StickyNotes") : id;
}

inline bool isFlatpak()
{
    return QFileInfo::exists(QStringLiteral("/.flatpak-info"));
}

} // namespace Platform
