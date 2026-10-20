#pragma once

#include "Models.h"

#include <QWidget>
#include <QPixmap>
#include <QPoint>
#include <QSize>

namespace LayerShellQt { class Window; }

class QImage;
class QPainter;
class QLabel;
class QLineEdit;
class QTextEdit;
class QTimer;
class QToolButton;

/// A single desktop note, implemented as a layer-shell surface.
///
/// - no system decorations
/// - outside the taskbar and Alt+Tab (by layer-shell definition)
/// - draggable ONLY from the title bar
/// - resizable from the bottom-right corner
class NoteWindow : public QWidget
{
    Q_OBJECT
public:
    explicit NoteWindow(const Note &note, QWidget *parent = nullptr);
    ~NoteWindow() override;

    qint64 noteId() const { return m_note.id; }
    const Note &note() const { return m_note; }

    void applyTheme(const QString &themeId);
    void applyLayer(const QString &layer);
    void setVisibleState(bool visible);
    /// The layer the note is configured to live on ("top" or "bottom"), not the
    /// layer it may be raised to temporarily while focused: the menu entry
    /// toggles this persistent setting.
    bool isAlwaysOnTop() const { return m_note.layer == QLatin1String("top"); }

    /// Brings the note back into focus and to the front.
    void activate();

    /// Pulls the note back onto a connected screen when its saved position falls
    /// outside every current screen (e.g. the monitor it lived on was unplugged)
    /// and persists the new geometry. A no-op when the position is already fine.
    void ensureOnScreen();

    /// Persists the current body scroll position (emits scrollEdited). Called
    /// when the note is hidden or the app quits: the position is not tracked
    /// incrementally (see NoteWindow.cpp).
    void persistScroll();

    /// Saves the body now if a debounced save is still pending. Idempotent:
    /// called from the destructor, on quit and before a backup restore, so no
    /// typed text is lost.
    void flushPendingSave();

signals:
    void contentEdited(qint64 id, const QString &title, const QString &content);
    void geometryEdited(qint64 id, int x, int y, int w, int h, const QString &screenKey);
    void themeEdited(qint64 id, const QString &themeId);
    void layerEdited(qint64 id, const QString &layer);
    void collapsedEdited(qint64 id, bool collapsed);
    void scrollEdited(qint64 id, int scroll);
    void opacityEdited(qint64 id, int opacity);
    void deleteRequested(qint64 id);
    void hideRequested(qint64 id);
    void newNoteRequested();
    void duplicateRequested(qint64 id);
    void settingsRequested();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    bool event(QEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void configureLayerShell();
    void applyMargins();
    void applySize();
    void clampToScreen();

    /// Commits the whole wl_surface, applying the double-buffered layer-shell
    /// state (margins, layer, keyboard interactivity). During a drag the card is
    /// frozen into m_dragSnapshot, so the full repaint is a blit plus the
    /// shadow (see beginDragVisuals()).
    void commitLayerSurface();
    /// Freezes the card into m_dragSnapshot and hides it, so the full-surface
    /// commit forced on every drag movement does not repaint the body.
    void beginDragVisuals();
    /// Drops the snapshot and shows the card again (end of drag).
    void endDragVisuals();

    /// Places the note on a connected screen and updates m_screenKey /
    /// m_screenRect. With \a proportional a note that ends up on a different
    /// screen keeps its relative position (or its saved per-screen position),
    /// fully visible; otherwise it is clamped into the screen it is already on
    /// (allowing the usual sliver overhang used while dragging). \a forceEmit
    /// re-emits geometryEdited even when nothing moved (used for the first
    /// placement, to record the per-screen position).
    void placeOnScreens(bool proportional, bool forceEmit);
    void applyStyle();
    void applyOpacity();

    /// Paints the rounded note background and the title strip in the card.
    void paintCard(QPainter &p);

    /// True when \a pos (in NoteWindow coordinates) is over the title bar.
    bool titleBarContains(const QPoint &pos) const;

    void refreshTitleLabel();
    void beginTitleEdit();
    void commitTitleEdit();
    void scheduleSaveContent();
    /// Persists the body immediately (used by the debounced m_saveTimer).
    void saveContent();
    void buildContextMenu(const QPoint &globalPos);

    /// Inserts \a image at the cursor as a PNG data URL (see the note about
    /// pasted images in NoteWindow.cpp). Returns false when the image is empty
    /// or cannot be encoded.
    bool insertImage(const QImage &image);

    /// Restores the display size of every image of the body: one image pixel
    /// drawn as one device pixel (sharp on scaled displays) and never wider
    /// than the note. Markdown carries no image size, so this runs after
    /// setMarkdown() (and is harmless on a document without images).
    void applyImageSizing();

    /// Re-applies the saved scroll position once the layout is ready. A no-op
    /// while the note is collapsed (the body is hidden and its scrollbar range
    /// is not meaningful yet); it runs again when the note is expanded.
    void restoreScroll();

    /// Applies the *effective* layer: as long as the note has focus it stays in
    /// front of normal windows (top layer), even when configured as a desktop
    /// note (bottom layer). On focus loss it returns to the configured layer.
    void applyEffectiveLayer();

    /// Switches the surface between KeyboardInteractivityOnDemand (true) and
    /// KeyboardInteractivityNone (false). A note is only focusable while the
    /// user is interacting with it; see the comment in configureLayerShell().
    void setKeyboardInteractive(bool interactive);

    /// Enables keyboard interactivity and asks the compositor to activate the
    /// note (used by activate() and by a mouse press on an unfocused note).
    void requestKeyboardFocus();

    /// Deferred check (m_focusLayerTimer) that focus is really lost.
    void onFocusLayerTimeout();

    /// Collapses the note to the title bar alone / restores it.
    void toggleCollapse();

    /// Applies the body/grip visibility and the button appearance based on
    /// m_collapsed (without resizing or persisting).
    void applyCollapsedVisuals();

    // formatting
    void pastePlainText();
    void toggleBold();
    void toggleItalic();
    void toggleUnderline();
    void toggleStrike();
    void makeList(bool numbered);
    void clearFormatting();

    Note m_note;
    Theme m_theme;

    LayerShellQt::Window *m_layer = nullptr;
    QWidget *m_card = nullptr;
    /// Card content frozen while dragging (see beginDragVisuals()). Null when
    /// not dragging.
    QPixmap m_dragSnapshot;
    /// Whether the body had keyboard focus when the drag started: hiding the
    /// card would drop it, so it is restored on release.
    bool m_dragRestoreBodyFocus = false;
    QWidget *m_titleBar = nullptr;
    QLabel *m_titleLabel = nullptr;
    QLineEdit *m_titleEdit = nullptr;
    QToolButton *m_collapseButton = nullptr;
    QToolButton *m_closeButton = nullptr;
    QTextEdit *m_body = nullptr;
    QLabel *m_resizeGrip = nullptr;
    QTimer *m_saveTimer = nullptr;
    QTimer *m_focusLayerTimer = nullptr;

    bool m_collapsed = false;      // only the title bar visible
    bool m_raisedForFocus = false; // in front because it has focus

    bool m_dragging = false;
    QPoint m_dragRefLocal;
    bool m_resizing = false;
    QPoint m_resizeRefLocal;
    QSize m_resizeStartSize;
    bool m_loading = false;

    /// Key of the screen the note is currently on ("<W>x<H>|<connector>", built
    /// by screenKeyOf()).
    QString m_screenKey;
    /// Available geometry (global) of the screen the note is on. Kept even after
    /// that screen disappears, as the source of the proportional mapping.
    QRect m_screenRect;
    /// Offset of the available area inside the output (availableGeometry minus
    /// geometry): normally (0,0), non-zero with a top/left panel. Added back when
    /// setting the layer-shell margins.
    QPoint m_screenOrigin;
    /// False until the first placement has been persisted (see placeOnScreens).
    bool m_placed = false;
};
