#include "InfoTab.h"

#include "I18n.h"
#include "Storage.h"

#include <QFont>
#include <QHeaderView>
#include <QLabel>
#include <QList>
#include <QPair>
#include <QSysInfo>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <qglobal.h> // qVersion()

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
        {I18n::t(QStringLiteral("info.author_label")), I18n::t(QStringLiteral("info.author"))},
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
        grid->setItem(row, 1, valueItem);
    }

    root->addWidget(grid);
    root->addStretch();
}
