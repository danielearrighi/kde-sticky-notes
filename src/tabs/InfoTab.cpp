#include "InfoTab.h"

#include "I18n.h"
#include "Storage.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QList>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPair>
#include <QPalette>
#include <QPushButton>
#include <QSysInfo>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QVersionNumber>
#include <qglobal.h> // qVersion()

namespace {
const QString kRepositoryUrl = QStringLiteral("https://github.com/danielearrighi/kde-sticky-notes");
const QString kReleasesUrl = QStringLiteral("https://github.com/danielearrighi/kde-sticky-notes/releases");
const QString kTagsUrl = QStringLiteral("https://api.github.com/repos/danielearrighi/kde-sticky-notes/tags?per_page=100");
} // namespace

InfoTab::InfoTab(QWidget *parent)
    : QWidget(parent)
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
}

void InfoTab::checkForUpdates()
{
    if (!m_network)
        m_network = new QNetworkAccessManager(this);

    m_checkUpdates->setEnabled(false);
    m_checkUpdates->setText(I18n::t(QStringLiteral("info.checking_updates")));

    QNetworkRequest request{QUrl(kTagsUrl)};
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("StickyNotes/%1").arg(QCoreApplication::applicationVersion()));
    request.setRawHeader("Accept", "application/vnd.github+json");

    QNetworkReply *reply = m_network->get(request);
    // Do not leave the button disabled forever when GitHub is unreachable.
    QTimer::singleShot(15000, reply, [reply] { reply->abort(); });
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        handleUpdateReply(reply);
        reply->deleteLater();
        m_checkUpdates->setEnabled(true);
        m_checkUpdates->setText(I18n::t(QStringLiteral("info.check_updates")));
    });
}

void InfoTab::handleUpdateReply(QNetworkReply *reply)
{
    const auto fail = [this](const QString &reason) {
        QMessageBox::warning(this, I18n::t(QStringLiteral("info.update_check_failed_title")),
                             I18n::t(QStringLiteral("info.update_check_failed")).arg(reason));
    };

    if (reply->error() != QNetworkReply::NoError) {
        fail(reply->errorString());
        return;
    }

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status != 200) {
        fail(QStringLiteral("HTTP %1").arg(status));
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    if (!doc.isArray()) {
        fail(I18n::t(QStringLiteral("info.update_check_invalid_response")));
        return;
    }

    // Tags may be prefixed with "v": compare their semantic versions and keep
    // the greatest one, regardless of the order the API returned them in.
    QVersionNumber latestVersion;
    QString latestTag;
    for (const QJsonValue &value : doc.array()) {
        const QString tag = value.toObject().value(QStringLiteral("name")).toString();
        QString normalized = tag;
        if (normalized.startsWith(QLatin1Char('v'), Qt::CaseInsensitive))
            normalized.remove(0, 1);
        const QVersionNumber version = QVersionNumber::fromString(normalized);
        if (version.isNull())
            continue;
        if (latestVersion.isNull() || QVersionNumber::compare(version, latestVersion) > 0) {
            latestVersion = version;
            latestTag = tag;
        }
    }

    if (latestVersion.isNull()) {
        fail(I18n::t(QStringLiteral("info.update_check_invalid_response")));
        return;
    }

    const QString installed = QCoreApplication::applicationVersion();
    const QVersionNumber installedVersion = QVersionNumber::fromString(installed);
    if (QVersionNumber::compare(latestVersion, installedVersion) <= 0) {
        QMessageBox::information(this, I18n::t(QStringLiteral("info.up_to_date_title")),
                                 I18n::t(QStringLiteral("info.up_to_date")).arg(installed));
        return;
    }

    QMessageBox box(this);
    box.setIcon(QMessageBox::Information);
    box.setWindowTitle(I18n::t(QStringLiteral("info.update_available_title")));
    box.setText(I18n::t(QStringLiteral("info.update_available")).arg(latestTag, installed));
    QPushButton *download = box.addButton(I18n::t(QStringLiteral("info.download")),
                                          QMessageBox::AcceptRole);
    box.addButton(I18n::t(QStringLiteral("info.close")), QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() == download)
        QDesktopServices::openUrl(QUrl(kReleasesUrl));
}
