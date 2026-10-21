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
#include <QWindow>
#include <QClipboard>
#include <QDesktopServices>
#include <QPalette>
#include <QRegularExpression>
#include <QScreen>
#include <QSet>
#include <QMimeData>
#include <QUrl>
#include <QMouseEvent>
#include <QResizeEvent>
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
// How long the resizing edge stays on the edge of a nearby note it just met.
// While the hold is active the pointer movement is ignored; when it expires the
// edge catches up with the pointer again.
constexpr int kResizeHoldMs = 100;
// Maximum gap between the notes on the axis perpendicular to the edge being
// dragged for their edges to be considered. A note does not have to touch the
// resized one: two notes a few pixels apart still line up.
constexpr int kResizeHoldRange = 600;
// How close (in pixels) the moving edge must get to an edge of a nearby note to
// snap onto it: within this distance the edge jumps straight onto the exact
// alignment (drag and resize), which makes the snap clearly visible. 0 = snap
// only when the edges actually touch. Increase it to make the snap stronger.
constexpr int kSnapDistance = 10;
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
/// stays on the same port). This is the only place that builds the key: the
/// database stores it as an opaque string.
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

/// Live note windows, used as the resize hold targets: the geometry of a note
/// is not known by the caller that manages their lifetime (AppController), and
/// the set is small, so the notes keep track of themselves.
QSet<NoteWindow *> g_noteWindows;

/// True when two intervals (exclusive ends) overlap or their gap is at most
/// \a maxGap pixels: used to consider a note "near" on the other axis.
bool intervalsNear(int aStart, int aEnd, int bStart, int bEnd, int maxGap)
{
    return aStart <= bEnd + maxGap && bStart <= aEnd + maxGap;
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
    m_snapEnabled = Storage::instance().setting(QStringLiteral("snap"), QStringLiteral("1"))
                    == QLatin1String("1");
    setAttribute(Qt::WA_StyledBackground, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setObjectName(QStringLiteral("noteWindow"));
    setFocusPolicy(Qt::StrongFocus);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(kShadowMargin, kShadowMargin, kShadowMargin, kShadowMargin);
    root->setSpacing(0);
    // The minimum size of the window must come from the note (kMinW/kMinH), not
    // from the content: by default the layout exports its own minimum to the
    // window, and the title QLabel reports the whole text width as its minimum,
    // so a long title would stop the note from being narrowed. Worse, the
    // layer-shell wish (setDesiredSize()) and the clamped window size would
    // alternate: the compositor reconfigures the surface to the size asked by
    // the client, the widget refuses it and commits a bigger buffer, and so on
    // -> the note's width flashes while being resized. Keep the layout out of
    // the window size (the surface is sized by applySize()) and declare the same
    // floor the resize math enforces.
    root->setSizeConstraint(QLayout::SetNoConstraint);
    setMinimumSize(kMinW + 2 * kShadowMargin, kTitleHeight + 2 * kShadowMargin);

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
    // The title never dictates the note width: it is elided to the space the
    // label gets (see elideTitle()), while the size policy keeps its text out
    // of the layout minimum.
    m_titleLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_titleLabel->installEventFilter(this);   // re-elide when it is resized

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
    connect(m_saveTimer, &QTimer::timeout, this, &NoteWindow::saveContent);
    connect(m_focusLayerTimer, &QTimer::timeout, this, &NoteWindow::onFocusLayerTimeout);

    // content
    m_loading = true;
    m_body->document()->setMarkdown(m_note.content);
    applyImageSizing();   // Markdown carries no image size: refit to the note width
    m_body->document()->clearUndoRedoStacks();
    // From now on URLs typed or pasted are recognized; doing it during the
    // Markdown import would interfere with QTextDocument::setMarkdown().
    static_cast<NoteBodyEdit *>(m_body)->setLinkifyEnabled(true);

    refreshTitleLabel();
    // applyStyle() recolors the document; keep m_loading set so that this
    // formatting pass does not start a save of the just-loaded content.
    applyTheme(m_note.themeId);
    m_body->document()->clearUndoRedoStacks();
    m_loading = false;
    applyOpacity();

    // Saved collapsed state: restore the appearance (applySize() applies the height).
    m_collapsed = m_note.collapsed;
    applyCollapsedVisuals();

    // layer-shell
    createWinId();
    // Catch mouse presses before QWidgetWindow dispatches them: a note is
    // unfocusable until the user interacts with it (see configureLayerShell()).
    if (QWindow *wh = windowHandle())
        wh->installEventFilter(this);
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

    g_noteWindows.insert(this);
}

NoteWindow::~NoteWindow()
{
    g_noteWindows.remove(this);
    // The debounced save timer dies with the window: persist any pending edit
    // first (e.g. the note is deleted to the trash before the timer fires).
    flushPendingSave();
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
    // A note is not focusable by default. With OnDemand always set, KWin
    // considers the note a focus candidate (wantsInput()) and hands it the
    // keyboard as soon as no normal window has focus: when the last window is
    // minimized a note starts blinking its caret, and the WindowActivate event
    // raises it to the top layer for a moment (visible as a flash when the
    // window is restored).
    //
    // Keyboard interactivity is therefore enabled only when the user really
    // interacts with the note (a mouse press, see eventFilter()) or when the
    // app explicitly activates it (activate(), e.g. a new note), and it is
    // disabled again as soon as focus is lost (onFocusLayerTimeout()).
    // Note: setActivateOnShow() has no effect while interactivity is None, so
    // notes shown at startup do not steal the focus from any window.
    m_layer->setKeyboardInteractivity(LS::KeyboardInteractivityNone);
    m_layer->setExclusiveZone(0);
    m_layer->setActivateOnShow(true);
    m_layer->setAnchors(LS::Anchors::fromInt(int(LS::AnchorTop) | int(LS::AnchorLeft)));
}

void NoteWindow::commitLayerSurface()
{
    // setMargins()/setLayer()/setKeyboardInteractivity() are double-buffered:
    // only a wl_surface commit applies them. Declare the whole surface, so the
    // commit is valid for any compositor. During a drag the card is hidden and
    // frozen into m_dragSnapshot (beginDragVisuals()), hence this full repaint
    // is a blit plus the shadow, not a body repaint.
    update();
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
        // We commit the whole surface on purpose: the damage size does not move
        // the surface (any commit applies the new margin) and a commit whose
        // damage does not cover the moved surface is fragile. The full repaint
        // stays cheap during a drag because the card is replaced by
        // m_dragSnapshot (see beginDragVisuals()): it costs a blit plus the
        // shadow, even for a note holding a large image, so the surface keeps
        // up with the pointer and the fixed-reference math in mouseMoveEvent()
        // remains valid. Moving does not change the note content.
        commitLayerSurface();
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

QRect NoteWindow::globalNoteRect() const
{
    // m_note.x/y are relative to the available area of the note's screen
    // (m_screenRect, in global coordinates); the note's card does not include
    // the transparent shadow border of the surface.
    const int h = m_collapsed ? kTitleHeight : m_note.h;
    return QRect(m_screenRect.topLeft() + QPoint(m_note.x, m_note.y),
                 QSize(m_note.w, h));
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
    int h = m_collapsed ? kTitleHeight : m_note.h;
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

    // A restored per-screen position may carry a different height: refresh the
    // one used by the final clamp (the mapping above used the current one).
    h = m_collapsed ? kTitleHeight : m_note.h;
    if (proportional) {
        // Guarantee the whole note is reachable again (its left/top part when
        // the note is wider/taller than the screen).
        m_note.x = qBound(targetRel.left(), m_note.x,
                          targetRel.left() + qMax(0, targetRel.width() - m_note.w));
        m_note.y = qBound(targetRel.top(), m_note.y,
                          targetRel.top() + qMax(0, targetRel.height() - h));
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
    // without a wl_surface commit the compositor does not apply it. Use the
    // single commit helper.
    commitLayerSurface();
}

void NoteWindow::setKeyboardInteractive(bool interactive)
{
    if (!m_layer)
        return;
    using LS = LayerShellQt::Window;
    const LS::KeyboardInteractivity mode = interactive ? LS::KeyboardInteractivityOnDemand
                                                       : LS::KeyboardInteractivityNone;
    if (m_layer->keyboardInteractivity() == mode)
        return;
    m_layer->setKeyboardInteractivity(mode);

    // The keyboard interactivity is double-buffered as well: commit the whole
    // surface through the single helper (see commitLayerSurface()).
    commitLayerSurface();
}

void NoteWindow::requestKeyboardFocus()
{
    // A bottom-layer note is raised first: KWin only activates top/overlay
    // on-demand surfaces when their keyboard interactivity is switched on
    // (LayerShellV1Window::handleAcceptsFocusChanged()), while a bottom surface
    // stays below normal windows and never takes the keyboard. Raising it is
    // what WindowActivate would do anyway a moment later; if the request is
    // refused, the focus timer lowers it again.
    const bool raised = !isActiveWindow() && m_note.layer == QLatin1String("bottom");
    if (raised) {
        m_raisedForFocus = true;
        applyEffectiveLayer();
    }
    setKeyboardInteractive(true);
    if (QWindow *wh = windowHandle())
        wh->requestActivate();

    if (raised) {
        // Safeguard: if the compositor refuses the focus request, do not leave
        // a bottom note raised and focusable (it would be the very focus
        // candidate this change avoids). WindowActivate stops the timer when
        // the note really gets the focus; onFocusLayerTimeout() also handles
        // the popup case.
        m_focusLayerTimer->start();
    }
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
    // Really unfocused: stop being a focus candidate, otherwise the
    // compositor would hand the keyboard to this note when no normal window
    // has focus (see configureLayerShell()).
    setKeyboardInteractive(false);
}

void NoteWindow::setSnappingEnabled(bool enabled)
{
    if (m_snapEnabled == enabled)
        return;
    m_snapEnabled = enabled;
    // A hold in progress must not survive the option being turned off.
    if (!enabled) {
        clearResizeHolds();
        clearDragHolds();
    }
}

void NoteWindow::setVisibleState(bool visible)
{
    m_note.visible = visible;
    if (!visible) {
        // Hidden: no focus possible, return to the configured layer.
        m_focusLayerTimer->stop();
        m_raisedForFocus = false;
        m_dragging = false;
        m_resizing = false;
        clearResizeHolds();
        clearDragHolds();
        unsetCursor();
        m_dragRestoreBodyFocus = false;
        endDragVisuals();
        setKeyboardInteractive(false);
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
    // Enable keyboard interactivity before showing: setActivateOnShow() only
    // requests activation when the surface is not None.
    setKeyboardInteractive(true);
    show();
    raise();
    // Asks the compositor for focus: this way a "desktop" note opened from the
    // hub is really brought to the front (and stays there while it has focus).
    requestKeyboardFocus();
    m_body->setFocus();
}

void NoteWindow::refreshTitleLabel()
{
    const QString t = m_note.title.trimmed();
    m_titleLabel->setText(t.isEmpty() ? UiHelpers::untitled() : t);
    elideTitle();
    if (m_titleEdit->isVisible())
        m_titleEdit->setText(m_note.title);
}

void NoteWindow::elideTitle()
{
    // The label shows the elided form of the full title, recomputed from
    // m_note.title (never from the already elided text) whenever the label
    // changes width or font. Keeping the full text in the note also means the
    // title is not lost when the note is too narrow to show it.
    const QString title = m_note.title.trimmed();
    const QString full = title.isEmpty() ? UiHelpers::untitled() : title;
    const QString shown = m_titleLabel->fontMetrics().elidedText(full, Qt::ElideRight,
                                                                 m_titleLabel->width());
    if (shown != m_titleLabel->text())
        m_titleLabel->setText(shown);
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
    elideTitle();   // the new font may fit a different part of the title

    // the document text color follows the theme
    QTextCursor c = m_body->textCursor();
    QTextCharFormat fmt = c.charFormat();
    fmt.setForeground(QColor(m_theme.text));
    m_body->setCurrentCharFormat(fmt);

    // The format above only affects newly typed text, and the stylesheet only
    // the text without an explicit foreground: recolor what is already in the
    // document (e.g. after a theme change). A merge keeps bold/italic/links.
    QTextCursor all(m_body->document());
    all.select(QTextCursor::Document);
    QTextCharFormat fg;
    fg.setForeground(QColor(m_theme.text));
    all.mergeCharFormat(fg);

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
    // during a drag the card is frozen into m_dragSnapshot and the full-surface
    // commit only blits it (see beginDragVisuals()).
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

void NoteWindow::saveContent()
{
    if (m_loading)
        return;
    m_note.content = m_body->document()->toMarkdown();
    emit contentEdited(m_note.id, m_note.title, m_note.content);
}

void NoteWindow::flushPendingSave()
{
    if (m_loading || !m_saveTimer->isActive())
        return;
    m_saveTimer->stop();
    saveContent();
}

// --------------------------------------------------------------- images

bool NoteWindow::insertImage(const QImage &sourceImage)
{
    if (sourceImage.isNull())
        return false;

    // The image is embedded in the note content as a data URL (no file, no
    // path: it is saved with the note and travels with the database backups). It is
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

    // During a drag the card is hidden and frozen into m_dragSnapshot, so the
    // full-surface commit costs a blit instead of a body repaint (see
    // beginDragVisuals()). The snapshot is captured without the card opacity
    // effect, so apply the opacity here once, like for the shadow.
    if (!m_dragSnapshot.isNull()) {
        p.setOpacity(opacity);
        p.drawPixmap(kShadowMargin, kShadowMargin, m_dragSnapshot);
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
    // The layout gives the title whatever width is left once the buttons have
    // theirs: elide the text to it, so a long title is cut with "..." instead of
    // forcing the note wider (see elideTitle()).
    if (watched == m_titleLabel && event->type() == QEvent::Resize) {
        elideTitle();
        return false;
    }

    // A note is not a focus candidate until the user interacts with it (see
    // configureLayerShell()). The press is caught on the QWindow, before
    // QWidgetWindow dispatches it to the body/title bar/buttons.
    if (watched == windowHandle() && event->type() == QEvent::MouseButtonPress) {
        // Remember whether the body has the keyboard BEFORE the press moves the
        // focus to the note: hiding the card for the drag snapshot takes it
        // away, and endDragVisuals() gives it back (see beginDragVisuals()).
        m_dragRestoreBodyFocus = m_body->hasFocus();
        if (!isActiveWindow())
            requestKeyboardFocus();
        return false;   // keep normal event delivery
    }

    return QWidget::eventFilter(watched, event);
}

void NoteWindow::beginDragVisuals()
{
    if (!m_dragSnapshot.isNull())
        return;
    // QWidget::grab() on a widget carrying a QGraphicsEffect renders the effect
    // source in device coordinates: the returned pixmap has the card content
    // shifted by the card position in the window and its right/bottom edge cut
    // (visible as a shrinking note with missing borders while dragging). Remove
    // the opacity effect for the grab; paintEvent() applies the opacity to the
    // snapshot, and applyOpacity() recreates the effect on the card.
    const bool hadEffect = m_card->graphicsEffect() != nullptr;
    if (hadEffect)
        m_card->setGraphicsEffect(nullptr);   // deletes the effect
    m_dragSnapshot = m_card->grab();          // DPR-aware: drawn as-is
    if (hadEffect)
        applyOpacity();                       // restore the card fade
    if (m_dragSnapshot.isNull())
        return;          // fallback: the card stays visible
    m_card->hide();      // paintEvent() draws the snapshot instead
    update();
}

void NoteWindow::endDragVisuals()
{
    if (m_dragSnapshot.isNull())
        return;
    m_dragSnapshot = QPixmap();
    m_card->show();
    // Hiding the card took the focus away from the body: give the caret back,
    // so typing can continue right after the drag.
    if (m_dragRestoreBodyFocus) {
        m_dragRestoreBodyFocus = false;
        // Hiding the card may also have dropped the window activation: ask for
        // it again before giving the keyboard back to the body.
        if (!isActiveWindow())
            requestKeyboardFocus();
        m_body->setFocus();
    }
    update();
}

int NoteWindow::resizeHoldEdge(ResizeHold &hold, int rawEdge, int fixedEdge,
                               const QList<int> &peerEdges, int minSize)
{
    if (hold.active) {
        // The capture zone starts kSnapDistance before the edge in the direction
        // of the movement: leaving it on the opposite side releases the snap.
        const int boundary = hold.direction > 0 ? hold.heldEdge - kSnapDistance
                                                : hold.heldEdge + kSnapDistance;
        const bool reversed = hold.direction > 0 ? rawEdge < boundary
                                                 : rawEdge > boundary;
        if (reversed) {
            hold.active = false;
            hold.prevEdge = rawEdge;
            return rawEdge;
        }
        // Stay on the target while the pointer is still short of it. Once the
        // pointer has passed it (after the pause) the snap is released and the
        // edge catches up with the pointer.
        const bool passed = hold.direction > 0 ? rawEdge >= hold.heldEdge
                                               : rawEdge <= hold.heldEdge;
        if (!passed || hold.timer.elapsed() < kResizeHoldMs) {
            hold.prevEdge = hold.heldEdge;
            return hold.heldEdge;
        }
        hold.active = false;
        hold.prevEdge = rawEdge;
        return rawEdge;
    }

    // Nearest peer edge ahead (in the direction of the movement) that is now
    // within the capture distance: the snap brings the edge exactly onto it. A
    // target that would leave less than the minimum size is not valid.
    const int prev = hold.prevEdge;
    bool found = false;
    int best = 0;
    int bestTravel = 0;
    if (rawEdge > prev) {
        for (int edge : peerEdges) {
            if (edge - fixedEdge < minSize)
                continue;
            if (prev >= edge || rawEdge < edge - kSnapDistance)
                continue;
            const int travel = edge - prev;
            if (!found || travel < bestTravel) {
                found = true;
                bestTravel = travel;
                best = edge;
            }
        }
    } else if (rawEdge < prev) {
        for (int edge : peerEdges) {
            if (edge - fixedEdge < minSize)
                continue;
            if (prev <= edge || rawEdge > edge + kSnapDistance)
                continue;
            const int travel = prev - edge;
            if (!found || travel < bestTravel) {
                found = true;
                bestTravel = travel;
                best = edge;
            }
        }
    }

    if (found) {
        hold.active = true;
        hold.direction = rawEdge > prev ? 1 : -1;
        hold.heldEdge = best;
        hold.timer.restart();
        hold.prevEdge = best;
        return best;
    }

    hold.prevEdge = rawEdge;
    return rawEdge;
}

void NoteWindow::clearResizeHolds()
{
    m_resizeHoldX = ResizeHold();
    m_resizeHoldY = ResizeHold();
}

int NoteWindow::dragHoldPos(DragHold &hold, int freePos, int prevPos, int size,
                            const QList<int> &nearEdges, const QList<int> &farEdges)
{
    if (hold.active) {
        // The capture zone starts kSnapDistance before the edge in the direction
        // of the movement: leaving it on the opposite side releases the snap.
        const int boundary = hold.direction > 0 ? hold.heldPos - kSnapDistance
                                                : hold.heldPos + kSnapDistance;
        const bool reversed = hold.direction > 0 ? freePos < boundary
                                                 : freePos > boundary;
        if (reversed) {
            hold.active = false;
            return freePos;
        }
        // Stay on the alignment while the pointer is still short of it. Once
        // the pointer has passed it (after the pause) the snap is released and
        // the note catches up with the pointer.
        const bool passed = hold.direction > 0 ? freePos >= hold.heldPos
                                               : freePos <= hold.heldPos;
        if (!passed || hold.timer.elapsed() < kResizeHoldMs)
            return hold.heldPos;
        hold.active = false;
        return freePos;
    }

    const int direction = freePos > prevPos ? 1 : (freePos < prevPos ? -1 : 0);
    if (direction == 0)
        return freePos;

    // Nearest homologous edge ahead (in the direction of the movement) that is
    // now within the capture distance: the note is placed exactly on it. The
    // near edge of the note (left/top) is checked against \a nearEdges, the far
    // one (right/bottom) against \a farEdges.
    bool found = false;
    int bestTravel = 0;
    int bestPos = 0;
    for (int edge : nearEdges) {
        if (direction > 0 ? (prevPos >= edge || freePos < edge - kSnapDistance)
                          : (prevPos <= edge || freePos > edge + kSnapDistance))
            continue;
        const int travel = direction > 0 ? edge - prevPos : prevPos - edge;
        if (!found || travel < bestTravel) {
            found = true;
            bestTravel = travel;
            bestPos = edge;
        }
    }
    for (int edge : farEdges) {
        const int movingStart = prevPos + size;
        const int movingEnd = freePos + size;
        if (direction > 0 ? (movingStart >= edge || movingEnd < edge - kSnapDistance)
                          : (movingStart <= edge || movingEnd > edge + kSnapDistance))
            continue;
        const int travel = direction > 0 ? edge - movingStart : movingStart - edge;
        if (!found || travel < bestTravel) {
            found = true;
            bestTravel = travel;
            bestPos = edge - size;
        }
    }

    if (!found)
        return freePos;

    hold.active = true;
    hold.direction = direction;
    hold.heldPos = bestPos;
    hold.timer.restart();
    return bestPos;
}

void NoteWindow::clearDragHolds()
{
    m_dragHoldX = DragHold();
    m_dragHoldY = DragHold();
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
        clearResizeHolds();
        const QRect g = globalNoteRect();
        m_resizeHoldX.prevEdge = g.x() + g.width();
        m_resizeHoldY.prevEdge = g.y() + g.height();
        event->accept();
        return;
    }

    if (titleBarContains(pos)) {
        m_dragging = true;
        m_dragRefLocal = pos;
        clearDragHolds();
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
        clearResizeHolds();
        clearDragHolds();
        unsetCursor();
        clampToScreen();
        applyMargins();
        endDragVisuals();
        if (wasDragging)
            emit geometryEdited(m_note.id, m_note.x, m_note.y, m_note.w, m_note.h, m_screenKey);
    }

    if (m_dragging) {
        // Freeze the card on the first real movement only (a click or the first
        // click of a double click must not pay the grab nor flicker).
        if (m_dragSnapshot.isNull())
            beginDragVisuals();

        // LOCAL coordinate with a FIXED reference to the grab point: on Wayland
        // it is the only reliable one. For it to work, the surface must REALLY
        // move on every event: applyMargins() forces a full commit
        // (commitLayerSurface()), otherwise the surface stays still, the local
        // coordinate keeps growing and the delta accumulates until it
        // "explodes". See applyMargins().
        const QPoint delta = pos - m_dragRefLocal;
        if (!delta.isNull() && !m_snapEnabled) {
            // Snapping off: free movement, the original path untouched.
            m_note.x += delta.x();
            m_note.y += delta.y();
            clampToScreen();
            applyMargins();
        } else if (!delta.isNull()) {
            // The note pauses as soon as one of its edges meets the homologous
            // edge of a nearby note (left-left, right-right, top-top,
            // bottom-bottom): see dragHoldPos(). Notes only need to be near on
            // the other axis (same kResizeHoldRange as the resize).
            const int visualH = m_collapsed ? kTitleHeight : m_note.h;
            const int prevX = m_note.x;
            const int prevY = m_note.y;
            const int freeX = prevX + delta.x();
            const int freeY = prevY + delta.y();
            // The note occupies its current position: the free target is only
            // used to find the edge crossings below.
            const QRect ours(m_screenRect.topLeft() + QPoint(prevX, prevY),
                             QSize(m_note.w, visualH));

            QList<int> peerLefts;
            QList<int> peerRights;
            QList<int> peerTops;
            QList<int> peerBottoms;
            for (NoteWindow *peer : g_noteWindows) {
                if (peer == this || !peer->isVisible())
                    continue;
                const QRect r = peer->globalNoteRect();
                if (intervalsNear(ours.top(), ours.top() + ours.height(),
                                  r.top(), r.top() + r.height(), kResizeHoldRange)) {
                    peerLefts.append(r.left());
                    peerRights.append(r.left() + r.width());
                }
                if (intervalsNear(ours.left(), ours.left() + ours.width(),
                                  r.left(), r.left() + r.width(), kResizeHoldRange)) {
                    peerTops.append(r.top());
                    peerBottoms.append(r.top() + r.height());
                }
            }

            m_note.x = dragHoldPos(m_dragHoldX, freeX, prevX, m_note.w,
                                   peerLefts, peerRights);
            m_note.y = dragHoldPos(m_dragHoldY, freeY, prevY, visualH,
                                   peerTops, peerBottoms);
            const QString oldKey = m_screenKey;
            clampToScreen();
            // A screen change (or the clamp) invalidates the held position.
            if (m_screenKey != oldKey) {
                clearDragHolds();
            } else {
                if (m_dragHoldX.active && m_note.x != m_dragHoldX.heldPos)
                    m_dragHoldX = DragHold();
                if (m_dragHoldY.active && m_note.y != m_dragHoldY.heldPos)
                    m_dragHoldY = DragHold();
            }
            applyMargins();
        }
        event->accept();
        return;
    }

    if (m_resizing) {
        const QPoint delta = pos - m_resizeRefLocal;
        const int rawW = qMax(kMinW, m_resizeStartSize.width() + delta.x());
        const int rawH = qMax(kMinH, m_resizeStartSize.height() + delta.y());

        if (!m_snapEnabled) {
            m_note.w = rawW;
            m_note.h = rawH;
            applySize();
            event->accept();
            return;
        }

        // The top-left corner is fixed while resizing: only the right and
        // bottom edges move, so the peers are compared in global coordinates.
        const QPoint origin = m_screenRect.topLeft() + QPoint(m_note.x, m_note.y);
        const QRect ours(origin, QSize(rawW, rawH));

        // Edge candidates of the nearby notes. On each axis only the notes
        // near the note on the other axis are considered (overlapping or within
        // kResizeHoldRange), so a note slightly apart still lines up while a
        // distant one cannot stop an edge that would not even reach it.
        QList<int> xEdges;
        QList<int> yEdges;
        for (NoteWindow *peer : g_noteWindows) {
            if (peer == this || !peer->isVisible())
                continue;
            const QRect r = peer->globalNoteRect();
            if (intervalsNear(ours.top(), ours.top() + ours.height(),
                              r.top(), r.top() + r.height(), kResizeHoldRange)) {
                xEdges.append(r.left());
                xEdges.append(r.left() + r.width());
            }
            if (intervalsNear(ours.left(), ours.left() + ours.width(),
                              r.left(), r.left() + r.width(), kResizeHoldRange)) {
                yEdges.append(r.top());
                yEdges.append(r.top() + r.height());
            }
        }

        const int right = resizeHoldEdge(m_resizeHoldX, origin.x() + rawW, origin.x(),
                                         xEdges, kMinW);
        const int bottom = resizeHoldEdge(m_resizeHoldY, origin.y() + rawH, origin.y(),
                                          yEdges, kMinH);

        m_note.w = qMax(kMinW, right - origin.x());
        m_note.h = qMax(kMinH, bottom - origin.y());
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
        clearDragHolds();
        unsetCursor();
        clampToScreen();
        applyMargins();
        endDragVisuals();
        emit geometryEdited(m_note.id, m_note.x, m_note.y, m_note.w, m_note.h, m_screenKey);
    } else if (m_resizing) {
        m_resizing = false;
        clearResizeHolds();
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
        QAction *a = colors->addAction(UiHelpers::swatchIcon(t.body), Themes::displayName(t));
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
