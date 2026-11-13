# SPDX-License-Identifier: GPL-3.0-or-later
#
# RPM spec for Sticky Notes. It packages the portable binary built by
# scripts/build_portable.sh; scripts/build_rpm.sh passes pkg_version and sn_date
# and runs rpmbuild inside the Fedora 44 container. Do not set the version here:
# it comes from project(... VERSION) in CMakeLists.txt.
#
# The package targets x86_64 Fedora and openSUSE (only x86_64 is published).
# The binary is linked against Qt 6.8 and LayerShellQt 6.3.4 (the Debian 13
# baseline every artifact shares): any distribution with those or newer works.
# The versioned dependencies reject an older Qt before installing a binary that
# would fail to start.

%global __os_install_post %{nil}
%global debug_package %{nil}
%{!?sn_date:%global sn_date Thu Jan 01 1970}

Name:           sticky-notes
Version:        %{pkg_version}
Release:        1
Summary:        Desktop sticky notes for KDE Plasma (Wayland)
License:        GPL-3.0-or-later
URL:            https://github.com/danielearrighi/kde-sticky-notes
Source0:        %{name}-%{version}.tar.gz

# Qt 6.8 and LayerShellQt 6.3.4 are the minimums. The alternatives cover the
# Fedora names (qt6-qtbase, layer-shell-qt) and the openSUSE ones
# (libQt6Core6, layer-shell-qt6).
Requires:       (qt6-qtbase >= 6.8 or libQt6Core6 >= 6.8)
Requires:       (layer-shell-qt >= 6.3.4 or layer-shell-qt6 >= 6.3.4)
# Storage needs the QSQLITE driver, loaded at runtime: a separate package on
# openSUSE (qt6-sql-sqlite), inside qt6-qtbase on Fedora. A package dependency
# instead of a file path: rpm does not always load filelists metadata.
Requires:       (qt6-sql-sqlite or qt6-qtbase)
# The Wayland QPA plugin is in qt6-qtbase-gui on Fedora and in libQt6Gui6 on
# openSUSE, both pulled in by the automatic libQt6Gui soname dependency.

%description
Sticky Notes places colored notes directly on the KDE Plasma desktop as Wayland
layer-shell surfaces: they stay under (or above) normal windows and out of the
taskbar and of Alt+Tab.

Each note supports Markdown formatting, a per-note theme, opacity, the layer and
the size. A hub window manages the notes (grid, trash, settings, backup and
restore), the system tray and the start at login.

The package uses the system Qt6 and LayerShellQt; it does not bundle them.

%prep
%setup -q

%install
install -Dpm755 sticky-notes %{buildroot}%{_bindir}/sticky-notes
install -Dpm644 sticky-notes.desktop \
    %{buildroot}%{_datadir}/applications/sticky-notes.desktop
install -Dpm644 sticky-notes.metainfo.xml \
    %{buildroot}%{_datadir}/metainfo/io.github.danielearrighi.StickyNotes.metainfo.xml
for size in 16 22 32 48 64 128 256; do
    install -Dpm644 "icons/sticky-notes-${size}.png" \
        "%{buildroot}%{_datadir}/icons/hicolor/${size}x${size}/apps/sticky-notes.png"
done

%files
%{_bindir}/sticky-notes
%{_datadir}/applications/sticky-notes.desktop
%{_datadir}/metainfo/io.github.danielearrighi.StickyNotes.metainfo.xml
%{_datadir}/icons/hicolor/*/apps/sticky-notes.png

%changelog
* %{sn_date} Daniele Arrighi <daniele.arrighi@gmail.com> - %{version}-1
- Release %{version}.
