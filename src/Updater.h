#pragma once

#include <QNetworkRequest>
#include <QObject>
#include <QString>

#include <memory>

class QFile;
class QNetworkAccessManager;
class QNetworkReply;
class QTemporaryDir;
class QTimer;

/// Checks GitHub for a newer release and, for the supported installation
/// layouts, downloads it and replaces the running binary.
///
/// Two layouts can update themselves:
///  - the AppImage: its runtime exports `$APPIMAGE`, the real file to replace;
///  - a user install from the release tarball: `<prefix>/bin/sticky-notes` next
///    to `<prefix>/share/applications/sticky-notes.desktop` (the tarball's
///    install.sh layout).
/// A development build (`build/sticky-notes`) and a distro package are reported
/// as Unsupported: the UI then only links to the release page.
///
/// The update is verified against the `SHA256SUMS` asset of the release (falling
/// back to the `digest` field of the GitHub API) and written atomically: the new
/// file is downloaded next to the target and then renamed over it, keeping the
/// previous one as `<target>.old` until the next successful start.
class Updater : public QObject
{
    Q_OBJECT
public:
    enum class InstallKind { AppImage, UserInstall, Unsupported };

    /// Why a check did not produce a version.
    enum class CheckError { None, Network, HttpStatus, InvalidResponse };

    /// Why an update could not be installed.
    enum class InstallError {
        None,
        Unsupported,
        NoAsset,
        NotWritable,
        Network,
        Checksum,
        SizeMismatch,
        Extract,
        Replace,
    };

    struct CheckResult
    {
        bool ok = false;
        bool updateAvailable = false;
        CheckError error = CheckError::None;
        QString errorDetail;     // network error string / HTTP status code
        QString latestVersion;   // normalized, e.g. "0.3.2"
        QString latestTag;       // e.g. "v0.3.2"
        QString assetName;       // release asset for this install, when any
        QString assetUrl;
        QString sumsUrl;
        qint64 assetSize = 0;
        QString expectedSha256;  // from the API `digest`, when provided
    };

    explicit Updater(QObject *parent = nullptr);
    ~Updater() override;

    InstallKind installKind() const { return m_kind; }
    /// File replaced by an update (the AppImage or the user-installed binary).
    QString targetPath() const { return m_target; }
    /// False for development builds and system packages.
    bool canSelfUpdate() const { return m_kind != InstallKind::Unsupported; }
    bool busy() const { return m_checking || m_downloading; }

    /// Queries the configured feed (the GitHub API unless
    /// `STICKY_NOTES_UPDATE_FEED` overrides it, useful for testing).
    /// When a check is already running, `userInitiated` is merged into it and
    /// that single check reports it, so pressing the button twice is harmless.
    void checkNow(bool userInitiated = false);

    /// Downloads and installs the version found by the last successful check.
    void downloadAndInstall();

    /// Aborts a running download; the partial file is removed.
    void cancel();

    /// Starts the (already replaced) executable with `--restart` and quits.
    bool restartApplication();

signals:
    void checkFinished(const Updater::CheckResult &result, bool userInitiated);
    void downloadProgress(qint64 received, qint64 total);
    void installSucceeded();
    /// `error` is an untranslated technical detail (network/tar message).
    void installFailed(Updater::InstallError error, const QString &errorDetail);
    void installCancelled();

private:
    void onCheckReply(QNetworkReply *reply);
    void startSumsDownload();
    void onSumsReply(QNetworkReply *reply);
    void startAssetDownload();
    void onAssetReply(QNetworkReply *reply);
    void installDownloadedFile(const QString &downloaded);
    void failInstall(InstallError error, const QString &errorDetail = QString());
    void resetDownload();

    QNetworkRequest makeRequest(const QUrl &url) const;

    InstallKind m_kind = InstallKind::Unsupported;
    QString m_target;

    QNetworkAccessManager *m_network = nullptr;
    QNetworkReply *m_reply = nullptr;
    QTimer *m_stallTimer = nullptr;
    QFile *m_partFile = nullptr;
    QString m_downloadPath;
    std::unique_ptr<QTemporaryDir> m_tmpDir;

    bool m_checking = false;
    bool m_promptPending = false;
    bool m_downloading = false;
    bool m_cancelled = false;
    bool m_stalled = false;

    CheckResult m_last;
    QString m_expectedSha256;
};
