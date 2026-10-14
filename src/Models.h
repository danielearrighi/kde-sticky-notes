#pragma once

#include <QString>

struct Note
{
    qint64 id = -1;
    QString title;
    QString content;          // Markdown
    QString themeId = QStringLiteral("giallo");
    int x = 220;
    int y = 160;
    int w = 230;
    int h = 190;
    QString layer = QStringLiteral("top");   // "top" | "bottom"
    bool visible = true;
    bool collapsed = false;   // collapsed to the title bar only
    bool deleted = false;
    QString createdAt;
    QString updatedAt;
    QString deletedAt;
};

struct Theme
{
    QString id;
    QString name;
    QString body;        // note body color
    QString titleBar;    // title bar color
    QString text;        // text color
    int sort = 0;
    bool builtin = true;

    bool isValid() const { return !id.isEmpty(); }
};
