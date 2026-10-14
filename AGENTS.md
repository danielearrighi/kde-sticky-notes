# AGENTS.md

Desktop sticky notes for **KDE Plasma on Wayland** only. Single Qt6/C++17
executable; no libraries, tests, CI, lint, or formatter config in this repo.

## Build & run

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/sticky-notes # must run in a Wayland session
```

- `find_package(LayerShellQt REQUIRED)` — configure fails without it. openSUSE:
  `zypper install layer-shell-qt6-devel qt6-base-devel qt6-wayland-devel cmake ninja gcc-c++`
- Force the Wayland QPA and logging when launching from a script/agent:
  `QT_QPA_PLATFORM=wayland QT_FORCE_STDERR_LOGGING=1 ./build/sticky-notes`
  (Qt suppresses logs when stderr is not a terminal).
- The app cannot be exercised on X11, GNOME, or headless: layer-shell is
  KWin (Plasma ≥ 6.6) / wlroots only. There is no test suite to substitute.
- Install for the current user: `./packaging/install.sh` (honors `PREFIX`, default
  `~/.local`). It expects an already-built `build/sticky-notes`.

## Architecture

Single target `sticky-notes` built from `src/` (`CMakeLists.txt`). Entry
`src/main.cpp` → `AppController` wires everything.

- `Storage` is a singleton over SQLite at `~/.local/share/StickyNotes/notes.db`;
  tables are created in `Storage::ensureSchema()`, which also runs a lightweight
  add-column migration (checks `PRAGMA table_info`, then `ALTER TABLE ... ADD
  COLUMN`) for columns added after the fact — there is no separate migration
  framework. There is no pre-existing file to run against — delete `notes.db` to
  reset state.
- `NoteWindow` = one layer-shell note (title bar, Markdown body, context menu,
  drag/resize). `HubWindow` = thin `QMainWindow` shell over three tab widgets —
  `NotesTab` (grid), `TrashTab`, `SettingsTab` — which own their own widgets and
  emit signals; `HubWindow` re-emits them so `AppController` is unchanged.
  Shared UI helpers (`plainSnippet`, `makeCardButton`, `swatchIcon`, `untitled`,
  date formatting) live in `UiHelpers`. `Themes` = builtin palette; `I18n` =
  runtime translations (symbolic keys, `resources/i18n/*.json`), selected by the
  `language` setting and applied at the next start.
- Closing the hub (X or *Quit app*) quits the app by default: `HubWindow::closeEvent()`
  emits `quitRequested`, and `AppController::onQuitRequested()` decides what that
  means. With the **Close to system tray** setting the hub is only hidden instead
  and a `QSystemTrayIcon` (with *Show window* / *Quit app*) provides a way back.
  `setQuitOnLastWindowClosed(false)` is kept so hiding a note window never quits
  the app.
- Single instance via a `QLocalServer` named `sticky-notes-single-instance`: a
  second launch tells the running instance to show the hub, then exits 0.

## Conventions & traps

- **Prose is English**: code comments, docstrings, `README.md`, `CHANGELOG.md` and
  this file. **UI strings use `I18n::t("symbolic.key")`** with lowercase,
  dot-grouped keys (groups: generic like `open`/`hide`/`delete`/`restore`/`new_note`/
  `untitled`, then `app.*`, `notes.*`, `trash.*`, `note.*`, `format.*`,
  `settings.*`, `colors.*`). Values contain **no HTML**: bold/italic are applied in
  C++ with `QFont`. Translations live in `resources/i18n/*.json` (one flat
  symbolic-key → value map per language, English source/fallback in `en.json`,
  Italian in `it.json`), embedded as Qt resources and loaded at runtime by `I18n`.
  `en.json` is **always installed as the base translator**; the selected language
  is installed on top, so a missing key falls back to English. Files under `docs/`
  stay Italian. To add a language: drop `resources/i18n/<code>.json` with a `_meta`
  `{code, name}` plus the translated values and rebuild.
- `Storage::bindText()` normalizes a **null** `QString` to `""`. Binding a default
  `QString` directly stores SQL `NULL` and violates `NOT NULL` on `notes.title`.
  Use/keep this helper for text columns.
- Layer-shell state (including margins) is double-buffered and
  `LayerShellQt::Window::setMargins()` does **not** commit. `NoteWindow::applyMargins()`
  calls `update()` to force a `wl_surface` commit. Remove that and drag positions
  explode. Drag math must use the **local** coordinate incrementally with a fixed
  press reference (see `NoteWindow::mouseMoveEvent()`), never `globalPosition()`
  as an absolute base.
- Do not reference /docs files since they are not committed to the repository.

## Notes for agents (context budget)

- Locate symbols with `grep`/pattern search and open only the files you actually
  need instead of reading whole sources: every file you read stays in the
  conversation and bloats the context window on each following request.
- The hub is split per tab: open/attach only the tab you are working on
  (`NotesTab`, `TrashTab` or `SettingsTab`) instead of `HubWindow.cpp`, which is
  now a thin shell.
