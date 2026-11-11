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

## Release builds

Every build script takes `--arch x86_64|aarch64` (default `uname -m`) and
`scripts/build_all.sh` builds **both** (host architecture first, override with
`--archs`). Release artifacts are therefore eight:

- `release/sticky-notes-<version>-<arch>.{tar.gz,AppImage,flatpak}` for x86_64
  and aarch64;
- `release/sticky-notes_<version>-1_amd64.deb` and
  `release/sticky-notes-<version>-1.x86_64.rpm` for **x86_64 only** (the
  Debian/RPM packages use the standard file naming, not the `<arch>` suffix of
  the other artifacts).

A "foreign" architecture (not the host one) is emulated with QEMU user mode, so
it needs a `binfmt_misc` entry: `qemu-linux-user` on openSUSE,
`qemu-user-static` on Fedora/Debian/Ubuntu. Each container image and each Flatpak
SDK is per architecture: `localhost/sticky-notes-build:ubuntu26.04-<arch>`,
`localhost/sticky-notes-rpmbuild:fedora44-<arch>` and `org.kde.Sdk//6.10`
installed with `--arch`. The scripts qualify unqualified `--image` tags with
`localhost/` and run containers with `--pull=never`, so podman never prompts for
an unqualified short name or pulls a different image from a configured registry.

- `scripts/build_release.sh` builds the tarball binary with
  `scripts/build_portable.sh` inside `scripts/container/ubuntu26.04.Dockerfile`
  (Qt 6.10, LayerShellQt 6.6) and writes
  `build-release-<arch>/sticky-notes`. That baseline is not cosmetic: the
  tarball does not bundle Qt, and Qt is forward compatible only. A binary built
  on the rolling host (Qt 6.12) fails on Fedora 44 (Qt 6.11.2) with
  `version 'Qt_6.12' not found`. `--native` exists for local testing only (host
  architecture only); never publish a tarball built that way.
- `scripts/build_portable.sh` gets the architecture from `--arch`, tags the
  container image with it and passes `--platform` (`linux/amd64`,
  `linux/arm64`) only for a foreign architecture. The Qt 6.10 tripwire at the
  end reads the ELF symbols with `readelf`, which works on a foreign binary.
- `scripts/build_flatpak.sh` builds
  `release/sticky-notes-<version>-<arch>.flatpak` from
  `packaging/io.github.danielearrighi.StickyNotes.yml` against
  `org.kde.Platform`/`org.kde.Sdk` **6.10** (Qt 6.10.3), and compiles
  **LayerShellQt 6.6 from source** because `org.kde.Platform` does not ship it.
  The runtime is a Flatpak dependency (downloaded on install), so the host Qt
  is irrelevant: the same bundle runs on Fedora 44, Ubuntu, Debian, etc. The
  build needs `org.kde.Sdk//6.10` **for the target architecture**
  (`flatpak install --arch=aarch64 flathub org.kde.Sdk//6.10`); if
  `flatpak-builder` is not installed the script falls back to
  `flatpak run --command=flatpak-builder org.flatpak.Builder`
  (with `FLATPAK_USER_DIR` pointed at the real user installation, otherwise the
  host flatpak looks for the SDK in the Builder's own `~/.var/app` data). The
  build tree is `build-flatpak-<arch>/` and `--install` only applies to the
  host architecture.
- `scripts/build_deb.sh` packages the portable binary into
  `release/sticky-notes_<version>-1_<debarch>.deb` inside the Ubuntu 26.04 image
  (`amd64` for x86_64). `dpkg-shlibdeps` generates the versioned dependencies
  from the binary's ELF symbols, but its LayerShellQt shlibs entry only reports
  the ABI baseline (6.4.90) while the app uses APIs added in 6.6, so the script
  pins `liblayershellqtinterface6 (>= 6.6)` explicitly. It also adds
  `qt6-wayland` and `libqt6sql6-sqlite`: the QPA and SQLite plugins are loaded
  at runtime, so shlibdeps cannot see them. The package installs the binary, the
  desktop entry, every hicolor icon size, the metainfo (launchable patched to
  `sticky-notes.desktop`) and Debian copyright/changelog files. It targets
  Ubuntu 26.04+ and Debian forky/sid+; an older Qt is refused by the versioned
  dependency instead of being installed and failing at startup.
- `scripts/build_rpm.sh` builds `release/sticky-notes-<version>-1.<rpmarch>.rpm`
  with `rpmbuild` inside `scripts/container/fedora44.Dockerfile`. There is no
  compilation: the portable binary is staged into a source tarball and
  `packaging/sticky-notes.spec` only installs the files. Fedora and openSUSE
  naming differences are covered by rich dependencies
  (`(qt6-qtbase >= 6.10 or libQt6Core6 >= 6.10)`,
  `(layer-shell-qt >= 6.6 or layer-shell-qt6 >= 6.6)` and
  `(qt6-sql-sqlite or qt6-qtbase)` for the QSQLITE driver); the rest, including
  the `Qt_6.10` symbol requirement, comes from rpm's automatic ELF dependency
  generator. Do not turn the SQLite dependency into the plugin file path:
  dnf/zypper do not always load the filelists metadata and the install then
  fails with "nothing provides *.so". `%global __os_install_post %{nil}` skips
  the brp strip/debuginfo steps (the binary is already built and may belong to
  the foreign architecture).
- `scripts/build_all.sh` is a convenience wrapper that runs the five builds
  in canonical order (tarball, AppImage, Flatpak, deb, rpm) for every
  architecture and prints a summary with the artifact sizes. When the tarball is
  selected it runs first and its portable binary is passed to the AppImage, deb
  and rpm scripts with `--binary`, so they do not compile the same sources
  again; the Flatpak keeps its own build inside the SDK. The .deb and .rpm are
  skipped for aarch64 (they are published for x86_64 only). It only forwards
  options (`--native`, `--skip-build`, `--install`, `--keep-appdir`) and can
  select targets with `--only`/`--skip` and architectures with `--archs`; with
  no options it builds x86_64 + aarch64, so QEMU must be available. Each script
  stays the one to use for a single artifact and keeps its own requirements.
- `scripts/build_appimage.sh` builds both architectures by default inside the
  Ubuntu 26.04 container, in `build-appimage-<arch>/` (`APPIMAGE_EXTRACT_AND_RUN=1`,
  no FUSE inside), against the same Qt 6.10/glibc baseline. linuxdeploy, the Qt
  plugin and appimagetool are architecture-specific binaries that must run next
  to the Qt they deploy, so the container is used for the native architecture
  too; a foreign one additionally needs QEMU user emulation. A type-2 AppImage
  header does not match the qemu binfmt ELF magic, so in an emulated container
  the script mounts the host's static qemu interpreter and runs the tools through
  it (linuxdeploy's Qt plugin is exposed as wrapper scripts for the same reason).
  `--binary PATH` reuses an existing binary (used by `build_all.sh`); `--native`
  builds on the host in `build-appimage-native-<arch>/` for local testing only
  and may require a newer Qt/glibc. The KDE platform theme deployment is skipped
  when the environment has no `KDEPlasmaPlatformTheme6.so` (the container
  installs it when the package is available), so an AppImage may fall back to
  the Qt default style.
- The tarball `install.sh` (`scripts/install-release.sh`) distinguishes a
  missing library from a too-old one (`ldd` prints `version '…' not found`):
  the latter cannot be fixed by installing packages, so it refuses to install
  and points at the AppImage.
- The AppImage ships `KDEPlasmaPlatformTheme6.so` but not the KIO workers
  (`plugins/kf6/kio`, e.g. `kio_file.so`), which linuxdeploy does not deploy.
  That theme builds the native KDE file dialog on `KFileWidget`/KIO, so inside
  the AppImage the dialog would open, show the sidebar and list **no file at
  all**. Every `QFileDialog` call therefore passes
  `UiHelpers::fileDialogOptions()`, which selects `DontUseNativeDialog` when
  the AppImage runtime exported `APPIMAGE`/`APPDIR`; the Qt dialog lists the
  filesystem directly and still takes the Plasma palette from the theme.

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
  Shared UI helpers (`plainSnippet`, `makeCardButton`, `swatchIcon`,
  `trayIcon`, `untitled`, date formatting) live in `UiHelpers`. `Themes` = builtin
  palette; `I18n` = runtime translations (symbolic keys, `resources/i18n/*.json`),
  selected by the `language` setting and applied at the next start.
- The tray icon is resolved by `UiHelpers::trayIcon()` from the `tray_icon`
  (`auto`, `app`, `custom`) and `tray_icon_path` settings. `auto` tints the
  bundled `sticky-notes-symbolic.svg` with the theme foreground. The icon is a
  single 64 px pixmap forced to DPR 1: the QSystemTrayIcon D-Bus exporter
  publishes one entry, so a multi-size icon can end up advertising the smallest
  (16 px) and the panel draws it tiny; inheriting the application DPR is equally
  wrong (128 px "logical" entries on HiDPI). The glyph is deliberately solid: a
  thin outline reads smaller than the themed icons next to it at tray size.
- `Updater` (`src/Updater.{h,cpp}`) checks
  `https://api.github.com/repos/danielearrighi/kde-sticky-notes/releases/latest`,
  verifies the asset against the release `SHA256SUMS` and replaces the running
  file. `AppController` runs the daily automatic check (`update_check`,
  `update_last_check`, `update_skipped_version` settings; never on an
  `--autostart` launch) and routes the prompt to `InfoTab`, which also owns the
  manual button and the download progress. `STICKY_NOTES_UPDATE_FEED` overrides
  the API URL for local testing.
- The Flatpak (`packaging/io.github.danielearrighi.StickyNotes.yml`,
  `src/Platform.h`) is detected through `/.flatpak-info`. `/app` is read-only,
  so `Updater` reports `Unsupported` and `AppController` skips the daily check;
  a manual check still runs and `InfoTab` points at `flatpak update`. Flatpak
  sets `XDG_DATA_HOME` inside the sandbox, so the DB is
  `~/.var/app/<id>/data/StickyNotes/notes.db`; the autostart entry must instead
  be written to the real host `~/.config/autostart` (the manifest grants
  `--filesystem=xdg-config/autostart:create`) with
  `Exec=flatpak run <id> --autostart`. The manifest `post-install` renames the
  desktop file and icon to the app id, and `main.cpp` sets that id as the
  desktop file name. `QFileDialog` stays native: unlike the AppImage, the KDE
  runtime ships the KIO workers. The manifest must keep `--share=network`
  (Flatpak does not grant it by default, and the update check then fails with a
  DNS error) and installs the icon in the standard hicolor sizes (16-256).
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
  the next start. Development builds and `/usr/bin` packages (the .deb/.rpm,
  updated by apt/dnf/zypper) are reported as unsupported (not writable): never
  attempt a self-update there.
- `scripts/create_release.sh` uploads every artifact found in `release/` for
  both architectures, the x86_64-only `.deb`/`.rpm`, plus one `SHA256SUMS` with
  every release: the updater refuses to install an asset whose hash cannot be
  verified (it looks the file up by name, so a single checksum file covers all
  architectures), and the Flatpak asset is one of the supported installations. A
  missing architecture is a warning, not an error.
- The Flatpak `metainfo.xml` under `packaging/` carries its own `<releases>`
  entry: bump it together with `project(... VERSION)` in `CMakeLists.txt`.
- **The AppImage bundles the C library** (`scripts/build_appimage.sh`): it is
  built in Ubuntu 26.04 (glibc 2.43) while AppImageHub tests on the oldest Ubuntu
  LTS still in standard support (glibc 2.35). `AppRun` is a script that execs the
  bundled `ld-linux` with `--library-path`, and `libstdc++`/`libgcc_s` are
  bundled as well because the build GCC is newer than the target's. Do not let
  linuxdeploy's symlink replace `AppRun` and do not drop the copy step: without
  it the AppImage is rejected as "glibc too new".

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