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
#include <QToolButton>
#include <QMenu>
#include <QTimer>
#include <QPainter>
#include <QPixmap>
#include <QIcon>
#include <QApplication>
#include <QScreen>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QFocusEvent>
#include <QTextListFormat>
#include <QTextList>
#include <QTextBlock>
#include <QContextMenuEvent>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QDebug>

namespace {

constexpr int kTitleHeight = 30;
constexpr int kResizeGrip = 16;
constexpr int kMinW = 160;
constexpr int kMinH = 120;
// Inner padding of the note body. The same value is used for QTextDocument's
// margin (= top/left/right/bottom padding of the text) and for the extra
// bottom padding of the editor (see applyStyle()): without it, when the caret
// reaches the last line QTextEdit scrolls the document's bottom margin out of
// view and the text touches the bottom border.
constexpr int kBodyPadding = 10;
constexpr qreal kListIndentWidth = 25; //Default QTextListFormat indent.

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

} // namespace

NoteWindow::NoteWindow(const Note &note, QWidget *parent)
    : QWidget(parent)
    , m_note(note)
{
    setWindowTitle(note.title.isEmpty() ? I18n::t(QStringLiteral("note.default_title"))
                                        : note.title);
    setAttribute(Qt::WA_StyledBackground, true);
    setObjectName(QStringLiteral("noteWindow"));
    setFocusPolicy(Qt::StrongFocus);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ---- title bar ----
    m_titleBar = new QWidget(this);
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
    m_body = new QTextEdit(this);
    m_body->setObjectName(QStringLiteral("body"));
    m_body->setFrameShape(QFrame::NoFrame);
    m_body->setAcceptRichText(true);
    m_body->setContextMenuPolicy(Qt::CustomContextMenu);
    m_body->document()->setDocumentMargin(kBodyPadding);
    m_body->document()->setIndentWidth(kListIndentWidth);

    root->addWidget(m_titleBar);
    root->addWidget(m_body, 1);

    // ---- resize grip (bottom right) ----
    m_resizeGrip = new QLabel(QStringLiteral("\u25e2"), this);   // ◢
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
    m_body->document()->clearUndoRedoStacks();
    m_loading = false;

    refreshTitleLabel();
    applyTheme(m_note.themeId);

    // Saved collapsed state: restore the appearance (applySize() applies the height).
    m_collapsed = m_note.collapsed;
    applyCollapsedVisuals();

    // layer-shell
    createWinId();
    configureLayerShell();
    m_note.w = qMax(kMinW, m_note.w);
    m_note.h = qMax(kMinH, m_note.h);
    clampToScreen();     // repairs any saved off-screen positions
    applySize();
    applyMargins();
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
        m_layer->setMargins(QMargins(m_note.x, m_note.y, 0, 0));

        // The layer-shell state (margin included) is double-buffered: the
        // compositor applies it only on the next wl_surface commit.
        // LayerShellQt::setMargins() does NOT commit and during a drag the
        // widget does not redraw, so the window does not move until something
        // else (e.g. a click) triggers a redraw that commits it.
        // We force a redraw to apply the new margin right away.
        update();
    }
}

void NoteWindow::applySize()
{
    // Collapsed: the surface is as tall as the title bar alone. m_note.h stays
    // the "expanded" height and is never overwritten by the collapsed state.
    const int h = m_collapsed ? kTitleHeight : m_note.h;
    if (m_layer)
        m_layer->setDesiredSize(QSize(m_note.w, h));
    resize(m_note.w, h);
}

void NoteWindow::clampToScreen()
{
    QRect area;
    for (const QScreen *s : QGuiApplication::screens())
        area = area.united(s->availableGeometry());
    if (area.isEmpty())
        return;
    const int keep = 60;   // how many pixels of the note always stay reachable
    m_note.x = qBound(area.left() - m_note.w + keep, m_note.x, area.right() - keep);
    m_note.y = qBound(area.top(), m_note.y, area.bottom() - keep);
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
    }
    setVisible(visible);
}

void NoteWindow::activate()
{
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
                      "#noteWindow { background-color: %1; }"
                      "#titleBar { background-color: %2; }"
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
    applyCollapsedVisuals();
    applySize();
    applyMargins();
    emit collapsedEdited(m_note.id, m_collapsed);
}

// ------------------------------------------------------------ interaction

void NoteWindow::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    const QSize sz = event->size();
    m_resizeGrip->move(sz.width() - kResizeGrip, sz.height() - kResizeGrip);
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

    if (!m_collapsed && m_resizeGrip->isVisible() && m_resizeGrip->geometry().contains(pos)) {
        m_resizing = true;
        m_resizeRefLocal = pos;
        m_resizeStartSize = QSize(m_note.w, m_note.h);
        event->accept();
        return;
    }

    if (m_titleBar->geometry().contains(pos)) {
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
            emit geometryEdited(m_note.id, m_note.x, m_note.y, m_note.w, m_note.h);
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
        emit geometryEdited(m_note.id, m_note.x, m_note.y, m_note.w, m_note.h);
    } else if (m_resizing) {
        m_resizing = false;
        emit geometryEdited(m_note.id, m_note.x, m_note.y, m_note.w, m_note.h);
    }
    QWidget::mouseReleaseEvent(event);
}

void NoteWindow::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton
        && m_titleBar->geometry().contains(event->position().toPoint())) {
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
    QTextCursor c = m_body->textCursor();
    if (!c.hasSelection())
        c.select(QTextCursor::Document);
    QTextCharFormat fmt;
    fmt.setFontWeight(QFont::Normal);
    fmt.setFontItalic(false);
    fmt.setFontUnderline(false);
    fmt.setFontStrikeOut(false);
    fmt.setForeground(QColor(m_theme.text));
    c.mergeCharFormat(fmt);
}

// -------------------------------------------------------------- menu

void NoteWindow::buildContextMenu(const QPoint &globalPos)
{
    QMenu menu;
    const QTextCharFormat cur = m_body->currentCharFormat();

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
    if (m_titleBar->geometry().contains(event->pos()))
        buildContextMenu(event->globalPos());
    else
        QWidget::contextMenuEvent(event);
}
