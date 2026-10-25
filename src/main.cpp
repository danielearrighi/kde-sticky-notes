#include <QApplication>
#include <QLocalSocket>
#include <QMessageBox>
#include <QScreen>
#include <QIcon>
#include <QDebug>
#include <QTextStream>

#include "AppController.h"
#include "I18n.h"
#include "Storage.h"
#include "Version.h"

namespace {
const char *kServerName = "sticky-notes-single-instance";
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("StickyNotes"));
    QApplication::setApplicationVersion(QStringLiteral(STICKY_NOTES_VERSION));
    QApplication::setApplicationDisplayName(QStringLiteral("Sticky Notes"));
    QApplication::setDesktopFileName(QStringLiteral("sticky-notes"));
    QApplication::setQuitOnLastWindowClosed(false);
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/sticky-notes.png")));

    // --version: print and exit, before the single-instance probe (used by
    // packaging/updater scripts).
    if (QCoreApplication::arguments().contains(QStringLiteral("--version"))) {
        QTextStream(stdout) << QCoreApplication::applicationName() << ' '
                            << QCoreApplication::applicationVersion() << Qt::endl;
        return 0;
    }

    qWarning() << "[app] QPA platform:" << QGuiApplication::platformName()
               << "screen:" << QGuiApplication::primaryScreen()->geometry();

    // Single instance: if an instance is already running, ask it to show the hub and exit.
    // A "--restart" launch skips the probe: it must always start, otherwise it
    // would forward "show" to the previous (dying) instance instead.
    // An "--autostart" launch (at login) only probes: the hub must not pop up,
    // and the running instance already has its notes on screen.
    const QStringList arguments = QCoreApplication::arguments();
    const bool restart = arguments.contains(QStringLiteral("--restart"));
    const bool autostart = arguments.contains(QStringLiteral("--autostart"));
    if (!restart) {
        QLocalSocket probe;
        probe.connectToServer(QString::fromLatin1(kServerName));
        if (probe.waitForConnected(300)) {
            if (!autostart) {
                probe.write("show");
                probe.flush();
                probe.waitForBytesWritten(300);
            }
            return 0;
        }
    }

    AppController controller;
    QString error;
    if (!controller.init(&error)) {
        QMessageBox::critical(nullptr, QStringLiteral("Sticky Notes"),
                              I18n::t(QStringLiteral("app.db_error")).arg(error));
        return 1;
    }

    I18n::applyLanguage(Storage::instance().setting(QStringLiteral("language"), QStringLiteral("en")));

    QString serverError;
    if (!controller.listenForSecondInstance(&serverError))
        qWarning() << "[app] cannot start the single-instance server:" << serverError;

    controller.start(autostart);
    return app.exec();
}
