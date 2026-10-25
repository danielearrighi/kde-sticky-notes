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
- Install for the current user: `./scripts/install.sh` (honors `PREFIX`, default
  `~/.local`). It expects an already-built `build/sticky-notes`.

## Architecture

Single target `sticky-notes` built from `src/` (`CMakeLists.txt`). Entry
`src/main.cpp` → `AppController` wires everything. The hub tab widgets live under
`src/tabs/`.

- `Storage` is a singleton over SQLite at `~/.local/share/StickyNotes/notes.db`;
  tables are created in `Storage::ensureSchema()`, which also runs a lightweight
  add-column migration (checks `PRAGMA table_info`, then `ALTER TABLE ... ADD
  COLUMN`) for columns added after the fact — there is no separate migration
  framework. There is no pre-existing file to run against — delete `notes.db` to
  reset state.
- `NoteWindow` = one layer-shell note (title bar, Markdown body, context menu,
  drag/resize). `HubWindow` = thin `QMainWindow` shell over the tab widgets in
  `src/tabs/` — `NotesTab` (grid), `TrashTab`, `SettingsTab` (preferences, themes
  and backup & restore), `InfoTab` — which own their own widgets and emit
  signals; `HubWindow` re-emits them so `AppController` is unchanged.
  Shared UI helpers (`plainSnippet`, `makeCardButton`, `swatchIcon`, `untitled`,
  date formatting) live in `UiHelpers`. `Themes` = builtin palette; `I18n` =
  runtime translations (symbolic keys, `resources/i18n/*.json`), selected by the
  `language` setting and applied at the next start.
- `Updater` (`src/Updater.{h,cpp}`) checks
  `https://api.github.com/repos/danielearrighi/kde-sticky-notes/releases/latest`,
  verifies the asset against the release `SHA256SUMS` and replaces the running
  file. `AppController` runs the daily automatic check (`update_check`,
  `update_last_check`, `update_skipped_version` settings; never on an
  `--autostart` launch) and routes the prompt to `InfoTab`, which also owns the
  manual button and the download progress. `STICKY_NOTES_UPDATE_FEED` overrides
  the API URL for local testing.
- Closing the hub (X or *Quit app*) quits the app by default: `HubWindow::closeEvent()`
  emits `quitRequested`, and `AppController::onQuitRequested()` decides what that
  means. With the **Close to system tray** setting the hub is only hidden instead
  and a `QSystemTrayIcon` (with *Show window* / *Quit app*) provides a way back.
  `setQuitOnLastWindowClosed(false)` is kept so hiding a note window never quits
  the app.
- Single instance via a `QLocalServer` named `sticky-notes-single-instance`: a
  second launch tells the running instance to show the hub, then exits 0.
  `main.cpp` recognizes `--restart` (skip the probe, used after a language change)
  and `--autostart` (written into the generated autostart `.desktop`): an
  autostart launch only probes and calls `AppController::start(true)`, which shows
  the notes but keeps the hub hidden (tray) or minimized.

## Conventions & traps

- **Prose is English**: code comments, docstrings, `README.md`, `CHANGELOG.md` and
  this file. **UI strings use `I18n::t("symbolic.key")`** with lowercase,
  dot-grouped keys (groups: generic like `open`/`hide`/`delete`/`restore`/`new_note`/
  `untitled`, then `app.*`, `notes.*`, `trash.*`, `note.*`, `format.*`,
  `settings.*`, `colors.*`). Values contain **no HTML**: bold/italic are applied in
  C++ with `QFont`. Translations live in `resources/i18n/*.json` (one flat
  symbolic-key → value map per language, English source/fallback in `en.json`,
  Italian in `it.json`, Spanish in `es.json`), embedded as Qt resources and loaded at runtime by `I18n`.
  `en.json` is **always installed as the base translator**; the selected language
  is installed on top, so a missing key falls back to English. Files under `docs/`
  stay Italian. To add a language: drop `resources/i18n/<code>.json` with a `_meta`
  `{code, name}` plus the translated values and rebuild.
- `Storage::bindText()` normalizes a **null** `QString` to `""`. Binding a default
  `QString` directly stores SQL `NULL` and violates `NOT NULL` on `notes.title`.
  Use/keep this helper for text columns.
- Layer-shell state (including margins) is double-buffered and
  `LayerShellQt::Window::setMargins()` does **not** commit.
  `NoteWindow::commitLayerSurface()` is the single place that forces a
  full-surface `wl_surface` commit (`update()`), used by `applyMargins()`,
  `applyEffectiveLayer()` and `setKeyboardInteractive()`. The full repaint stays
  cheap during a drag because `beginDragVisuals()` freezes the card into
  `m_dragSnapshot` and hides it on the first real movement, so `paintEvent()`
  draws the snapshot plus the shadow. Do not remove that snapshot: a body repaint
  on every movement makes the surface lag the pointer and drag positions explode.
  The snapshot is grabbed with the card's `QGraphicsOpacityEffect` temporarily
  removed: Qt renders the effect source in device coordinates, so grabbing a
  faded card returns the content shifted by its position and cut at the
  right/bottom. Hiding the card would also take the keyboard focus from the body:
  `eventFilter()` records whether the body had focus before the press moves it,
  and `endDragVisuals()` gives it back. Drag math must use the **local**
  coordinate incrementally with a fixed press reference (see
  `NoteWindow::mouseMoveEvent()`), never `globalPosition()` as an absolute base.
- The note minimum size comes from `kMinW`/`kMinH`, **never from the content**.
  A `QLayout` exports its minimum to the window by default, and a title `QLabel`
  reports the whole text width: a long title would raise the window's minimum
  width. The layer-shell desired size (`applySize()`) is then smaller than the
  size Qt allows, the surface keeps being reconfigured and the note flickers
  while it is resized below the title width. Hence the root layout is
  `SetNoConstraint`, the window has an explicit minimum and the title label is
  `Ignored` horizontally and elided by `elideTitle()`.
- Notes are layer-shell surfaces with keyboard interactivity **None** unless the
  user interacts with them: `NoteWindow::requestKeyboardFocus()` enables
  OnDemand on a mouse press or `activate()`, and `onFocusLayerTimeout()` resets
  it to None when focus is lost. Keeping OnDemand always set makes KWin treat
  every note as a focus candidate and hand it the keyboard as soon as no normal
  window has focus (e.g. the last window is minimized): the caret starts
  blinking and the note is briefly raised to the top layer. A bottom-layer note
  must be raised *before* OnDemand is enabled, because KWin activates only
  top/overlay on-demand surfaces when their interactivity changes
  (`LayerShellV1Window::handleAcceptsFocusChanged()`).
- Do not reference /docs files since they are not committed to the repository.
- **Self-update restart paths**: an AppImage must be restarted through
  `$APPIMAGE` (the file the runtime exported), never through
  `QCoreApplication::applicationFilePath()`, which points inside
  `/tmp/.mount_*/usr/bin/` and disappears with the process. A tarball install
  replaces `~/.local/bin/sticky-notes` and restarts that file. The replacement
  is atomic: the new file is downloaded/extracted next to the target, checked
  against `SHA256SUMS`, the old one is renamed to `<target>.old` and removed on
  the next start. Development builds and `/usr/bin` packages are reported as
  unsupported (not writable): never attempt a self-update there.
- `scripts/create_release.sh` must upload `SHA256SUMS` with every release: the
  updater refuses to install an asset whose hash cannot be verified.
- **The AppImage bundles the C library** (`scripts/build_appimage.sh`): it is
  built on a rolling release (Tumbleweed, glibc 2.44) while AppImageHub tests on
  the oldest Ubuntu LTS still in standard support (glibc 2.35). `AppRun` is a
  script that execs the bundled `ld-linux` with `--library-path`, and
  `libstdc++`/`libgcc_s` are bundled as well because the build GCC is newer than
  the target's. Do not let linuxdeploy's symlink replace `AppRun` and do not
  drop the copy step: without it the AppImage is rejected as "glibc too new".

## Git

- **Never commit unless the user has explicitly asked you to commit.** A request
  to implement, fix or test something is **not** permission to commit: do the
  work, show the result, and wait. Do not commit, amend, revert or tag on your
  own initiative.
- The same applies to everything that publishes or rewrites state outside the
  working tree: `git push`, tags, releases, and comments or reviews on GitHub.
  Do it only when the user asks for that specific action.

## Notes for agents (context budget)

- Locate symbols with `grep`/pattern search and open only the files you actually
  need instead of reading whole sources: every file you read stays in the
  conversation and bloats the context window on each following request.
- The hub is split per tab: open/attach only the tab you are working on
  (`src/tabs/NotesTab`, `TrashTab`, `SettingsTab` or `InfoTab`)
  instead of `HubWindow.cpp`, which is now a thin shell.
- Do not take screenshot or start a infinite set of test (creating test app versions) 
  tring to test/fix something all by yourself. If something is not clear, analyze it,
  try a possibile fix and ask the user to test for you. 