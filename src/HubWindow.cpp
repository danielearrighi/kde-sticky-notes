#include "HubWindow.h"

#include "I18n.h"
#include "NotesTab.h"
#include "SettingsTab.h"
#include "TrashTab.h"

#include <QCloseEvent>
#include <QTabWidget>

HubWindow::HubWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("Sticky Notes"));
    resize(880, 620);

    m_notes = new NotesTab;
    m_trash = new TrashTab;
    m_settings = new SettingsTab;

    m_tabs = new QTabWidget(this);
    m_tabs->addTab(m_notes, I18n::t(QStringLiteral("app.tab_notes")));
    m_tabs->addTab(m_trash, I18n::t(QStringLiteral("app.tab_trash")));
    m_tabs->addTab(m_settings, I18n::t(QStringLiteral("app.tab_settings")));
    setCentralWidget(m_tabs);

    // The hub exposes the same public interface as before (signals + refresh):
    // it forwards the tabs' ones, which stay independent from each other.
    connect(m_notes, &NotesTab::newNoteRequested, this, &HubWindow::newNoteRequested);
    connect(m_notes, &NotesTab::openNoteRequested, this, &HubWindow::openNoteRequested);
    connect(m_notes, &NotesTab::noteVisibilityToggled, this, &HubWindow::noteVisibilityToggled);
    connect(m_notes, &NotesTab::deleteNoteRequested, this, &HubWindow::deleteNoteRequested);
    connect(m_notes, &NotesTab::quitRequested, this, &HubWindow::quitRequested);

    connect(m_trash, &TrashTab::restoreNoteRequested, this, &HubWindow::restoreNoteRequested);
    connect(m_trash, &TrashTab::purgeNoteRequested, this, &HubWindow::purgeNoteRequested);
    connect(m_trash, &TrashTab::purgeAllRequested, this, &HubWindow::purgeAllRequested);

    connect(m_settings, &SettingsTab::settingsChanged, this, &HubWindow::settingsChanged);
    // The "Quit app" button turns into "Close to tray" while that option is on.
    connect(m_settings, &SettingsTab::settingsChanged, m_notes, &NotesTab::updateQuitButton);
    // The trash dates use the date/time format: refresh them right away.
    connect(m_settings, &SettingsTab::dateFormatChanged, m_trash, &TrashTab::refresh);

    refresh();
}

void HubWindow::closeEvent(QCloseEvent *event)
{
    // The hub is the only "normal" window. AppController decides what closing
    // means: quit the app, or (with "close to tray") just hide it, since the
    // tray icon then provides a way back to the hub and a real quit.
    event->accept();
    emit quitRequested();
}

void HubWindow::refresh()
{
    m_notes->refresh();
    m_trash->refresh();
    m_settings->refresh();
}
