#include "TrashTab.h"

#include "I18n.h"
#include "Storage.h"
#include "UiHelpers.h"

#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

TrashTab::TrashTab(QWidget *parent)
    : QWidget(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    auto *head = new QHBoxLayout;
    auto *heading = new QLabel(I18n::t(QStringLiteral("trash.heading")));
    {
        QFont headingFont = heading->font();
        headingFont.setBold(true);
        heading->setFont(headingFont);
    }
    head->addWidget(heading);
    head->addStretch();
    m_restore = UiHelpers::makeCardButton(I18n::t(QStringLiteral("restore")));
    m_purge = UiHelpers::makeCardButton(I18n::t(QStringLiteral("trash.delete_permanently")));
    m_purgeAll = UiHelpers::makeCardButton(I18n::t(QStringLiteral("trash.empty")));
    head->addWidget(m_restore);
    head->addWidget(m_purge);
    head->addWidget(m_purgeAll);
    root->addLayout(head);

    m_table = new QTableWidget;
    m_table->setColumnCount(4);
    m_table->setHorizontalHeaderLabels({I18n::t(QStringLiteral("trash.col_title")),
                                        I18n::t(QStringLiteral("trash.col_content")),
                                        I18n::t(QStringLiteral("trash.col_created")),
                                        I18n::t(QStringLiteral("trash.col_deleted"))});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->setShowGrid(false);
    m_table->setWordWrap(false);
    m_table->verticalHeader()->setVisible(false);

    QHeaderView *header = m_table->horizontalHeader();
    header->setSectionsMovable(false);
    header->setSectionResizeMode(0, QHeaderView::Interactive);
    header->setSectionResizeMode(1, QHeaderView::Stretch);
    header->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_table->setColumnWidth(0, 200);

    m_empty = new QLabel(I18n::t(QStringLiteral("trash.empty_message")));
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setEnabled(false);

    root->addWidget(m_empty);
    root->addWidget(m_table, 1);

    connect(m_restore, &QPushButton::clicked, this, [this] {
        const qint64 id = selectedId();
        if (id >= 0)
            emit restoreNoteRequested(id);
    });
    connect(m_purge, &QPushButton::clicked, this, [this] {
        const qint64 id = selectedId();
        if (id >= 0)
            emit purgeNoteRequested(id);
    });
    connect(m_purgeAll, &QPushButton::clicked, this, [this] {
        if (Storage::instance().deletedCount() == 0)
            return;
        if (QMessageBox::question(this, I18n::t(QStringLiteral("trash.empty")),
                                  I18n::t(QStringLiteral("trash.confirm_empty")))
            == QMessageBox::Yes)
            emit purgeAllRequested();
    });
}

qint64 TrashTab::selectedId() const
{
    const int row = m_table->currentRow();
    if (row < 0)
        return -1;
    QTableWidgetItem *it = m_table->item(row, 0);
    return it ? it->data(Qt::UserRole).toLongLong() : -1;
}

void TrashTab::refresh()
{
    QList<Note> deleted;
    for (const Note &n : Storage::instance().notes(true))
        if (n.deleted)
            deleted << n;

    m_table->setRowCount(deleted.size());
    for (int row = 0; row < deleted.size(); ++row) {
        const Note &n = deleted.at(row);

        const QString title = n.title.trimmed().isEmpty() ? UiHelpers::untitled()
                                                          : n.title;
        auto *titleItem = new QTableWidgetItem(title);
        titleItem->setData(Qt::UserRole, n.id);
        titleItem->setToolTip(title);
        m_table->setItem(row, 0, titleItem);

        auto *contentItem = new QTableWidgetItem(UiHelpers::plainSnippet(n.content, 120));
        contentItem->setToolTip(n.content);
        m_table->setItem(row, 1, contentItem);

        m_table->setItem(row, 2, new QTableWidgetItem(UiHelpers::formatWhen(n.createdAt)));
        m_table->setItem(row, 3, new QTableWidgetItem(UiHelpers::formatWhen(n.deletedAt)));
    }

    const int count = deleted.size();
    m_empty->setVisible(count == 0);
    m_table->setVisible(count > 0);
    m_restore->setEnabled(count > 0);
    m_purge->setEnabled(count > 0);
    m_purgeAll->setEnabled(count > 0);
}
