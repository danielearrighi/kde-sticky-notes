#include "NoteWindow.h"
#include "I18n.h"
#include "Storage.h"
#include "Themes.h"
#include "UiHelpers.h"

#include <LayerShellQt/window.h>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QTextEdit>
#include <QScrollBar>
#include <QToolButton>
#include <QMenu>
#include <QTimer>
#include <QPainter>
#include <QPainterPath>
#include <QGraphicsOpacityEffect>
#include <QPixmap>
#include <QImage>
#include <QImageReader>
#include <QBuffer>
#include <QIcon>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QPalette>
#include <QRegularExpression>
#include <QScreen>
#include <QMimeData>
#include <QUrl>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QFocusEvent>
#include <QTextListFormat>
#include <QTextList>
#include <QTextBlock>
#include <QTextFragment>
#include <QContextMenuEvent>
#include <QTextCharFormat>
#include <QTextImageFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QDebug>

#include <functional>

namespace {

constexpr int kTitleHeight = 30;
constexpr int kResizeGrip = 16;
constexpr int kMinW = 160;
constexpr int kMinH = 120;
// Rounded corners of the note and the transparent border left around it to
// draw a small drop shadow. The layer-shell surface is kShadowMargin larger
// than the note on every side; applyMargins() compensates the offset so the
// note keeps its saved position.
constexpr int kCornerRadius = 4;
constexpr int kShadowMargin = 8;
// Inner padding of the note body. The same value is used for QTextDocument's
// margin (= top/left/right/bottom padding of the text) and for the extra
// bottom padding of the editor (see applyStyle()): without it, when the caret
// reaches the last line QTextEdit scrolls the document's bottom margin out of
// view and the text touches the bottom border.
constexpr int kBodyPadding = 10;
constexpr qreal kListIndentWidth = 25; //Default QTextListFormat indent.
// Longest side of a pasted image, in device pixels. The image lives inside the
// note content as a base64 data URL (so it travels with the backups), hence it
// is shrunk to something reasonable instead of being stored verbatim.
constexpr int kMaxImageSide = 900;

// ---- screen identity -------------------------------------------------------

/// Key that identifies a monitor: its resolution plus the connector name, e.g.
/// "2560x1440|DP-3". The resolution alone would collide between two identical
/// monitors; the connector distinguishes them (stable as long as the monitor
/// stays on the same port).
QString screenKeyOf(const QScreen *s)
{
    if (!s)
        return QString();
    const QSize sz = s->geometry().size();
    QString key = QString::number(sz.width()) + QLatin1Char('x') + QString::number(sz.height());
    if (!s->name().isEmpty())
        key += QLatin1Char('|') + s->name();
    return key;
}

/// Connected screens with a real geometry (skips the transient QtWayland
/// placeholder screen created while the outputs are reconfigured).
QList<QScreen *> realScreens()
{
    QList<QScreen *> out;
    for (QScreen *s : QGuiApplication::screens())
        if (!s->geometry().isEmpty() && !s->availableGeometry().isEmpty())
            out.append(s);
    return out;
}

QScreen *screenForKey(const QString &key, const QList<QScreen *> &screens)
{
    if (key.isEmpty())
        return nullptr;
    for (QScreen *s : screens)
        if (screenKeyOf(s) == key)
            return s;
    return nullptr;
}

QIcon colorIcon(const QString &hex)
{
    QPixmap pm(14, 14);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(QColor(hex));
    p.setPen(QPen(QColor("#00000055")));
    p.drawRoundedRect(QRectF(1, 1, 12, 12), 3, 3);
    p.end();
    return QIcon(pm);
}

QString themeColor(const QString &id, const QString &fallback)
{
    const Theme t = Storage::instance().theme(id);
    return t.isValid() ? t.body : fallback;
}

/// Pixel size of the image referenced by \a name, read from the image header
/// only (no full decode). QTextDocument already knows how to fetch the payload
/// of a "data:" URL (and of a local file), so its resource is asked first: a
/// null size means "unknown", and the caller then leaves the image natural.
QSize imagePixelSize(QTextDocument *doc, const QString &name)
{
    const QVariant data = doc->resource(QTextDocument::ImageResource, QUrl(name));
    if (data.userType() == QMetaType::QImage)
        return data.value<QImage>().size();
    if (data.userType() == QMetaType::QPixmap)
        return data.value<QPixmap>().toImage().size();

    QByteArray raw = data.toByteArray();
    if (raw.isEmpty())
        return QSize();
    QBuffer buffer(&raw);
    buffer.open(QIODevice::ReadOnly);
    return QImageReader(&buffer).size();
}

/// QTextEdit that turns a pasted/dropped image into a note image: the handler
/// installed by NoteWindow stores it inside the document (see
/// NoteWindow::insertImage()). It also recognizes URLs typed or pasted in the
/// body (see linkifyRange()) and opens anchors with Ctrl+Click. Everything else
/// keeps the QTextEdit behaviour (plain text, HTML, file links).
///
/// Ctrl+V, Shift+Insert, the "Paste" entry of the context menu and drag & drop
/// all funnel through insertFromMimeData(): QTextEdit::paste() goes to an
/// internal QWidgetTextControl that calls the widget's virtual override.
class NoteBodyEdit : public QTextEdit
{
public:
    explicit NoteBodyEdit(QWidget *parent = nullptr) : QTextEdit(parent)
    {
        connect(document(), &QTextDocument::contentsChange, this,
                [this](int position, int charsRemoved, int charsAdded) {
                    handleContentsChange(position, charsRemoved, charsAdded);
                });
    }

    void setImageHandler(std::function<bool(const QImage &)> handler)
    {
        m_imageHandler = std::move(handler);
    }

    void setPlainPasteHandler(std::function<void()> handler)
    {
        m_plainPasteHandler = std::move(handler);
    }

    /// Enables the automatic recognition of URLs typed or pasted in the body.
    /// It is off while the note is loaded from Markdown: QTextDocument already
    /// creates the anchors there, and modifying the document during the import
    /// would interfere with it.
    void setLinkifyEnabled(bool enabled) { m_linkifyEnabled = enabled; }

protected:
    void keyPressEvent(QKeyEvent *event) override
    {
        // Ctrl+Shift+V: paste without formatting. Handled here (and not as a
        // QAction shortcut) so it works whenever the body has focus, including
        // while the context menu is closed.
        const Qt::KeyboardModifiers mods = event->modifiers() & ~Qt::KeypadModifier;
        if (event->key() == Qt::Key_V
            && mods == (Qt::ControlModifier | Qt::ShiftModifier)
            && m_plainPasteHandler) {
            m_plainPasteHandler();
            event->accept();
            return;
        }
        QTextEdit::keyPressEvent(event);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        // Ctrl+Click on a link opens it in the browser instead of moving the
        // text cursor. AnchorAt() also works for the links created by the
        // Markdown import and by linkifyRange().
        if (event->button() == Qt::LeftButton
            && event->modifiers().testFlag(Qt::ControlModifier)) {
            const QString href = anchorAt(event->position().toPoint());
            if (!href.isEmpty() && openLink(href)) {
                m_linkPressed = true;
                event->accept();
                return;
            }
        }
        QTextEdit::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (m_linkPressed) {
            m_linkPressed = false;
            event->accept();
            return;
        }
        QTextEdit::mouseReleaseEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        // The pointing hand while Ctrl is held tells that the link can be
        // opened; without Ctrl the text cursor behaviour is left untouched.
        if (!m_linkPressed) {
            const bool overLink = event->modifiers().testFlag(Qt::ControlModifier)
                && !anchorAt(event->position().toPoint()).isEmpty();
            viewport()->setCursor(overLink ? Qt::PointingHandCursor : Qt::IBeamCursor);
        }
        QTextEdit::mouseMoveEvent(event);
    }

    bool canInsertFromMimeData(const QMimeData *source) const override
    {
        if (source && (source->hasImage() || source->hasUrls()))
            return true;
        return QTextEdit::canInsertFromMimeData(source);
    }

    void insertFromMimeData(const QMimeData *source) override
    {
        if (source && m_imageHandler) {
            // An image on the clipboard (a screenshot, or a copy from any
            // application).
            const QImage clipboardImage = imageFromMimeData(source);
            if (!clipboardImage.isNull() && m_imageHandler(clipboardImage))
                return;

            // Otherwise: image files (a copy or a drag from the file manager).
            bool inserted = false;
            for (const QUrl &url : source->urls()) {
                if (!url.isLocalFile())
                    continue;
                const QImage fileImage(url.toLocalFile());
                if (!fileImage.isNull() && m_imageHandler(fileImage))
                    inserted = true;
            }
            if (inserted)
                return;
        }
        QTextEdit::insertFromMimeData(source);
    }

private:
    /// The URL pattern accepted in the note body. Markdown links ([text](url))
    /// are already turned into anchors by QTextDocument::setMarkdown(); this is
    /// for URLs typed or pasted while editing.
    static const QRegularExpression &linkPattern()
    {
        static const QRegularExpression pattern(
            QStringLiteral(R"((?:https?://|www\.)[^\s<>"'`]+)"),
            QRegularExpression::CaseInsensitiveOption);
        return pattern;
    }

    /// End of the link match after dropping the punctuation that usually
    /// follows a URL in prose (".", ",", an unbalanced closing bracket, ...).
    static int linkEnd(const QString &text, int start, int end)
    {
        while (end > start) {
            const QChar ch = text.at(end - 1);
            if (QStringLiteral(".,;:!?'\"").contains(ch)) {
                --end;
                continue;
            }
            if (ch == QLatin1Char(')') || ch == QLatin1Char(']') || ch == QLatin1Char('}')) {
                const QChar opening = ch == QLatin1Char(')') ? QLatin1Char('(')
                    : ch == QLatin1Char(']') ? QLatin1Char('[') : QLatin1Char('{');
                const QString range = text.mid(start, end - start);
                if (range.count(ch) > range.count(opening)) {
                    --end;
                    continue;
                }
            }
            break;
        }
        return end;
    }

    /// True when [start, end) of \a text is the target of a Markdown link
    /// ([label](url) or <url>): those are left to the Markdown importer,
    /// otherwise the anchor added here would corrupt the link syntax.
    static bool isMarkdownTarget(const QString &text, int start, int end)
    {
        if (start >= 2 && text.at(start - 1) == QLatin1Char('(')
            && text.at(start - 2) == QLatin1Char(']'))
            return true;
        return start >= 1 && text.at(start - 1) == QLatin1Char('<')
            && end < text.size() && text.at(end) == QLatin1Char('>');
    }

    static bool openLink(const QString &href)
    {
        const QUrl url = QUrl::fromUserInput(href);
        if (!url.isValid() || url.isEmpty())
            return false;
        const QString scheme = url.scheme().toLower();
        if (scheme != QLatin1String("http") && scheme != QLatin1String("https")
            && scheme != QLatin1String("mailto"))
            return false;
        return QDesktopServices::openUrl(url);
    }

    /// Turns every naked URL in [from, to) into an anchor (keeping the text and
    /// its existing character format) so it can be opened with Ctrl+Click.
    void linkifyRange(int from, int to)
    {
        if (!m_linkifyEnabled || from < 0)
            return;
        to = qMin(to, document()->characterCount() - 1);
        if (from >= to)
            return;

        QTextCursor scan(document());
        scan.setPosition(from);
        scan.setPosition(to, QTextCursor::KeepAnchor);
        const QString text = scan.selectedText();
        if (text.isEmpty())
            return;

        QList<QPair<int, int>> ranges;   // (position, length) in the document
        QRegularExpressionMatchIterator matches = linkPattern().globalMatch(text);
        while (matches.hasNext()) {
            const QRegularExpressionMatch match = matches.next();
            const int start = match.capturedStart();
            const int end = linkEnd(text, start, match.capturedEnd());
            if (end > start && !isMarkdownTarget(text, start, end))
                ranges.append({from + start, end - start});
        }
        if (ranges.isEmpty())
            return;

        // The format the cursor had before the link: restored below when the
        // cursor ends up at the end of a freshly linked URL, so the next typed
        // character does not inherit the link colour/underline.
        const QTextCharFormat before = textCursor().charFormat();

        m_linkifying = true;
        QTextCursor c(document());
        c.beginEditBlock();
        for (const QPair<int, int> &range : ranges) {
            c.setPosition(range.first);
            c.setPosition(range.first + range.second, QTextCursor::KeepAnchor);
            QTextCharFormat fmt = c.charFormat();
            if (fmt.isAnchor())
                continue;   // already a link: keep its href and formatting
            QString href = text.mid(range.first - from, range.second);
            if (href.startsWith(QLatin1String("www."), Qt::CaseInsensitive))
                href.prepend(QStringLiteral("https://"));
            fmt.setAnchor(true);
            fmt.setAnchorHref(href);
            fmt.setFontUnderline(true);
            fmt.setForeground(palette().color(QPalette::Link));
            c.mergeCharFormat(fmt);
        }
        c.endEditBlock();

        if (textCursor().charFormat().isAnchor())
            setCurrentCharFormat(before);

        m_linkifying = false;
    }

    /// Linkifies a word as soon as it is terminated by a space/Enter, or the
    /// text just pasted/inserted. Formatting-only changes (bold, clear
    /// formatting, ...) are ignored: they report the same removed/added length.
    void handleContentsChange(int position, int charsRemoved, int charsAdded)
    {
        if (!m_linkifyEnabled || m_linkifying || charsAdded <= 0
            || charsRemoved == charsAdded)
            return;

        if (charsRemoved > 0 || charsAdded > 1) {
            linkifyRange(position, position + charsAdded);
            return;
        }

        QTextCursor inserted(document());
        inserted.setPosition(position);
        inserted.setPosition(position + charsAdded, QTextCursor::KeepAnchor);
        const QString added = inserted.selectedText();
        if (added.size() != 1 || !added.at(0).isSpace())
            return;   // still typing the URL: wait for its end

        int end = position;
        while (end > 0 && document()->characterAt(end - 1).isSpace())
            --end;
        int start = end;
        while (start > 0 && !document()->characterAt(start - 1).isSpace())
            --start;
        linkifyRange(start, end);
    }

    /// The image carried by the MIME data as image data (a screenshot, a copy
    /// from another application), or a null image.
    static QImage imageFromMimeData(const QMimeData *source)
    {
        if (!source || !source->hasImage())
            return QImage();
        const QVariant data = source->imageData();
        if (data.userType() == QMetaType::QImage)
            return data.value<QImage>();
        if (data.userType() == QMetaType::QPixmap)
            return data.value<QPixmap>().toImage();
        return qvariant_cast<QImage>(data);
    }

    std::function<bool(const QImage &)> m_imageHandler;
    std::function<void()> m_plainPasteHandler;
    bool m_linkifyEnabled = false;
    bool m_linkifying = false;
    bool m_linkPressed = false;
};

/// Container of the note card: it paints the rounded background and the title
/// strip (see NoteWindow::paintCard()) and is the widget the opacity effect is
/// applied to, so background, text and images are faded together.
class NoteCard : public QWidget
{
public:
    explicit NoteCard(QWidget *parent = nullptr) : QWidget(parent) {}

    void setPaintHandler(std::function<void(QPainter &)> handler)
    {
        m_paintHandler = std::move(handler);
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        if (!m_paintHandler) {
            QWidget::paintEvent(event);
            return;
        }
        QPainter p(this);
        m_paintHandler(p);
    }

private:
    std::function<void(QPainter &)> m_paintHandler;
};

} // namespace

NoteWindow::NoteWindow(const Note &note, QWidget *parent)
    : QWidget(parent)
    , m_note(note)
{
    setWindowTitle(note.title.isEmpty() ? I18n::t(QStringLiteral("note.default_title"))
                                        : note.title);
    setAttribute(Qt::WA_StyledBackground, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setObjectName(QStringLiteral("noteWindow"));
    setFocusPolicy(Qt::StrongFocus);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(kShadowMargin, kShadowMargin, kShadowMargin, kShadowMargin);
    root->setSpacing(0);

    // The card holds the title bar and the body and paints their background;
    // the opacity effect is applied to it as a whole (see applyOpacity()).
    auto *card = new NoteCard(this);
    m_card = card;
    card->setPaintHandler([this](QPainter &p) { paintCard(p); });
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(0);

    // ---- title bar ----
    m_titleBar = new QWidget(card);
    m_titleBar->setObjectName(QStringLiteral("titleBar"));
    m_titleBar->setFixedHeight(kTitleHeight);
    m_titleBar->setAttribute(Qt::WA_StyledBackground, true);

    auto *bar = new QHBoxLayout(m_titleBar);
    bar->setContentsMargins(6, 0, 2, 0);
    bar->setSpacing(4);

    m_titleLabel = new QLabel(m_titleBar);
    m_titleLabel->setTextInteractionFlags(Qt::NoTextInteraction);
    m_titleLabel->setObjectName(QStringLiteral("titleText"));

    m_titleEdit = new QLineEdit(m_titleBar);
    m_titleEdit->setObjectName(QStringLiteral("titleEdit"));
    m_titleEdit->setFrame(false);
    m_titleEdit->hide();

    // "collapse" button: shows only the title bar
    m_collapseButton = new QToolButton(m_titleBar);
    m_collapseButton->setObjectName(QStringLiteral("collapseButton"));
    m_collapseButton->setText(QStringLiteral("_"));
    m_collapseButton->setAutoRaise(true);
    m_collapseButton->setFocusPolicy(Qt::NoFocus);
    m_collapseButton->setFixedSize(20, 20);
    m_collapseButton->setCursor(Qt::PointingHandCursor);
    m_collapseButton->setToolTip(I18n::t(QStringLiteral("note.minimize")));

    // delete button: moves the note to the trash
    m_closeButton = new QToolButton(m_titleBar);
    m_closeButton->setObjectName(QStringLiteral("closeButton"));
    m_closeButton->setText(QStringLiteral("\u2715"));   // ✕
    m_closeButton->setAutoRaise(true);
    m_closeButton->setFocusPolicy(Qt::NoFocus);
    m_closeButton->setFixedSize(20, 20);
    m_closeButton->setCursor(Qt::PointingHandCursor);
    m_closeButton->setToolTip(I18n::t(QStringLiteral("note.delete")));

    bar->addWidget(m_titleLabel, 1);
    bar->addWidget(m_titleEdit, 1);
    bar->addWidget(m_collapseButton);
    bar->addWidget(m_closeButton);

    // ---- body ----
    auto *noteBody = new NoteBodyEdit(card);
    m_body = noteBody;
    noteBody->setImageHandler([this](const QImage &image) { return insertImage(image); });
    noteBody->setPlainPasteHandler([this] { pastePlainText(); });
    m_body->setObjectName(QStringLiteral("body"));
    m_body->setFrameShape(QFrame::NoFrame);
    m_body->setAcceptRichText(true);
    m_body->setContextMenuPolicy(Qt::CustomContextMenu);
    m_body->document()->setDocumentMargin(kBodyPadding);
    m_body->document()->setIndentWidth(kListIndentWidth);

    cardLayout->addWidget(m_titleBar);
    cardLayout->addWidget(m_body, 1);
    root->addWidget(card);

    // ---- resize grip (bottom right) ----
    m_resizeGrip = new QLabel(QStringLiteral("\u25e2"), card);   // ◢
    m_resizeGrip->setObjectName(QStringLiteral("resizeGrip"));
    m_resizeGrip->setAlignment(Qt::AlignCenter);
    m_resizeGrip->setFixedSize(kResizeGrip, kResizeGrip);
    m_resizeGrip->setCursor(Qt::SizeFDiagCursor);
    m_resizeGrip->raise();

    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(700);

    // The layer of "desktop" notes (bottom) must be restored when focus is
    // really lost, but deactivating the surface is not always a definitive
    // loss: opening the context menu moves keyboard focus to the popup surface.
    // So we check a moment later who has focus (see NoteWindow::onFocusLayerTimeout()).
    m_focusLayerTimer = new QTimer(this);
    m_focusLayerTimer->setSingleShot(true);
    m_focusLayerTimer->setInterval(200);

    connect(m_body->document(), &QTextDocument::contentsChanged, this, [this] {
        if (!m_loading)
            scheduleSaveContent();
    });
    connect(m_body, &QTextEdit::customContextMenuRequested, this, [this](const QPoint &p) {
        buildContextMenu(m_body->mapToGlobal(p));
    });
    connect(m_titleEdit, &QLineEdit::editingFinished, this, &NoteWindow::commitTitleEdit);
    connect(m_collapseButton, &QToolButton::clicked, this, &NoteWindow::toggleCollapse);
    connect(m_closeButton, &QToolButton::clicked, this, [this] {
        emit deleteRequested(m_note.id);
    });
    connect(m_saveTimer, &QTimer::timeout, this, [this] {
        if (m_loading)
            return;
        m_note.content = m_body->document()->toMarkdown();
        emit contentEdited(m_note.id, m_note.title, m_note.content);
    });
    connect(m_focusLayerTimer, &QTimer::timeout, this, &NoteWindow::onFocusLayerTimeout);

    // content
    m_loading = true;
    m_body->document()->setMarkdown(m_note.content);
    applyImageSizing();   // Markdown carries no image size: refit to the note width
    m_body->document()->clearUndoRedoStacks();
    // From now on URLs typed or pasted are recognized; doing it during the
    // Markdown import would interfere with QTextDocument::setMarkdown().
    static_cast<NoteBodyEdit *>(m_body)->setLinkifyEnabled(true);
    m_loading = false;

    refreshTitleLabel();
    applyTheme(m_note.themeId);
    applyOpacity();

    // Saved collapsed state: restore the appearance (applySize() applies the height).
    m_collapsed = m_note.collapsed;
    applyCollapsedVisuals();

    // layer-shell
    createWinId();
    configureLayerShell();
    m_note.w = qMax(kMinW, m_note.w);
    m_note.h = qMax(kMinH, m_note.h);
    // First placement: restores the per-screen position saved for the note's
    // last screen (or maps it proportionally) and records the screen key.
    placeOnScreens(true, false);

    // Restore the saved scroll position. The document layout (and therefore
    // the scrollbar range) is only complete once the event loop runs, so the
    // value is applied after the current event returns (see restoreScroll()).
    // There is no incremental tracking while scrolling: the position is
    // persisted when the note is hidden or the app quits (see persistScroll()).
    restoreScroll();
}

NoteWindow::~NoteWindow()
{
    if (m_layer)
        m_layer->deleteLater();
}

void NoteWindow::configureLayerShell()
{
    QWindow *wh = windowHandle();
    if (!wh) {
        qWarning() << "[nota] windowHandle() nullo: nessuna superficie layer-shell";
        return;
    }
    m_layer = LayerShellQt::Window::get(wh);
    if (!m_layer) {
        qWarning() << "[nota] LayerShellQt::Window::get() nullo";
        return;
    }
    using LS = LayerShellQt::Window;
    m_layer->setScope(QStringLiteral("sticky-notes-note"));
    m_layer->setLayer(m_note.layer == QLatin1String("bottom") ? LS::LayerBottom : LS::LayerTop);
    m_layer->setKeyboardInteractivity(LS::KeyboardInteractivityOnDemand);
    m_layer->setExclusiveZone(0);
    m_layer->setActivateOnShow(true);
    m_layer->setAnchors(LS::Anchors::fromInt(int(LS::AnchorTop) | int(LS::AnchorLeft)));
}

void NoteWindow::applyMargins()
{
    if (m_layer) {
        // The surface is kShadowMargin larger than the note on every side (see
        // kShadowMargin): shift it so the note stays at (m_note.x, m_note.y).
        // m_note.x/y are relative to the screen's available area, while the
        // layer-shell margin is relative to the output edge: add the offset of
        // the available area inside the output (m_screenOrigin).
        m_layer->setMargins(QMargins(m_screenOrigin.x() + m_note.x - kShadowMargin,
                                     m_screenOrigin.y() + m_note.y - kShadowMargin, 0, 0));

        // The layer-shell state (margin included) is double-buffered: the
        // compositor applies it only on the next wl_surface commit.
        // LayerShellQt::setMargins() does NOT commit and during a drag the
        // widget does not redraw, so the window does not move until something
        // else (e.g. a click) triggers a redraw that commits it.
        //
        // We only dirty a corner of the transparent shadow border, not the
        // whole note: this is enough for QtWayland to render and commit the
        // surface. A full update() would repaint the body too, and with an
        // image inside it that repaint costs far more than a frame. The
        // surface would then lag several pointer events behind: the local
        // coordinate keeps growing from the press point and the fixed
        // reference in mouseMoveEvent() sums the delta again on every event,
        // making the note jump around (the same "explosion" as when nothing
        // commits at all). Moving does not change the note content, so a
        // minimal damage is enough — the compositor moves the existing buffer.
        update(QRect(0, 0, kShadowMargin, kShadowMargin));
    }
}

void NoteWindow::applySize()
{
    // Collapsed: the surface is as tall as the title bar alone. m_note.h stays
    // the "expanded" height and is never overwritten by the collapsed state.
    const int h = m_collapsed ? kTitleHeight : m_note.h;
    // The surface also carries the transparent border used for the shadow.
    const QSize surface(m_note.w + 2 * kShadowMargin, h + 2 * kShadowMargin);
    if (m_layer)
        m_layer->setDesiredSize(surface);
    resize(surface);
}

void NoteWindow::clampToScreen()
{
    placeOnScreens(false, false);
}

void NoteWindow::ensureOnScreen()
{
    placeOnScreens(true, !m_placed);
    m_placed = true;
}

void NoteWindow::placeOnScreens(bool proportional, bool forceEmit)
{
    // Ignore the transient placeholder screen QtWayland creates while the real
    // outputs are reconfigured (it has an empty geometry): mapping onto it would
    // send every note to (0,0).
    const QList<QScreen *> screens = realScreens();
    if (screens.isEmpty())
        return;
    QScreen *primary = QGuiApplication::primaryScreen();
    if (!primary || !screens.contains(primary))
        primary = screens.first();

    const int oldX = m_note.x, oldY = m_note.y, oldW = m_note.w, oldH = m_note.h;
    const QString oldKey = m_screenKey;
    const int h = m_collapsed ? kTitleHeight : m_note.h;
    const QSize noteSize(m_note.w, h);

    // Reference screen: the one the note is on (if still connected), otherwise
    // its last available geometry (kept in m_screenRect), otherwise - at startup
    // - the screen saved as the note's last screen. m_note.x/y are relative to
    // that screen's available area.
    QScreen *current = screenForKey(m_screenKey, screens);
    QRect refAvail;
    if (current)
        refAvail = current->availableGeometry();
    else if (m_screenRect.isValid())
        refAvail = m_screenRect;
    else if (QScreen *last = screenForKey(m_note.lastScreenKey, screens))
        refAvail = last->availableGeometry();
    const bool haveRef = refAvail.isValid();

    // Global rect the note occupies now.
    const QRect noteGlobal = haveRef
                                 ? QRect(refAvail.topLeft() + QPoint(m_note.x, m_note.y), noteSize)
                                 : QRect(QPoint(m_note.x, m_note.y), noteSize);

    // Target: the screen the note overlaps the most (fallback: the primary).
    QScreen *target = primary;
    int bestArea = 0;
    for (QScreen *s : screens) {
        const QRect inter = s->availableGeometry().intersected(noteGlobal);
        const int area = inter.width() * inter.height();
        if (area > bestArea) {
            bestArea = area;
            target = s;
        }
    }

    const QString targetKey = screenKeyOf(target);
    const QRect targetAvail = target->availableGeometry();
    const QRect targetRel(QPoint(0, 0), targetAvail.size());   // available area, relative

    // On a screen change, restore the exact position saved for that screen; if
    // there is none, map proportionally from the reference screen. The mapping is
    // normalised over the *usable* range (screen minus the note size) so that
    // moving away and back returns to the original position instead of drifting.
    if (proportional && (!current || targetKey != m_screenKey)) {
        const QRect stored = Storage::instance().notePosition(m_note.id, targetKey);
        if (stored.isValid()) {
            m_note.x = stored.x();
            m_note.y = stored.y();
            m_note.w = qMax(kMinW, stored.width());
            m_note.h = qMax(kMinH, stored.height());
        } else if (haveRef && refAvail != targetAvail) {
            const int spanX = refAvail.width() - m_note.w;
            const int spanY = refAvail.height() - h;
            const qreal fx = spanX > 0 ? qBound(0.0, qreal(m_note.x) / spanX, 1.0) : 0.0;
            const qreal fy = spanY > 0 ? qBound(0.0, qreal(m_note.y) / spanY, 1.0) : 0.0;
            m_note.x = targetRel.left() + qRound(fx * qMax(0, targetRel.width() - m_note.w));
            m_note.y = targetRel.top() + qRound(fy * qMax(0, targetRel.height() - h));
        }
    }

    const int hh = m_collapsed ? kTitleHeight : m_note.h;
    if (proportional) {
        // Guarantee the whole note is reachable again (its left/top part when
        // the note is wider/taller than the screen).
        m_note.x = qBound(targetRel.left(), m_note.x,
                          targetRel.left() + qMax(0, targetRel.width() - m_note.w));
        m_note.y = qBound(targetRel.top(), m_note.y,
                          targetRel.top() + qMax(0, targetRel.height() - hh));
    } else {
        const int keep = 60;   // how many pixels of the note always stay reachable
        m_note.x = qBound(targetRel.left() - m_note.w + keep, m_note.x, targetRel.right() - keep);
        m_note.y = qBound(targetRel.top(), m_note.y, targetRel.bottom() - keep);
    }

    m_screenKey = targetKey;
    m_screenRect = targetAvail;
    m_screenOrigin = targetAvail.topLeft() - target->geometry().topLeft();

    const bool sizeChanged = (m_note.w != oldW || m_note.h != oldH);
    const bool moved = (m_note.x != oldX || m_note.y != oldY || sizeChanged || targetKey != oldKey);

    // If the compositor dismissed the surface when its output went away, or the
    // QWindow still points at a screen that is no longer connected, re-map it on
    // a live screen (hide + show recreates the layer surface).
    QWindow *wh = windowHandle();
    const bool staleScreen = wh && wh->screen() && !screens.contains(wh->screen());
    const bool remap = proportional && m_note.visible && (!isVisible() || staleScreen);
    if (remap) {
        hide();
        show();
    }

    applySize();
    applyMargins();

    if (moved || forceEmit)
        emit geometryEdited(m_note.id, m_note.x, m_note.y, m_note.w, m_note.h, m_screenKey);
}

void NoteWindow::applyLayer(const QString &layer)
{
    m_note.layer = layer;
    applyEffectiveLayer();
}

void NoteWindow::applyEffectiveLayer()
{
    if (!m_layer)
        return;
    using LS = LayerShellQt::Window;

    // While the note has focus it stays in front of normal windows, even when
    // configured as a "desktop" note (bottom layer): without this, clicking a
    // desktop note would leave it below the window currently in use.
    const LS::Layer wanted = (m_raisedForFocus || m_note.layer != QLatin1String("bottom"))
                                 ? LS::LayerTop
                                 : LS::LayerBottom;
    if (m_layer->layer() == wanted)
        return;

    m_layer->setLayer(wanted);

    // The layer is double-buffered too: as with the margin in applyMargins(),
    // without a wl_surface commit the compositor does not apply it. update()
    // forces it (QtWayland translates it into a commit).
    update();
}

bool NoteWindow::event(QEvent *event)
{
    // Note: WindowActivate/WindowDeactivate do NOT go through changeEvent()
    // (which only receives ActivationChange), they must be caught here.
    switch (event->type()) {
    case QEvent::WindowActivate:
        // The surface has keyboard focus: the note goes to the front.
        m_focusLayerTimer->stop();
        m_raisedForFocus = true;
        applyEffectiveLayer();
        break;
    case QEvent::WindowDeactivate:
        // Focus may have moved to a note popup (context menu) or to another
        // window: wait and verify (onFocusLayerTimeout).
        m_focusLayerTimer->start();
        break;
    default:
        break;
    }
    return QWidget::event(event);
}

void NoteWindow::onFocusLayerTimeout()
{
    // A popup of this application (the note context menu) takes keyboard focus
    // instead of the surface: while it is open the note stays in front,
    // otherwise the menu would end up below the other windows.
    if (QApplication::activePopupWidget()) {
        m_focusLayerTimer->start();
        return;
    }

    if (isActiveWindow())
        return;     // focus returned to the note: it stays in front

    m_raisedForFocus = false;
    applyEffectiveLayer();
}

void NoteWindow::setVisibleState(bool visible)
{
    m_note.visible = visible;
    if (!visible) {
        // Hidden: no focus possible, return to the configured layer.
        m_focusLayerTimer->stop();
        m_raisedForFocus = false;
        applyEffectiveLayer();
        persistScroll();
        setVisible(false);
        return;
    }
    setVisible(true);
    // A note restored from the hub may still hold a position on a screen that is
    // no longer connected: bring it back before showing it.
    ensureOnScreen();
}

void NoteWindow::activate()
{
    ensureOnScreen();
    show();
    raise();
    if (m_layer)
        m_layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityOnDemand);
    // Asks the compositor for focus: this way a "desktop" note opened from the
    // hub is really brought to the front (and stays there while it has focus).
    if (QWindow *wh = windowHandle())
        wh->requestActivate();
    m_body->setFocus();
}

void NoteWindow::refreshTitleLabel()
{
    const QString t = m_note.title.trimmed();
    m_titleLabel->setText(t.isEmpty() ? UiHelpers::untitled() : t);
    if (m_titleEdit->isVisible())
        m_titleEdit->setText(m_note.title);
}

void NoteWindow::applyTheme(const QString &themeId)
{
    Theme t = Storage::instance().theme(themeId);
    if (!t.isValid())
        t = Storage::instance().theme(QStringLiteral("giallo"));
    m_theme = t;
    m_note.themeId = t.id;
    applyStyle();
}

void NoteWindow::applyStyle()
{
    const QString fontFamily = Storage::instance().setting(QStringLiteral("font_family"),
                                                           QStringLiteral("DejaVu Sans"));
    const int fontSize = Storage::instance().setting(QStringLiteral("font_size"), QStringLiteral("9")).toInt();

    setStyleSheet(QStringLiteral(
                      // The note background and its title strip are painted by
                      // hand in paintCard() (the shadow in paintEvent()): here
                      // the containers are just made transparent and the
                      // widgets drawn on top of them are styled.
                      "#noteWindow { background: transparent; }"
                      "#titleBar { background: transparent; }"
                      "#titleText { color: %3; font-weight: bold; }"
                      "#titleEdit { color: %3; background: transparent; border: none; font-weight: bold; }"
                      "#resizeGrip { color: %2; background: transparent; font-size: 10px; }"
                      "#collapseButton, #closeButton { color: %3; background: transparent;"
                      " border: none; font-size: 12px; font-weight: bold; }"
                      "#collapseButton:hover, #closeButton:hover {"
                      " background-color: rgba(0, 0, 0, 40); border-radius: 4px; }"
                      "#closeButton:hover { background-color: rgba(190, 40, 40, 200); color: #ffffff; }"
                      // The bottom padding is applied here (not as a document
                      // margin) so it survives when QTextEdit scrolls the caret
                      // into view: the document margin alone is scrolled off and
                      // the last line would touch the bottom border.
                      "#body { background: transparent; border: none; color: %3;"
                      " padding-bottom: %5px; }"
                      // Scrollbars: track in the note background colour, handle in the
                      // title bar colour, so they blend with the note instead of the
                      // default dark/blue platform style. The top/bottom/right insets
                      // are done with borders (not margin): a margin would leave the
                      // area unpainted, showing the platform's black base style. The
                      // %4 bottom/right border (= kResizeGrip) keeps both bars clear of
                      // the bottom-right resize grip; the 8px top border is just spacing.
                      "#body QScrollBar:vertical { background: %1; width: 5px;"
                      " border: 0; border-top: 8px solid %1; border-bottom: %4px solid %1; }"
                      "#body QScrollBar::handle:vertical { background: %2; min-height: 24px;"
                      " border-radius: 2px; }"
                      "#body QScrollBar::add-line:vertical, #body QScrollBar::sub-line:vertical {"
                      " height: 0; background: none; }"
                      "#body QScrollBar::add-page:vertical, #body QScrollBar::sub-page:vertical {"
                      " background: none; }"
                      "#body QScrollBar:horizontal { background: %1; height: 5px;"
                      " border: 0; border-right: %4px solid %1; }"
                      "#body QScrollBar::handle:horizontal { background: %2; min-width: 24px;"
                      " border-radius: 2px; }"
                      "#body QScrollBar::add-line:horizontal, #body QScrollBar::sub-line:horizontal {"
                      " width: 0; background: none; }"
                      "#body QScrollBar::add-page:horizontal, #body QScrollBar::sub-page:horizontal {"
                      " background: none; }")
                      .arg(m_theme.body, m_theme.titleBar, m_theme.text,
                           QString::number(kResizeGrip), QString::number(kBodyPadding)));

    QFont f(fontFamily, fontSize);
    QFont ft(fontFamily, fontSize);
    ft.setBold(true);
    m_titleLabel->setFont(ft);
    m_titleEdit->setFont(ft);
    m_body->document()->setDefaultFont(f);

    // the document text color follows the theme
    QTextCursor c = m_body->textCursor();
    QTextCharFormat fmt = c.charFormat();
    fmt.setForeground(QColor(m_theme.text));
    m_body->setCurrentCharFormat(fmt);

    // The note background is painted by hand from m_theme (see paintCard()):
    // force a repaint so a theme change is reflected right away.
    m_card->update();
}

void NoteWindow::applyOpacity()
{
    m_note.opacity = qBound(10, m_note.opacity, 100);
    const qreal opacity = m_note.opacity / 100.0;

    // QtWayland does not implement QWindow::setOpacity() (Wayland has no
    // standard surface-opacity protocol), so the translucency is composed
    // client-side. The effect is applied to the card, not to the window: the
    // background, the text and the pasted images are composited together and
    // then faded (a real group opacity), while dragging keeps working because
    // NoteWindow::applyMargins() only damages the shadow corner and does not
    // repaint the card (with a top-level effect every partial update would
    // re-render the whole note, images included).
    if (opacity >= 1.0) {
        if (m_card->graphicsEffect())
            m_card->setGraphicsEffect(nullptr);
        update();
        return;
    }
    auto *effect = qobject_cast<QGraphicsOpacityEffect *>(m_card->graphicsEffect());
    if (!effect) {
        effect = new QGraphicsOpacityEffect(m_card);
        m_card->setGraphicsEffect(effect);
    }
    effect->setOpacity(opacity);

    // The shadow is painted by NoteWindow (outside the card), so its alpha
    // follows the opacity by hand.
    update();
}

void NoteWindow::beginTitleEdit()
{
    m_titleEdit->setText(m_note.title);
    m_titleLabel->hide();
    m_titleEdit->show();
    m_titleEdit->setFocus();
    m_titleEdit->selectAll();
}

void NoteWindow::commitTitleEdit()
{
    if (!m_titleEdit->isVisible())
        return;
    m_note.title = m_titleEdit->text();
    m_titleEdit->hide();
    m_titleLabel->show();
    refreshTitleLabel();
    setWindowTitle(m_note.title.isEmpty() ? I18n::t(QStringLiteral("note.default_title"))
                                          : m_note.title);
    m_note.content = m_body->document()->toMarkdown();
    emit contentEdited(m_note.id, m_note.title, m_note.content);
    m_body->setFocus();
}

void NoteWindow::scheduleSaveContent()
{
    m_saveTimer->start();
}

// --------------------------------------------------------------- images

bool NoteWindow::insertImage(const QImage &sourceImage)
{
    if (sourceImage.isNull())
        return false;

    // The image is embedded in the note content as a data URL (no file, no
    // path: it is saved with the note and travels with the JSON backups). It is
    // stored at its original size, capped to kMaxImageSide: the note width is
    // NOT baked in, so the body shows it as wide as the note and widening the
    // note really enlarges the image, up to its stored resolution (see
    // applyImageSizing()).
    const QSize limit(kMaxImageSide, kMaxImageSide);
    QImage image = sourceImage;
    if (image.width() > limit.width() || image.height() > limit.height())
        image = image.scaled(limit, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG")) {
        qWarning() << "[nota] image not encoded";
        return false;
    }

    QTextImageFormat fmt;
    fmt.setName(QStringLiteral("data:image/png;base64,") + QString::fromLatin1(png.toBase64()));
    fmt.setProperty(QTextFormat::ImageAltText, QStringLiteral("image"));
    // One image pixel is drawn as one device pixel, so the image is sharp on
    // scaled displays; QTextImageHandler then keeps it within the note width
    // and grows it back when the note is widened, up to that size.
    const qreal dpr = qMax(1.0, m_body->devicePixelRatioF());
    fmt.setWidth(qMax(1.0, image.width() / dpr));
    fmt.setMaximumWidth(QTextLength(QTextLength::PercentageLength, 100));
    m_body->textCursor().insertImage(fmt);   // replaces the selection
    return true;
}

void NoteWindow::applyImageSizing()
{
    QTextDocument *doc = m_body->document();

    // The image positions are collected first: merging a char format can split
    // or merge fragments and would invalidate the iterators.
    QList<QPair<int, int>> images;   // (position, length)
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (fragment.isValid() && fragment.charFormat().isImageFormat())
                images.append({fragment.position(), fragment.length()});
        }
    }
    if (images.isEmpty())
        return;

    QTextCursor c(doc);
    c.beginEditBlock();
    const qreal dpr = qMax(1.0, m_body->devicePixelRatioF());
    for (const QPair<int, int> &range : images) {
        c.setPosition(range.first);
        c.setPosition(range.first + range.second, QTextCursor::KeepAnchor);
        QTextImageFormat fmt = c.charFormat().toImageFormat();
        // Markdown carries no image size: restore the "one image pixel = one
        // device pixel" width (1:1, sharp on scaled displays) plus the note
        // width limit.
        const QSize px = imagePixelSize(doc, fmt.name());
        if (px.isValid())
            fmt.setWidth(qMax(1.0, px.width() / dpr));
        fmt.setMaximumWidth(QTextLength(QTextLength::PercentageLength, 100));
        c.mergeCharFormat(fmt);
    }
    c.endEditBlock();
}

void NoteWindow::persistScroll()
{
    // Read the live value at persist time instead of tracking every scroll
    // event: only the final position matters when the note is hidden or the
    // app quits.
    if (!m_body)
        return;
    m_note.scroll = m_body->verticalScrollBar()->value();
    emit scrollEdited(m_note.id, m_note.scroll);
}

void NoteWindow::restoreScroll()
{
    if (m_collapsed)
        return;

    const int scroll = m_note.scroll;
    QTimer::singleShot(0, this, [this, scroll] {
        m_body->verticalScrollBar()->setValue(scroll);
    });
}

void NoteWindow::applyCollapsedVisuals()
{
    m_body->setVisible(!m_collapsed);
    m_resizeGrip->setVisible(!m_collapsed);
    m_collapseButton->setText(m_collapsed ? QStringLiteral("\u25a1")   // □ ripristina
                                          : QStringLiteral("_"));
    m_collapseButton->setToolTip(m_collapsed ? I18n::t(QStringLiteral("restore"))
                                             : I18n::t(QStringLiteral("note.minimize")));
}

void NoteWindow::toggleCollapse()
{
    m_collapsed = !m_collapsed;
    m_note.collapsed = m_collapsed;
    if (m_collapsed)
        persistScroll();   // the body disappears: save its position first
    applyCollapsedVisuals();
    applySize();
    applyMargins();
    if (!m_collapsed)
        restoreScroll();
    emit collapsedEdited(m_note.id, m_collapsed);
}

// ------------------------------------------------------------ interaction

void NoteWindow::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // The note occupies the surface minus the transparent shadow border.
    const QRectF card(kShadowMargin, kShadowMargin,
                      width() - 2.0 * kShadowMargin, height() - 2.0 * kShadowMargin);

    // The shadow is painted here, outside the card: the card opacity effect
    // does not cover it, so its alpha is scaled by hand.
    const qreal opacity = qBound(10, m_note.opacity, 100) / 100.0;

    // Small drop shadow: a few concentric rounded rects whose alpha grows
    // toward the note, so it looks slightly raised above the desktop.
    p.setPen(Qt::NoPen);
    for (int i = kShadowMargin; i >= 1; --i) {
        const qreal t = qreal(kShadowMargin - i + 1) / kShadowMargin;   // 0 outer → 1 inner
        const int alpha = int(16.0 * t * t * opacity);
        if (alpha <= 0)
            continue;
        p.setBrush(QColor(0, 0, 0, alpha));
        p.drawRoundedRect(card.adjusted(-i, -i, i, i), kCornerRadius + i, kCornerRadius + i);
    }
}

void NoteWindow::paintCard(QPainter &p)
{
    p.setRenderHint(QPainter::Antialiasing, true);

    // The card covers the note area (the transparent shadow border belongs to
    // NoteWindow, see paintEvent()).
    const QRectF card(QPointF(0, 0), m_card->size());

    // Rounded background, clipped to the corners.
    QPainterPath path;
    path.addRoundedRect(card, kCornerRadius, kCornerRadius);
    p.fillPath(path, QColor(m_theme.body));

    // Title strip, clipped to the same path so it follows the rounded top
    // corners (all four when the note is collapsed and the title bar alone
    // fills the whole card).
    p.save();
    p.setClipPath(path);
    p.fillRect(QRectF(card.left(), card.top(), card.width(), m_titleBar->height()),
               QColor(m_theme.titleBar));
    p.restore();
}

bool NoteWindow::titleBarContains(const QPoint &pos) const
{
    return m_titleBar->rect().contains(m_titleBar->mapFrom(this, pos));
}

void NoteWindow::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // The grip lives in the card, so its coordinates are card-relative.
    const QSize sz = event->size();
    const int cardW = sz.width() - 2 * kShadowMargin;
    const int cardH = sz.height() - 2 * kShadowMargin;
    m_resizeGrip->move(cardW - kResizeGrip, cardH - kResizeGrip);
    m_resizeGrip->raise();
}

bool NoteWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_titleEdit && event->type() == QEvent::FocusOut)
        commitTitleEdit();
    return QWidget::eventFilter(watched, event);
}

void NoteWindow::mousePressEvent(QMouseEvent *event)
{
    const QPoint pos = event->position().toPoint();
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    if (!m_collapsed && m_resizeGrip->isVisible()
        && m_resizeGrip->rect().contains(m_resizeGrip->mapFrom(this, pos))) {
        m_resizing = true;
        m_resizeRefLocal = pos;
        m_resizeStartSize = QSize(m_note.w, m_note.h);
        event->accept();
        return;
    }

    if (titleBarContains(pos)) {
        m_dragging = true;
        m_dragRefLocal = pos;
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void NoteWindow::mouseMoveEvent(QMouseEvent *event)
{
    const QPoint pos = event->position().toPoint();

    // If the button release was lost (no mouseReleaseEvent received), the drag
    // would stay active with a now-stale reference and the position would
    // "explode". We detect it from the button no longer being pressed.
    if ((m_dragging || m_resizing) && !(event->buttons() & Qt::LeftButton)) {
        const bool wasDragging = m_dragging;
        m_dragging = false;
        m_resizing = false;
        unsetCursor();
        clampToScreen();
        applyMargins();
        if (wasDragging)
            emit geometryEdited(m_note.id, m_note.x, m_note.y, m_note.w, m_note.h, m_screenKey);
    }

    if (m_dragging) {
        // LOCAL coordinate with a FIXED reference to the grab point: on Wayland
        // it is the only reliable one. For it to work, the surface must REALLY
        // move on every event: applyMargins() forces the commit (update()),
        // otherwise the surface stays still, the local coordinate keeps
        // growing and the delta accumulates until it "explodes". See applyMargins().
        const QPoint delta = pos - m_dragRefLocal;
        if (!delta.isNull()) {
            m_note.x += delta.x();
            m_note.y += delta.y();
            clampToScreen();
            applyMargins();
        }
        event->accept();
        return;
    }

    if (m_resizing) {
        const QPoint delta = pos - m_resizeRefLocal;
        m_note.w = qMax(kMinW, m_resizeStartSize.width() + delta.x());
        m_note.h = qMax(kMinH, m_resizeStartSize.height() + delta.y());
        applySize();
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void NoteWindow::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_dragging) {
        m_dragging = false;
        unsetCursor();
        clampToScreen();
        applyMargins();
        emit geometryEdited(m_note.id, m_note.x, m_note.y, m_note.w, m_note.h, m_screenKey);
    } else if (m_resizing) {
        m_resizing = false;
        emit geometryEdited(m_note.id, m_note.x, m_note.y, m_note.w, m_note.h, m_screenKey);
    }
    QWidget::mouseReleaseEvent(event);
}

void NoteWindow::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton
        && titleBarContains(event->position().toPoint())) {
        beginTitleEdit();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void NoteWindow::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape && m_titleEdit->isVisible()) {
        commitTitleEdit();
        return;
    }
    QWidget::keyPressEvent(event);
}

// ---------------------------------------------------------- formatting

void NoteWindow::toggleBold()
{
    QTextCharFormat fmt;
    const bool on = m_body->currentCharFormat().fontWeight() == QFont::Bold;
    fmt.setFontWeight(on ? QFont::Normal : QFont::Bold);
    m_body->mergeCurrentCharFormat(fmt);
}

void NoteWindow::toggleItalic()
{
    QTextCharFormat fmt;
    fmt.setFontItalic(!m_body->currentCharFormat().fontItalic());
    m_body->mergeCurrentCharFormat(fmt);
}

void NoteWindow::toggleUnderline()
{
    QTextCharFormat fmt;
    fmt.setFontUnderline(!m_body->currentCharFormat().fontUnderline());
    m_body->mergeCurrentCharFormat(fmt);
}

void NoteWindow::toggleStrike()
{
    QTextCharFormat fmt;
    fmt.setFontStrikeOut(!m_body->currentCharFormat().fontStrikeOut());
    m_body->mergeCurrentCharFormat(fmt);
}

void NoteWindow::makeList(bool numbered)
{
    QTextCursor c = m_body->textCursor();
    c.beginEditBlock();
    QTextListFormat lf;
    lf.setStyle(numbered ? QTextListFormat::ListDecimal : QTextListFormat::ListDisc);
    QTextList *existing = c.currentList();
    if (existing && existing->format().style() == lf.style()) {
        QTextBlockFormat bf = c.blockFormat();
        bf.setIndent(0);
        c.setBlockFormat(bf);
        existing->remove(c.block());
    } else {
        c.createList(lf);
    }
    c.endEditBlock();
}

void NoteWindow::clearFormatting()
{
    QTextCursor selection = m_body->textCursor();
    if (!selection.hasSelection())
        selection.select(QTextCursor::Document);

    QTextDocument *doc = m_body->document();
    const int start = selection.selectionStart();
    const int end = selection.selectionEnd();

    // A fresh format, applied with setCharFormat() instead of mergeCharFormat():
    // replacing the format is what also drops the background and the fonts
    // inherited from a rich paste, while a merge would only override the
    // properties listed here. Links are dropped too, because this format has no
    // anchor properties (the URL text itself stays).
    QTextCharFormat clean;
    clean.setFontWeight(QFont::Normal);
    clean.setFontItalic(false);
    clean.setFontUnderline(false);
    clean.setFontStrikeOut(false);
    clean.setForeground(QColor(m_theme.text));
    clean.clearBackground();

    // The ranges are collected first: changing a format can split or merge
    // fragments and would invalidate the iterators (as in applyImageSizing()).
    // Image fragments are skipped because setCharFormat() replaces the whole
    // char format and would delete the embedded image.
    QList<QPair<int, int>> ranges;   // (position, length)
    for (QTextBlock block = doc->findBlock(start);
         block.isValid() && block.position() < end; block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid() || fragment.charFormat().isImageFormat())
                continue;
            const int from = qMax(fragment.position(), start);
            const int to = qMin(fragment.position() + fragment.length(), end);
            if (from < to)
                ranges.append({from, to - from});
        }
    }

    QTextCursor c(doc);
    c.beginEditBlock();
    for (const QPair<int, int> &range : ranges) {
        c.setPosition(range.first);
        c.setPosition(range.first + range.second, QTextCursor::KeepAnchor);
        c.setCharFormat(clean);
    }

    // A white background pasted from a web page can also be stored in the
    // block format (paragraph background) and in the block char format (the
    // paragraph mark), which the character format reset does not touch. Lists,
    // indentation and alignment are preserved.
    for (QTextBlock block = doc->findBlock(start);
         block.isValid() && block.position() < end; block = block.next()) {
        QTextBlockFormat bf = block.blockFormat();
        QTextCursor blockCursor(block);
        if (bf.hasProperty(QTextFormat::BackgroundBrush)) {
            bf.clearBackground();
            blockCursor.setBlockFormat(bf);
        }
        blockCursor.setBlockCharFormat(clean);
    }
    c.endEditBlock();

    // The insertion format may still carry the old background: make sure text
    // typed right after the reset starts clean too.
    if (!m_body->textCursor().hasSelection())
        m_body->setCurrentCharFormat(clean);
}

void NoteWindow::pastePlainText()
{
    const QMimeData *mime = QApplication::clipboard()->mimeData();
    if (!mime)
        return;

    if (mime->hasText()) {
        // Insert the plain text with a clean character format: dropping the
        // HTML alone is not enough, because QTextEdit would otherwise keep the
        // color/background of the cursor's current format (e.g. inherited from
        // a previous rich paste). Foreground is set to the note's text color so
        // the result matches the theme; everything else is left at its default
        // (normal weight, no italic/underline/strike, no background).
        QTextCharFormat fmt;
        fmt.setForeground(QColor(m_theme.text));
        QTextCursor c = m_body->textCursor();
        c.beginEditBlock();
        c.insertText(mime->text(), fmt);
        c.endEditBlock();
        return;
    }

    // No text on the clipboard (an image, or image files): keep the normal
    // paste behaviour so the shortcut stays useful.
    m_body->paste();
}

// -------------------------------------------------------------- menu

void NoteWindow::buildContextMenu(const QPoint &globalPos)
{
    QMenu menu;
    const QTextCharFormat cur = m_body->currentCharFormat();

    // Clipboard: paste while dropping the HTML (colors, backgrounds, fonts)
    // copied from a web page. Ctrl+Shift+V is handled by the body itself; the
    // shortcut here is only displayed in the menu.
    QAction *plainPaste = menu.addAction(I18n::t(QStringLiteral("paste_plain")));
    plainPaste->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+V")));
    connect(plainPaste, &QAction::triggered, this, &NoteWindow::pastePlainText);

    menu.addSeparator();

    QAction *bold = menu.addAction(I18n::t(QStringLiteral("format.bold")));
    bold->setShortcut(QKeySequence::Bold);
    bold->setCheckable(true);
    bold->setChecked(cur.fontWeight() == QFont::Bold);
    connect(bold, &QAction::triggered, this, &NoteWindow::toggleBold);

    QAction *ital = menu.addAction(I18n::t(QStringLiteral("format.italic")));
    ital->setShortcut(QKeySequence::Italic);
    ital->setCheckable(true);
    ital->setChecked(cur.fontItalic());
    connect(ital, &QAction::triggered, this, &NoteWindow::toggleItalic);

    QAction *under = menu.addAction(I18n::t(QStringLiteral("format.underline")));
    under->setShortcut(QKeySequence::Underline);
    under->setCheckable(true);
    under->setChecked(cur.fontUnderline());
    connect(under, &QAction::triggered, this, &NoteWindow::toggleUnderline);

    QAction *strike = menu.addAction(I18n::t(QStringLiteral("format.strikethrough")));
    strike->setCheckable(true);
    strike->setChecked(cur.fontStrikeOut());
    connect(strike, &QAction::triggered, this, &NoteWindow::toggleStrike);

    menu.addSeparator();
    connect(menu.addAction(I18n::t(QStringLiteral("format.bulleted_list"))), &QAction::triggered,
            this, [this] { makeList(false); });
    connect(menu.addAction(I18n::t(QStringLiteral("format.numbered_list"))), &QAction::triggered,
            this, [this] { makeList(true); });
    connect(menu.addAction(I18n::t(QStringLiteral("format.clear"))), &QAction::triggered,
            this, &NoteWindow::clearFormatting);

    menu.addSeparator();
    QMenu *colors = menu.addMenu(I18n::t(QStringLiteral("note.color")));
    for (const Theme &t : Storage::instance().themes()) {
        QAction *a = colors->addAction(colorIcon(t.body), Themes::displayName(t));
        a->setCheckable(true);
        a->setChecked(t.id == m_note.themeId);
        const QString id = t.id;
        connect(a, &QAction::triggered, this, [this, id] {
            applyTheme(id);
            emit themeEdited(m_note.id, id);
        });
    }

    QMenu *opacity = menu.addMenu(I18n::t(QStringLiteral("note.opacity")));
    for (int pct = 100; pct >= 10; pct -= 10) {
        QAction *a = opacity->addAction(QStringLiteral("%1%").arg(pct));
        a->setCheckable(true);
        a->setChecked(pct == m_note.opacity);
        connect(a, &QAction::triggered, this, [this, pct] {
            m_note.opacity = pct;
            applyOpacity();
            emit opacityEdited(m_note.id, pct);
        });
    }

    QAction *ontop = menu.addAction(I18n::t(QStringLiteral("note.always_on_top")));
    ontop->setCheckable(true);
    ontop->setChecked(isAlwaysOnTop());
    connect(ontop, &QAction::triggered, this, [this] {
        const QString layer = isAlwaysOnTop() ? QStringLiteral("bottom") : QStringLiteral("top");
        applyLayer(layer);
        emit layerEdited(m_note.id, layer);
    });

    menu.addSeparator();
    connect(menu.addAction(I18n::t(QStringLiteral("new_note"))), &QAction::triggered,
            this, &NoteWindow::newNoteRequested);
    connect(menu.addAction(I18n::t(QStringLiteral("note.duplicate"))), &QAction::triggered,
            this, [this] { emit duplicateRequested(m_note.id); });
    connect(menu.addAction(I18n::t(QStringLiteral("note.hide"))), &QAction::triggered,
            this, [this] { emit hideRequested(m_note.id); });

    menu.addSeparator();
    connect(menu.addAction(I18n::t(QStringLiteral("note.delete_to_trash"))), &QAction::triggered,
            this, [this] { emit deleteRequested(m_note.id); });
    connect(menu.addAction(I18n::t(QStringLiteral("note.settings"))), &QAction::triggered,
            this, &NoteWindow::settingsRequested);

    menu.exec(globalPos);
}

void NoteWindow::contextMenuEvent(QContextMenuEvent *event)
{
    if (titleBarContains(event->pos()))
        buildContextMenu(event->globalPos());
    else
        QWidget::contextMenuEvent(event);
}
