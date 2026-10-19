#pragma once

#include <QWidget>

class QNetworkAccessManager;
class QNetworkReply;
class QPushButton;

/// "Info" tab: application, Qt/libraries and author information.
class InfoTab : public QWidget
{
    Q_OBJECT
public:
    explicit InfoTab(QWidget *parent = nullptr);

private:
    void checkForUpdates();
    void handleUpdateReply(QNetworkReply *reply);

    QPushButton *m_checkUpdates = nullptr;
    QNetworkAccessManager *m_network = nullptr;
};
