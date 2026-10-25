#pragma once

#include "Updater.h"

#include <QWidget>

class QProgressDialog;
class QPushButton;

/// "Info" tab: application, Qt/libraries and author information, plus the
/// update check (the network/install work lives in Updater).
class InfoTab : public QWidget
{
    Q_OBJECT
public:
    explicit InfoTab(Updater *updater, QWidget *parent = nullptr);

    /// Shows the "update available" dialog for an update found by an automatic
    /// check (AppController routes it here so the UI lives in one place).
    void promptForUpdate(const Updater::CheckResult &result);

private:
    void checkForUpdates();
    void onCheckFinished(const Updater::CheckResult &result, bool userInitiated);
    void startInstall();
    void onDownloadProgress(qint64 received, qint64 total);
    void onInstallSucceeded();
    void onInstallFailed(Updater::InstallError error, const QString &detail);
    void onInstallCancelled();
    void closeProgress();
    /// Dialog parent: the tab itself when it is the visible page, otherwise the
    /// hub (an automatic check prompts while another tab may be in front).
    QWidget *dialogParent();

    Updater *m_updater = nullptr;
    QPushButton *m_checkUpdates = nullptr;
    QProgressDialog *m_progress = nullptr;
};
