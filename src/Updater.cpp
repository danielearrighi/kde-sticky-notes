#include "Updater.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QVersionNumber>

#include <cerrno>
#include <cstdio>

namespace {
const QString kApiUrl =
    QStringLiteral("https://api.github.com/repos/danielearrighi/kde-sticky-notes/releases/latest");
constexpr int kCheckTimeoutMs = 15000;
constexpr int kSumsTimeoutMs = 15000;
constexpr int kStallTimeoutMs = 60000;
constexpr int kExtractTimeoutMs = 60000;

/// The GitHub API URL, unless the hidden STICKY_NOTES_UPDATE_FEED override is
/// set (a URL to a releases/latest-like JSON, used to test the update flow
/// against a local server without publishing a release).
QString feedUrl()
{
    const QString override = qEnvironmentVariable("STICKY_NOTES_UPDATE_FEED");
    return override.isEmpty() ? kApiUrl : override;
}

/// Release assets are named after `uname -m` (scripts/create_release.sh): map
/// the Qt architecture names onto it.
QString releaseArch()
{
    const QString cpu = QSysInfo::buildCpuArchitecture();
    if (cpu == QLatin1String("arm64") || cpu == QLatin1String("arm64e"))
        return QStringLiteral("aarch64");
    return cpu.isEmpty() ? QStringLiteral("x86_64") : cpu;
}

QString normalizedVersion(QString tag)
{
    tag = tag.trimmed();
    if (tag.startsWith(QLatin1Char('v'), Qt::CaseInsensitive))
        tag.remove(0, 1);
    return tag;
}

QString sha256OfFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file))
        return QString();
    return QString::fromLatin1(hash.result().toHex());
}

/// The SHA-256 of \a fileName from the `SHA256SUMS` asset.
QString sha256FromSums(const QByteArray &sums, const QString &fileName)
{
    const QString wanted = QFileInfo(fileName).fileName();
    const QStringList lines = QString::fromUtf8(sums).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        // sha256sum separates the hash from the name with two spaces (or a
        // leading "*" for binary mode); tolerate any run of spaces.
        const QStringList parts =
            line.trimmed().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.size() < 2)
            continue;
        const QString hash = parts.first().trimmed();
        QString name = parts.last().trimmed();
        if (name.startsWith(QLatin1Char('*')))
            name.remove(0, 1);
        if (name == wanted && hash.size() == 64)
            return hash;
    }
    return QString();
}
} // namespace

Updater::Updater(QObject *parent)
    : QObject(parent)
{
    // The AppImage runtime exports the real file: applicationFilePath() points
    // inside the temporary mount and would disappear when the app exits.
    const QString appImage =
        QFileInfo(QFile::decodeName(qgetenv("APPIMAGE"))).canonicalFilePath();
    if (!appImage.isEmpty()) {
        m_kind = InstallKind::AppImage;
        m_target = appImage;
    } else {
        // A user install from the release tarball: <prefix>/bin/sticky-notes
        // next to <prefix>/share/applications/sticky-notes.desktop. System
        // packages (in /usr/bin) are not writable, so they end up Unsupported.
        const QFileInfo exe(QCoreApplication::applicationFilePath());
        const QDir binDir(exe.canonicalPath());
        // Inside a mounted AppImage without $APPIMAGE (never expected): the
        // temporary mount must not be mistaken for a user install.
        if (binDir.absolutePath().contains(QStringLiteral("/.mount_"))) {
            m_kind = InstallKind::Unsupported;
        } else if (binDir.dirName() == QLatin1String("bin")) {
            const QString prefix = QFileInfo(binDir.absolutePath()).absolutePath();
            const QString desktop =
                QDir(prefix).filePath(QStringLiteral("share/applications/sticky-notes.desktop"));
            const QFileInfo canonical(exe.canonicalFilePath());
            if (QFileInfo::exists(desktop) && canonical.isWritable()
                && QFileInfo(canonical.absolutePath()).isWritable()) {
                m_kind = InstallKind::UserInstall;
                m_target = canonical.absoluteFilePath();
            }
        }
    }

    // A leftover ".old" from the previous update means the new binary started:
    // the rollback copy is no longer needed.
    if (!m_target.isEmpty()) {
        const QString old = m_target + QStringLiteral(".old");
        if (QFileInfo::exists(old))
            QFile::remove(old);
    }

    m_stallTimer = new QTimer(this);
    m_stallTimer->setSingleShot(true);
    m_stallTimer->setInterval(kStallTimeoutMs);
    connect(m_stallTimer, &QTimer::timeout, this, [this] {
        if (!m_reply || m_cancelled)
            return;
        m_stalled = true;
        m_reply->abort();
    });
}

Updater::~Updater() = default;

QNetworkRequest Updater::makeRequest(const QUrl &url) const
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("StickyNotes/%1").arg(QCoreApplication::applicationVersion()));
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    return request;
}

void Updater::checkNow(bool userInitiated)
{
    if (m_downloading)
        return;
    if (m_checking) {
        m_promptPending = m_promptPending || userInitiated;
        return;
    }

    m_checking = true;
    m_promptPending = userInitiated;
    if (!m_network)
        m_network = new QNetworkAccessManager(this);

    QNetworkRequest request = makeRequest(QUrl(feedUrl()));
    request.setRawHeader("Accept", "application/vnd.github+json");

    QNetworkReply *reply = m_network->get(request);
    // Do not leave the caller stuck when GitHub is unreachable.
    QTimer::singleShot(kCheckTimeoutMs, reply, [reply] {
        if (reply->isRunning())
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] { onCheckReply(reply); });
}

void Updater::onCheckReply(QNetworkReply *reply)
{
    reply->deleteLater();

    CheckResult result;
    const auto finish = [this, &result] {
        m_last = result;
        m_checking = false;
        const bool prompt = m_promptPending;
        m_promptPending = false;
        emit checkFinished(result, prompt);
    };

    if (reply->error() != QNetworkReply::NoError) {
        result.error = CheckError::Network;
        result.errorDetail = reply->errorString();
        finish();
        return;
    }

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    // A file:// feed (tests) has no HTTP status.
    if (status != 0 && status != 200) {
        result.error = CheckError::HttpStatus;
        result.errorDetail = QString::number(status);
        finish();
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    if (!doc.isObject()) {
        result.error = CheckError::InvalidResponse;
        finish();
        return;
    }

    const QJsonObject release = doc.object();
    const QString tag = release.value(QStringLiteral("tag_name")).toString();
    const QVersionNumber latest = QVersionNumber::fromString(normalizedVersion(tag));
    if (latest.isNull()) {
        result.error = CheckError::InvalidResponse;
        finish();
        return;
    }

    result.ok = true;
    result.latestTag = tag;
    result.latestVersion = normalizedVersion(tag);

    const QVersionNumber installed =
        QVersionNumber::fromString(QCoreApplication::applicationVersion());
    if (QVersionNumber::compare(latest, installed) <= 0) {
        finish();
        return;
    }
    result.updateAvailable = true;

    // Pick the asset for this installation and architecture. Without a matching
    // asset the UI still reports the new version, but only offers the release
    // page (the download step then fails with NoAsset).
    const QString arch = releaseArch();
    const QJsonArray assets = release.value(QStringLiteral("assets")).toArray();
    for (const QJsonValue &value : assets) {
        const QJsonObject asset = value.toObject();
        const QString name = asset.value(QStringLiteral("name")).toString();
        const QString url = asset.value(QStringLiteral("browser_download_url")).toString();
        if (name.isEmpty() || url.isEmpty())
            continue;

        if (name.compare(QStringLiteral("SHA256SUMS"), Qt::CaseInsensitive) == 0) {
            result.sumsUrl = url;
            continue;
        }

        const bool isAppImage = name.endsWith(QStringLiteral(".AppImage"), Qt::CaseInsensitive);
        const bool isTarball = name.endsWith(QStringLiteral(".tar.gz"), Qt::CaseInsensitive);
        const bool wanted = (m_kind == InstallKind::AppImage && isAppImage)
                            || (m_kind == InstallKind::UserInstall && isTarball);
        if (!wanted || !name.contains(arch) || !result.assetUrl.isEmpty())
            continue;

        result.assetName = name;
        result.assetUrl = url;
        result.assetSize = asset.value(QStringLiteral("size")).toInteger();
        const QString digest = asset.value(QStringLiteral("digest")).toString();
        if (digest.startsWith(QStringLiteral("sha256:"), Qt::CaseInsensitive))
            result.expectedSha256 = digest.mid(7).trimmed();
    }

    finish();
}

void Updater::downloadAndInstall()
{
    if (m_downloading)
        return;
    if (!canSelfUpdate()) {
        emit installFailed(InstallError::Unsupported, QString());
        return;
    }
    if (m_last.assetUrl.isEmpty() || m_last.assetName.isEmpty()) {
        emit installFailed(InstallError::NoAsset, QString());
        return;
    }

    const QFileInfo target(m_target);
    if (!target.isWritable() || !QFileInfo(target.absolutePath()).isWritable()) {
        emit installFailed(InstallError::NotWritable, QString());
        return;
    }

    if (!m_network)
        m_network = new QNetworkAccessManager(this);

    m_downloading = true;
    m_cancelled = false;
    m_stalled = false;
    m_expectedSha256 = m_last.expectedSha256;

    // Prefer the published checksums; the API digest is only a fallback (older
    // releases, or GitHub not exposing it).
    if (!m_last.sumsUrl.isEmpty()) {
        startSumsDownload();
        return;
    }
    if (m_expectedSha256.isEmpty()) {
        failInstall(InstallError::Checksum);
        return;
    }
    startAssetDownload();
}

void Updater::startSumsDownload()
{
    m_reply = m_network->get(makeRequest(QUrl(m_last.sumsUrl)));
    QNetworkReply *reply = m_reply;
    QTimer::singleShot(kSumsTimeoutMs, reply, [reply] {
        if (reply->isRunning())
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] { onSumsReply(reply); });
}

void Updater::onSumsReply(QNetworkReply *reply)
{
    if (m_reply == reply)
        m_reply = nullptr;
    reply->deleteLater();

    if (m_cancelled) {
        resetDownload();
        emit installCancelled();
        return;
    }
    if (m_stalled) {
        failInstall(InstallError::Network, QStringLiteral("timeout"));
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        failInstall(InstallError::Network, reply->errorString());
        return;
    }
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status != 0 && status != 200) {
        failInstall(InstallError::Network, QStringLiteral("HTTP %1").arg(status));
        return;
    }

    const QString hash = sha256FromSums(reply->readAll(), m_last.assetName);
    if (hash.isEmpty()) {
        failInstall(InstallError::Checksum);
        return;
    }
    m_expectedSha256 = hash;
    startAssetDownload();
}

void Updater::startAssetDownload()
{
    const QFileInfo target(m_target);
    if (m_kind == InstallKind::UserInstall) {
        // Extract next to the target so the final rename stays on one filesystem.
        m_tmpDir = std::make_unique<QTemporaryDir>(
            target.absolutePath() + QStringLiteral("/.sticky-notes-update-XXXXXX"));
        if (!m_tmpDir->isValid()) {
            m_tmpDir.reset();
            failInstall(InstallError::NotWritable);
            return;
        }
        m_downloadPath = m_tmpDir->filePath(QFileInfo(m_last.assetName).fileName());
    } else {
        m_downloadPath = m_target + QStringLiteral(".part");
        QFile::remove(m_downloadPath);
    }

    m_partFile = new QFile(m_downloadPath);
    if (!m_partFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        failInstall(InstallError::NotWritable);
        return;
    }

    m_reply = m_network->get(makeRequest(QUrl(m_last.assetUrl)));
    QNetworkReply *reply = m_reply;
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
        if (m_reply != reply || !m_partFile)
            return;
        const QByteArray chunk = reply->readAll();
        if (m_partFile->write(chunk) != chunk.size()) {
            m_stalled = true;
            reply->abort();
            return;
        }
        m_stallTimer->start();
    });
    connect(reply, &QNetworkReply::downloadProgress, this,
            [this, reply](qint64 received, qint64 total) {
                if (m_reply != reply)
                    return;
                const qint64 size = m_last.assetSize > 0 ? m_last.assetSize : total;
                emit downloadProgress(received, size);
            });
    connect(reply, &QNetworkReply::finished, this, [this, reply] { onAssetReply(reply); });
    m_stallTimer->start();
}

void Updater::onAssetReply(QNetworkReply *reply)
{
    if (m_reply == reply)
        m_reply = nullptr;
    reply->deleteLater();
    m_stallTimer->stop();

    if (m_partFile)
        m_partFile->close();

    if (m_cancelled) {
        resetDownload();
        emit installCancelled();
        return;
    }
    if (m_stalled) {
        failInstall(InstallError::Network, QStringLiteral("timeout"));
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        failInstall(InstallError::Network, reply->errorString());
        return;
    }
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status != 0 && status != 200) {
        failInstall(InstallError::Network, QStringLiteral("HTTP %1").arg(status));
        return;
    }
    if (!m_partFile) {
        failInstall(InstallError::Network);
        return;
    }

    const qint64 size = m_partFile->size();
    if (m_last.assetSize > 0 && size != m_last.assetSize) {
        failInstall(InstallError::SizeMismatch,
                    QStringLiteral("%1/%2").arg(size).arg(m_last.assetSize));
        return;
    }

    const QString actual = sha256OfFile(m_downloadPath);
    if (actual.isEmpty() || actual.compare(m_expectedSha256, Qt::CaseInsensitive) != 0) {
        failInstall(InstallError::Checksum);
        return;
    }

    installDownloadedFile(m_downloadPath);
}

void Updater::installDownloadedFile(const QString &downloaded)
{
    QString binary = downloaded;

    if (m_kind == InstallKind::UserInstall) {
        // The release tarball contains sticky-notes-<version>-<arch>/sticky-notes
        // (plus .desktop, icon and install.sh): only the binary is replaced.
        QProcess tar;
        tar.setProgram(QStringLiteral("tar"));
        tar.setArguments({QStringLiteral("-xzf"), downloaded, QStringLiteral("-C"),
                          m_tmpDir->path(), QStringLiteral("--no-same-owner")});
        tar.start();
        if (!tar.waitForStarted(5000)) {
            failInstall(InstallError::Extract, tar.errorString());
            return;
        }
        if (!tar.waitForFinished(kExtractTimeoutMs)) {
            tar.kill();
            tar.waitForFinished(1000);
            failInstall(InstallError::Extract, QStringLiteral("timeout"));
            return;
        }
        if (tar.exitStatus() != QProcess::NormalExit || tar.exitCode() != 0) {
            failInstall(InstallError::Extract,
                        QString::fromLocal8Bit(tar.readAllStandardError()).trimmed());
            return;
        }

        QDirIterator it(m_tmpDir->path(), {QStringLiteral("sticky-notes")}, QDir::Files,
                        QDirIterator::Subdirectories);
        if (it.hasNext())
            binary = it.next();
        if (binary.isEmpty() || !QFileInfo::exists(binary)) {
            failInstall(InstallError::Extract, QStringLiteral("sticky-notes not found"));
            return;
        }
    }

    QFile::setPermissions(binary, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                       | QFileDevice::ExeOwner | QFileDevice::ReadGroup
                                       | QFileDevice::ExeGroup | QFileDevice::ReadOther
                                       | QFileDevice::ExeOther);

    // The user may have cancelled while tar was extracting.
    if (m_cancelled) {
        resetDownload();
        emit installCancelled();
        return;
    }

    // Atomic replace: keep the current file as ".old" (dropped on the next
    // start) and rename the new one over the target. Both live on the same
    // filesystem, so the second rename is atomic.
    const QString backup = m_target + QStringLiteral(".old");
    QFile::remove(backup);
    if (::rename(QFile::encodeName(m_target).constData(), QFile::encodeName(backup).constData())
        != 0) {
        failInstall(InstallError::Replace, qt_error_string(errno));
        return;
    }
    if (::rename(QFile::encodeName(binary).constData(), QFile::encodeName(m_target).constData())
        != 0) {
        ::rename(QFile::encodeName(backup).constData(), QFile::encodeName(m_target).constData());
        failInstall(InstallError::Replace, qt_error_string(errno));
        return;
    }

    resetDownload();
    emit installSucceeded();
}

void Updater::cancel()
{
    if (!m_downloading)
        return;
    m_cancelled = true;
    if (m_reply)
        m_reply->abort();
}

bool Updater::restartApplication()
{
    // For the AppImage the target is the file the user launched; the mounted
    // AppDir (applicationFilePath) goes away with this process.
    const QString exe = m_kind == InstallKind::AppImage ? m_target
                                                        : QCoreApplication::applicationFilePath();
    if (exe.isEmpty())
        return false;
    if (!QProcess::startDetached(exe, {QStringLiteral("--restart")}))
        return false;
    QCoreApplication::quit();
    return true;
}

void Updater::failInstall(InstallError error, const QString &errorDetail)
{
    resetDownload();
    emit installFailed(error, errorDetail);
}

void Updater::resetDownload()
{
    if (m_stallTimer)
        m_stallTimer->stop();
    if (m_reply) {
        QNetworkReply *reply = m_reply;
        m_reply = nullptr;
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    if (m_partFile) {
        m_partFile->close();
        if (!m_downloadPath.isEmpty())
            QFile::remove(m_downloadPath);
        delete m_partFile;
        m_partFile = nullptr;
    }
    m_tmpDir.reset();
    m_downloadPath.clear();
    m_downloading = false;
    m_cancelled = false;
    m_stalled = false;
}
