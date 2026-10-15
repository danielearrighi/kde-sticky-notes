# Changelog

## Unreleased

### Added
- **Close to system tray** (settings option, off by default): when enabled,
  closing the hub (X or *Quit app*) keeps Sticky Notes running and shows a
  `QSystemTrayIcon` with *Show window* and *Quit app*. If the tray is not
  available, closing quits as before.
- **Internationalization**: the UI uses **symbolic keys**
  (`I18n::t("settings.title")`) with **English** as the source/fallback
  (`en.json`) and **Italian** included as a translation (`resources/i18n/*.json`,
  embedded as Qt resources and loaded at runtime). `en.json` is always installed
  as the base translator and the selected language on top of it, so a missing key
  falls back to English. Translation values contain no HTML (bold/italic are
  applied in C++). The settings have a **language selector** (*System* + the
  available languages) and a **Restart now** button: the choice is applied at the
  next start. Built-in theme names are translated at display time.
- **Restore theme**: the theme editor has a **Restore** (*Ripristina*) button that
  puts the selected built-in theme back to its factory colors (and name), undoing
  the user's edits; for custom themes it reloads the last saved state.
- **Focused note in the foreground**: the note that has keyboard focus stays in
  front of normal windows even when it is set *on the desktop* (*bottom* layer).
  When focus moves elsewhere it returns to the configured layer, after a short
  deferred check (the context menu is a popup that takes focus instead of the
  note: while it is open the note stays in front).
- **Remembered scroll position**: each note stores its body scroll position (new
  `scroll` column on `notes`, with the usual add-column migration) and restores
  it when the app is reopened. The value is not tracked incrementally: it is
  saved when the note is hidden and when the app quits.

### Changed
- **Autostart at login**: the generated `~/.config/autostart/sticky-notes.desktop`
  now launches the app with `--autostart`, so at login only the notes appear on
  the desktop and the hub stays out of the way — in the system tray when *Close
  to system tray* is enabled, otherwise minimized to the taskbar (a normal
  launch, or relaunching the app, still opens the hub as before).
- **Themes**: removed the concept of **border**. The border is no longer
  configurable in the theme editor nor saved to the database: notes no longer
  have the 1 px outline (which made the body appear ~2 px narrower than the
  title bar) and the hub cards are flat. The `border_color` column was removed
  from the `themes` schema; as the database is still in the design phase it is
  recreated (no migration).
- **Defaults** for a new database: **Random** theme, **on the desktop** position
  (*bottom* layer), **230×190 px** size, **DejaVu Sans** font (if available) at
  **9 pt**.
- **Rounded notes with a drop shadow**: notes now have 3 px rounded corners and a
  small soft shadow. The window is translucent and paints its background by hand
  (`NoteWindow::paintEvent`): the layer-shell surface is ~7 px larger than the
  note on every side to leave room for the shadow, and `applyMargins()`/`applySize()`
  compensate so the note keeps its saved position and size.

### Fixed
- **Note body bottom padding**: when the text reached the end of the note,
  `QTextEdit` scrolled the document's bottom margin out of view to keep the
  caret visible, so the last line touched the bottom border. The body now has a
  bottom padding equal to the text margin used at the top, applied through the
  stylesheet so it survives scrolling while editing.

## 0.1.0

First working version (KDE Plasma / Wayland).

### Added
- Desktop notes as **layer-shell surfaces**: no decorations, outside the taskbar
  and Alt+Tab, position chosen by the app.
- Draggable title bar (only from there) with smooth 1:1 dragging; title editable
  with a double click.
- Resizing from the bottom-right corner.
- Markdown formatting from the context menu: bold, italic, underline,
  strikethrough, bulleted and numbered lists, clear formatting.
- Themes/colors per note (post-it palette) and the *Always on top* option.
- **Hub** with the note grid, trash (restore/delete/empty) and settings (default
  theme and position, font, autostart, delete confirmation, theme editor with
  color picker).
- Persistence on **SQLite** (`~/.local/share/StickyNotes/notes.db`).
- **Single instance** and optional **autostart**.
- App icon and a **user install** script (`packaging/install.sh`).

### Verified on
- KDE Plasma 6 (KWin 6.7) / Wayland, Qt 6.11, LayerShellQt 6.7, 2560×1440 screen.

### Fixed
- **Creating new notes**: an empty *null* `QString` was bound as `NULL` in SQLite
  and violated the `NOT NULL` constraint on `notes.title`, making *New note*
  fail. Strings are now normalized to a non-null empty string
  (`Storage::bindText`).
- **Dragging**: `applyMargins()` set the layer-surface margin but **did not force
  the commit** of the `wl_surface`. The margin is double-buffered and
  `LayerShellQt::Window::setMargins()` does not commit: during the drag the widget
  does not redraw, the surface stays still, the local coordinate keeps growing
  (it is the *total* displacement since the start) and with the fixed press
  reference the delta adds up again on every event → position that "explodes"
  (`x=26884`…). Now `applyMargins()` calls `update()`: QtWayland translates it
  into a commit, the surface moves on every event and dragging is smooth **1:1**.
- **Dragging (guards)**: if the button release event was lost, the drag stayed
  active; now the button no longer being pressed is detected and the drag is
  terminated. Moreover the position is always **clamped to the screens area**
  (also on load, to repair already corrupted data).
- **Settings**: while loading into the hub the widget signals overwrote transient
  values; they are now blocked with `QSignalBlocker`.

### Known limitations
- KDE/Wayland only with layer-shell support (GNOME and X11 excluded).
- Multi-monitor not handled yet (anchored to the top-left edge).
- Order among notes of the same layer not controllable.
- AppImage/Flatpak packaging not present yet.
