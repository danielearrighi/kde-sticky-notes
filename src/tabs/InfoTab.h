#pragma once

#include <QWidget>

/// "Info" tab: application, Qt/libraries and author information.
class InfoTab : public QWidget
{
    Q_OBJECT
public:
    explicit InfoTab(QWidget *parent = nullptr);
};
