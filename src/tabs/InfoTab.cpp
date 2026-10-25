#include "InfoTab.h"

#include "I18n.h"
#include "Storage.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QList>
#include <QLocale>
#include <QMessageBox>
#include <QPair>
#include <QPalette>
#include <QProgressDialog>
#include <QPushButton>
#include <QSysInfo>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QUrl>
#include <QVBoxLayout>
#include <qglobal.h> // qVersion()

namespace {
const QString kRepositoryUrl = QStringLiteral("https://github.com/danielearrighi/kde-sticky-notes");
const QString kReleasesUrl = QStringLiteral("https://github.com/danielearrighi/kde-sticky-notes/releases");
} // namespace

InfoTab::InfoTab(Updater *updater, QWidget *parent)
    : QWidget(parent)
    , m_updater(updater)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(12);

    auto *heading = new QLabel(I18n::t(QStringLiteral("info.heading")));
    {
        QFont headingFont = heading->font();
        headingFont.setBold(true);
        headingFont.setPointSize(headingFont.pointSize() + 4);
        heading->setFont(headingFont);
    }
    root->addWidget(heading);

    auto *tagline = new QLabel(I18n::t(QStringLiteral("info.tagline")));
    tagline->setWordWrap(true);
    tagline->setEnabled(false);
    root->addWidget(tagline);

    // Two-column grid: the first column is the field name, the second its value.
    const QList<QPair<QString, QString>> rows = {
        {I18n::t(QStringLiteral("info.version_label")), QCoreApplication::applicationVersion()},
        {I18n::t(QStringLiteral("info.author_label")), I18n::t(QStringLiteral("info.author"))},
        {I18n::t(QStringLiteral("info.repository_label")), kRepositoryUrl},
        {I18n::t(QStringLiteral("info.qt_label")), QString::fromLatin1(qVersion())},
        {I18n::t(QStringLiteral("info.libs_label")), I18n::t(QStringLiteral("info.libs_value"))},
        {I18n::t(QStringLiteral("info.os_label")), QSysInfo::prettyProductName()},
        {I18n::t(QStringLiteral("info.data_label")), Storage::instance().databasePath()},
    };

    auto *grid = new QTableWidget(rows.size(), 2, this);
    grid->horizontalHeader()->setVisible(false);
    grid->verticalHeader()->setVisible(false);
    grid->setShowGrid(true);
    grid->setAlternatingRowColors(true);
    grid->setEditTriggers(QAbstractItemView::NoEditTriggers);
    grid->setSelectionMode(QAbstractItemView::NoSelection);
    grid->setFocusPolicy(Qt::NoFocus);
    grid->setWordWrap(true);
    grid->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    grid->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    // Rows grow with the wrapped values (libraries, path) as the window resizes.
    grid->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);

    for (int row = 0; row < rows.size(); ++row) {
        const QPair<QString, QString> &pair = rows.at(row);

        auto *keyItem = new QTableWidgetItem(pair.first);
        QFont keyFont = keyItem->font();
        keyFont.setBold(true);
        keyItem->setFont(keyFont);
        grid->setItem(row, 0, keyItem);

        auto *valueItem = new QTableWidgetItem(pair.second);
        valueItem->setToolTip(pair.second);
        if (pair.second == kRepositoryUrl) {
            // Render the repository like a link; a click opens it (see below).
            QFont linkFont = valueItem->font();
            linkFont.setUnderline(true);
            valueItem->setFont(linkFont);
            valueItem->setForeground(grid->palette().color(QPalette::Link));
            valueItem->setData(Qt::UserRole, kRepositoryUrl);
            valueItem->setToolTip(I18n::t(QStringLiteral("info.open_repository")));
        }
        grid->setItem(row, 1, valueItem);
    }

    connect(grid, &QTableWidget::cellClicked, this, [grid](int row, int column) {
        if (column != 1)
            return;
        const QTableWidgetItem *item = grid->item(row, column);
        if (!item)
            return;
        const QUrl url(item->data(Qt::UserRole).toString());
        if (url.isValid() && !url.isEmpty())
            QDesktopServices::openUrl(url);
    });

    root->addWidget(grid);

    auto *updateRow = new QHBoxLayout;
    m_checkUpdates = new QPushButton(I18n::t(QStringLiteral("info.check_updates")));
    updateRow->addWidget(m_checkUpdates);
    updateRow->addStretch();
    root->addLayout(updateRow);

    connect(m_checkUpdates, &QPushButton::clicked, this, &InfoTab::checkForUpdates);

    root->addStretch();

    if (m_updater) {
        connect(m_updater, &Updater::checkFinished, this, &InfoTab::onCheckFinished);
        connect(m_updater, &Updater::downloadProgress, this, &InfoTab::onDownloadProgress);
        connect(m_updater, &Updater::installSucceeded, this, &InfoTab::onInstallSucceeded);
        connect(m_updater, &Updater::installFailed, this, &InfoTab::onInstallFailed);
        connect(m_updater, &Updater::installCancelled, this, &InfoTab::onInstallCancelled);
    }
}

void InfoTab::checkForUpdates()
{
    if (!m_updater)
        return;

    m_checkUpdates->setEnabled(false);
    m_checkUpdates->setText(I18n::t(QStringLiteral("info.checking_updates")));
    // The result arrives through checkFinished (also when this launch merged
    // into an automatic check that was already running).
    m_updater->checkNow(true);
}

void InfoTab::onCheckFinished(const Updater::CheckResult &result, bool userInitiated)
{
    m_checkUpdates->setEnabled(true);
    m_checkUpdates->setText(I18n::t(QStringLiteral("info.check_updates")));

    // Automatic checks are handled by AppController; stay silent unless the
    // user pressed the button.
    if (!userInitiated)
        return;

    if (!result.ok) {
        if (result.error == Updater::CheckError::InvalidResponse) {
            QMessageBox::warning(this,
                                 I18n::t(QStringLiteral("info.update_check_failed_title")),
                                 I18n::t(QStringLiteral("info.update_check_invalid_response")));
            return;
        }
        const QString reason = result.error == Updater::CheckError::HttpStatus
                                   ? QStringLiteral("HTTP %1").arg(result.errorDetail)
                                   : result.errorDetail;
        QMessageBox::warning(this, I18n::t(QStringLiteral("info.update_check_failed_title")),
                             I18n::t(QStringLiteral("info.update_check_failed")).arg(reason));
        return;
    }

    if (!result.updateAvailable) {
        QMessageBox::information(
            this, I18n::t(QStringLiteral("info.up_to_date_title")),
            I18n::t(QStringLiteral("info.up_to_date")).arg(QCoreApplication::applicationVersion()));
        return;
    }

    promptForUpdate(result);
}

void InfoTab::promptForUpdate(const Updater::CheckResult &result)
{
    const bool selfUpdate = m_updater && m_updater->canSelfUpdate();
    const bool canInstall = selfUpdate && !result.assetUrl.isEmpty();

    QWidget *parentWidget = dialogParent();
    QMessageBox box(parentWidget);
    box.setIcon(QMessageBox::Information);
    box.setWindowTitle(I18n::t(QStringLiteral("info.update_available_title")));
    box.setText(I18n::t(QStringLiteral("info.update_available"))
                    .arg(result.latestTag, QCoreApplication::applicationVersion()));

    QPushButton *install = nullptr;
    if (canInstall)
        install = box.addButton(I18n::t(QStringLiteral("info.install_now")), QMessageBox::AcceptRole);
    else if (!selfUpdate)
        box.setInformativeText(I18n::t(QStringLiteral("info.update_unsupported")));
    else
        box.setInformativeText(I18n::t(QStringLiteral("info.update_no_asset")));

    QPushButton *page =
        box.addButton(I18n::t(QStringLiteral("info.open_release_page")), QMessageBox::ActionRole);
    box.addButton(I18n::t(QStringLiteral("later")), QMessageBox::RejectRole);

    box.exec();
    if (install && box.clickedButton() == install) {
        startInstall();
    } else if (box.clickedButton() == page) {
        QDesktopServices::openUrl(QUrl(kReleasesUrl));
    } else {
        // "Later": do not propose this version again on the automatic checks.
        Storage::instance().setSetting(QStringLiteral("update_skipped_version"),
                                       result.latestVersion);
    }
}

void InfoTab::startInstall()
{
    if (!m_updater || m_progress)
        return;

    m_progress = new QProgressDialog(I18n::t(QStringLiteral("info.update_downloading")),
                                     I18n::t(QStringLiteral("cancel")), 0, 100, dialogParent());
    m_progress->setWindowTitle(I18n::t(QStringLiteral("info.update_available_title")));
    m_progress->setWindowModality(Qt::WindowModal);
    m_progress->setMinimumDuration(0);
    m_progress->setAutoClose(false);
    m_progress->setAutoReset(false);
    m_progress->setValue(0);
    connect(m_progress, &QProgressDialog::canceled, this, [this] {
        if (m_updater)
            m_updater->cancel();
    });
    m_progress->show();

    m_updater->downloadAndInstall();
}

void InfoTab::onDownloadProgress(qint64 received, qint64 total)
{
    if (!m_progress)
        return;

    if (total > 0) {
        m_progress->setMaximum(100);
        m_progress->setValue(static_cast<int>((received * 100) / total));
        m_progress->setLabelText(QStringLiteral("%1\n%2 / %3")
                                     .arg(I18n::t(QStringLiteral("info.update_downloading")),
                                          QLocale::system().formattedDataSize(received),
                                          QLocale::system().formattedDataSize(total)));
    } else {
        m_progress->setMaximum(0);
    }
}

void InfoTab::onInstallSucceeded()
{
    closeProgress();

    QWidget *parentWidget = dialogParent();
    QMessageBox box(parentWidget);
    box.setIcon(QMessageBox::Information);
    box.setWindowTitle(I18n::t(QStringLiteral("info.update_installed_title")));
    box.setText(I18n::t(QStringLiteral("info.update_restart_question")));
    QPushButton *restart = box.addButton(I18n::t(QStringLiteral("restart_now")),
                                         QMessageBox::AcceptRole);
    box.addButton(I18n::t(QStringLiteral("later")), QMessageBox::RejectRole);
    box.exec();

    if (box.clickedButton() == restart && m_updater && !m_updater->restartApplication()) {
        QMessageBox::warning(parentWidget,
                             I18n::t(QStringLiteral("info.update_install_failed_title")),
                             I18n::t(QStringLiteral("info.update_restart_manual")));
    }
}

void InfoTab::onInstallFailed(Updater::InstallError error, const QString &detail)
{
    closeProgress();

    const QString why = detail.isEmpty() ? I18n::t(QStringLiteral("info.unknown_error")) : detail;
    QString text;
    switch (error) {
    case Updater::InstallError::Unsupported:
        text = I18n::t(QStringLiteral("info.update_unsupported"));
        break;
    case Updater::InstallError::NoAsset:
        text = I18n::t(QStringLiteral("info.update_no_asset"));
        break;
    case Updater::InstallError::NotWritable:
        text = I18n::t(QStringLiteral("info.update_not_writable"));
        break;
    case Updater::InstallError::Checksum:
        text = I18n::t(QStringLiteral("info.update_checksum_failed"));
        break;
    case Updater::InstallError::SizeMismatch:
        text = I18n::t(QStringLiteral("info.update_size_mismatch"));
        break;
    case Updater::InstallError::Extract:
        text = I18n::t(QStringLiteral("info.update_extract_failed")).arg(why);
        break;
    case Updater::InstallError::Network:
    case Updater::InstallError::Replace:
    case Updater::InstallError::None:
        text = I18n::t(QStringLiteral("info.update_install_failed")).arg(why);
        break;
    }

    QMessageBox::warning(dialogParent(),
                         I18n::t(QStringLiteral("info.update_install_failed_title")), text);
}

void InfoTab::onInstallCancelled()
{
    closeProgress();
}

void InfoTab::closeProgress()
{
    if (!m_progress)
        return;
    m_progress->disconnect(this);
    m_progress->close();
    m_progress->deleteLater();
    m_progress = nullptr;
}

QWidget *InfoTab::dialogParent()
{
    // A hidden parent would place the dialog badly: fall back to the hub.
    return isVisible() ? this : window();
}
