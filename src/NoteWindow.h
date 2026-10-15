#pragma once

#include "Models.h"

#include <QWidget>
#include <QPoint>
#include <QSize>

namespace LayerShellQt { class Window; }

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
    bool isAlwaysOnTop() const { return m_note.layer == QLatin1String("top"); }

    /// Brings the note back into focus and to the front.
    void activate();

signals:
    void contentEdited(qint64 id, const QString &title, const QString &content);
    void geometryEdited(qint64 id, int x, int y, int w, int h);
    void themeEdited(qint64 id, const QString &themeId);
    void layerEdited(qint64 id, const QString &layer);
    void collapsedEdited(qint64 id, bool collapsed);
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
    void applyStyle();
    void refreshTitleLabel();
    void beginTitleEdit();
    void commitTitleEdit();
    void scheduleSaveContent();
    void buildContextMenu(const QPoint &globalPos);

    /// Applies the *effective* layer: as long as the note has focus it stays in
    /// front of normal windows (top layer), even when configured as a desktop
    /// note (bottom layer). On focus loss it returns to the configured layer.
    void applyEffectiveLayer();

    /// Deferred check (m_focusLayerTimer) that focus is really lost.
    void onFocusLayerTimeout();

    /// Collapses the note to the title bar alone / restores it.
    void toggleCollapse();

    /// Applies the body/grip visibility and the button appearance based on
    /// m_collapsed (without resizing or persisting).
    void applyCollapsedVisuals();

    // formatting
    void toggleBold();
    void toggleItalic();
    void toggleUnderline();
    void toggleStrike();
    void makeList(bool numbered);
    void clearFormatting();

    Note m_note;
    Theme m_theme;

    LayerShellQt::Window *m_layer = nullptr;
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
};
