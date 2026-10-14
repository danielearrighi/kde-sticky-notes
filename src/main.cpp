#include <QApplication>
#include <QLocalSocket>
#include <QMessageBox>
#include <QScreen>
#include <QIcon>
#include <QDebug>

#include "AppController.h"

namespace {
const char *kServerName = "sticky-notes-single-instance";
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("StickyNotes"));
    QApplication::setApplicationDisplayName(QStringLiteral("Sticky Notes"));
    QApplication::setDesktopFileName(QStringLiteral("sticky-notes"));
    QApplication::setQuitOnLastWindowClosed(false);
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/sticky-notes.png")));

    qWarning() << "[app] QPA platform:" << QGuiApplication::platformName()
               << "screen:" << QGuiApplication::primaryScreen()->geometry();

    // Single instance: if an instance is already running, ask it to show the hub and exit.
    {
        QLocalSocket probe;
        probe.connectToServer(QString::fromLatin1(kServerName));
        if (probe.waitForConnected(300)) {
            probe.write("show");
            probe.flush();
            probe.waitForBytesWritten(300);
            return 0;
        }
    }

    AppController controller;
    QString error;
    if (!controller.init(&error)) {
        QMessageBox::critical(nullptr, QStringLiteral("Sticky Notes"),
                              QStringLiteral("Impossibile aprire il database:\n%1").arg(error));
        return 1;
    }

    QString serverError;
    if (!controller.listenForSecondInstance(&serverError))
        qWarning() << "[app] impossibile avviare l'istanza singola:" << serverError;

    controller.start();
    return app.exec();
}
