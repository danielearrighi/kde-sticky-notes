#include "NotesTab.h"

#include "I18n.h"
#include "Storage.h"
#include "Themes.h"
#include "UiHelpers.h"

#include <QApplication>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QVBoxLayout>

NotesTab::NotesTab(QWidget *parent)
    : QWidget(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    auto *head = new QHBoxLayout;
    auto *title = new QLabel(I18n::t(QStringLiteral("notes.your_notes")));
    {
        QFont titleFont = title->font();
        titleFont.setBold(true);
        title->setFont(titleFont);
    }
    auto *add = UiHelpers::makeCardButton(QStringLiteral("+  ")
                                          + I18n::t(QStringLiteral("new_note")));
    connect(add, &QPushButton::clicked, this, &NotesTab::newNoteRequested);
    m_quit = UiHelpers::makeCardButton(I18n::t(QStringLiteral("app.quit")));
    connect(m_quit, &QPushButton::clicked, this, &NotesTab::quitRequested);
    head->addWidget(title);
    head->addStretch();
    head->addWidget(add);
    head->addWidget(m_quit);
    root->addLayout(head);
    updateQuitButton();

    auto *hint = new QLabel(I18n::t(QStringLiteral("notes.hint")));
    hint->setWordWrap(true);
    hint->setEnabled(false);
    root->addWidget(hint);

    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto *container = new QWidget;
    m_grid = new QGridLayout(container);
    m_grid->setContentsMargins(0, 0, 0, 0);
    m_grid->setSpacing(12);
    m_grid->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    scroll->setWidget(container);

    m_empty = new QLabel(I18n::t(QStringLiteral("notes.empty")));
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setEnabled(false);

    root->addWidget(m_empty);
    root->addWidget(scroll, 1);
}

void NotesTab::refresh()
{
    updateQuitButton();

    // clear
    while (QLayoutItem *item = m_grid->takeAt(0)) {
        if (QWidget *w = item->widget())
            w->deleteLater();
        delete item;
    }

    const QList<Note> notes = Storage::instance().notes(false);
    m_empty->setVisible(notes.isEmpty());

    const int columns = 3;
    int row = 0, col = 0;
    for (const Note &n : notes) {
        const Theme t = Storage::instance().theme(n.themeId);
        auto *card = new QFrame;
        card->setFixedSize(260, 150);
        card->setStyleSheet(QStringLiteral(
            "QFrame { background-color: %1; border-radius: 4px; }"
            "QLabel { color: %2; background: transparent; border: none; }")
            .arg(t.body, t.text));

        auto *v = new QVBoxLayout(card);
        v->setContentsMargins(10, 8, 10, 8);
        v->setSpacing(4);

        auto *tt = new QLabel(QStringLiteral("<b>%1</b>")
                                  .arg(n.title.trimmed().isEmpty()
                                           ? UiHelpers::untitled()
                                           : n.title.toHtmlEscaped()));
        tt->setTextFormat(Qt::RichText);
        auto *sn = new QLabel(UiHelpers::plainSnippet(n.content));
        sn->setWordWrap(true);
        sn->setAlignment(Qt::AlignTop | Qt::AlignLeft);

        auto *buttons = new QHBoxLayout;
        auto *vis = UiHelpers::makeCardButton(n.visible ? I18n::t(QStringLiteral("hide"))
                                                        : I18n::t(QStringLiteral("show")));
        // Delete is icon-only: the system trash icon (Breeze), with the label as
        // tooltip; falls back to the text when the style has no icon.
        auto *del = UiHelpers::makeCardButton(QString());
        const QIcon trash = QApplication::style()->standardIcon(QStyle::SP_TrashIcon);
        if (trash.isNull()) {
            del->setText(I18n::t(QStringLiteral("delete")));
        } else {
            del->setIcon(trash);
            del->setToolTip(I18n::t(QStringLiteral("delete")));
        }
        buttons->addStretch();
        buttons->addWidget(vis);
        buttons->addWidget(del);

        const qint64 id = n.id;
        const bool visible = n.visible;
        connect(vis, &QPushButton::clicked, this, [this, id, visible] {
            emit noteVisibilityToggled(id, !visible);
        });
        connect(del, &QPushButton::clicked, this, [this, id] { emit deleteNoteRequested(id); });

        v->addWidget(tt);
        v->addWidget(sn, 1);
        v->addLayout(buttons);

        m_grid->addWidget(card, row, col);
        if (++col >= columns) {
            col = 0;
            ++row;
        }
    }
}

void NotesTab::updateQuitButton()
{
    // The button only hides the hub when "close to system tray" is on (the tray
    // menu offers the real quit), so reflect that in its label.
    const bool closeToTray =
        Storage::instance().setting(QStringLiteral("close_to_tray"), QStringLiteral("0")) == QLatin1String("1");
    m_quit->setText(closeToTray ? I18n::t(QStringLiteral("app.close_to_tray"))
                                : I18n::t(QStringLiteral("app.quit")));
}
