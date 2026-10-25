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
    bool locked = false;      // position locked: the note cannot be dragged
    int scroll = 0;           // saved body scroll position (vertical pixels)
    int opacity = 100;        // window opacity, 10..100 (%)
    bool deleted = false;
    QString createdAt;
    QString updatedAt;
    QString deletedAt;
    // Key of the screen the note was last placed on ("<W>x<H>|<connector>",
    // built by screenKeyOf()), used to reopen it on the same monitor. Empty for
    // notes never placed.
    QString lastScreenKey;
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
