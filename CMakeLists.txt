cmake_minimum_required(VERSION 3.20)

project(sticky-notes VERSION 0.1.9 LANGUAGES CXX)

# Version.h is generated from the project() VERSION above: bump that single line
# to release a new version. The header is what the app reads at build time
# (main.cpp, InfoTab.cpp).
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/generated)
configure_file(src/Version.h.in ${CMAKE_BINARY_DIR}/generated/Version.h @ONLY)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Generate build/compile_commands.json: used by editors (VS Code/CLion/clangd)
# to resolve Qt #includes in IntelliSense.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Sql Network)
find_package(LayerShellQt REQUIRED)

qt_add_executable(sticky-notes
    src/main.cpp
    src/Models.h
    src/Storage.h         src/Storage.cpp
    src/Themes.h          src/Themes.cpp
    src/I18n.h            src/I18n.cpp
    src/NoteWindow.h      src/NoteWindow.cpp
    src/UiHelpers.h       src/UiHelpers.cpp
    src/tabs/NotesTab.h        src/tabs/NotesTab.cpp
    src/tabs/TrashTab.h        src/tabs/TrashTab.cpp
    src/tabs/SettingsTab.h     src/tabs/SettingsTab.cpp
    src/tabs/InfoTab.h         src/tabs/InfoTab.cpp
    src/HubWindow.h       src/HubWindow.cpp
    src/AppController.h   src/AppController.cpp
)

# The tab widgets live under src/tabs/: add both roots so the quoted includes
# stay unqualified ("I18n.h" from a tab, "NotesTab.h" from the hub).
target_include_directories(sticky-notes PRIVATE src src/tabs ${CMAKE_BINARY_DIR}/generated)

# Translations are one JSON file per language, embedded as Qt resources. The
# files may not exist yet: an empty glob is fine and CONFIGURE_DEPENDS makes a
# later build pick them up.
file(GLOB I18N_JSON CONFIGURE_DEPENDS "resources/i18n/*.json")

qt_add_resources(sticky-notes "app_resources"
    PREFIX "/"
    BASE resources
    FILES resources/icons/sticky-notes.png
          resources/icons/sticky-notes-symbolic.svg
          ${I18N_JSON}
)

target_link_libraries(sticky-notes PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Sql
    Qt6::Network
    LayerShellQt::Interface
)

install(TARGETS sticky-notes RUNTIME DESTINATION bin)
install(FILES packaging/sticky-notes.desktop DESTINATION share/applications)
