#include "NotesTab.h"

#include "Storage.h"
#include "Themes.h"
#include "UiHelpers.h"

#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

NotesTab::NotesTab(QWidget *parent)
    : QWidget(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    auto *head = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("<b>Le tue note</b>"));
    auto *add = UiHelpers::makeCardButton(QStringLiteral("+  Nuova nota"));
    connect(add, &QPushButton::clicked, this, &NotesTab::newNoteRequested);
    auto *quit = UiHelpers::makeCardButton(QStringLiteral("Esci dall'app"));
    connect(quit, &QPushButton::clicked, this, &NotesTab::quitRequested);
    head->addWidget(title);
    head->addStretch();
    head->addWidget(add);
    head->addWidget(quit);
    root->addLayout(head);

    auto *hint = new QLabel(QStringLiteral(
        "Le note restano sul desktop e non compaiono nella barra delle applicazioni. "
        "Trascinale dalla barra del titolo; tasto destro per formattazione e colore."));
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

    m_empty = new QLabel(QStringLiteral("Nessuna nota. Creane una con \u201c+ Nuova nota\u201d."));
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setEnabled(false);

    root->addWidget(m_empty);
    root->addWidget(scroll, 1);
}

void NotesTab::refresh()
{
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
                                           ? QStringLiteral("(senza titolo)")
                                           : n.title.toHtmlEscaped()));
        tt->setTextFormat(Qt::RichText);
        auto *sn = new QLabel(UiHelpers::plainSnippet(n.content));
        sn->setWordWrap(true);
        sn->setAlignment(Qt::AlignTop | Qt::AlignLeft);

        auto *buttons = new QHBoxLayout;
        auto *open = UiHelpers::makeCardButton(QStringLiteral("Apri"));
        auto *vis = UiHelpers::makeCardButton(n.visible ? QStringLiteral("Nascondi")
                                                        : QStringLiteral("Mostra"));
        auto *del = UiHelpers::makeCardButton(QStringLiteral("Elimina"));
        buttons->addWidget(open);
        buttons->addWidget(vis);
        buttons->addStretch();
        buttons->addWidget(del);

        const qint64 id = n.id;
        const bool visible = n.visible;
        connect(open, &QPushButton::clicked, this, [this, id] { emit openNoteRequested(id); });
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
