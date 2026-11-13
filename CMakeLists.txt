cmake_minimum_required(VERSION 3.20)

project(sticky-notes VERSION 0.3.7 LANGUAGES CXX)

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
    src/Platform.h
    src/NoteWindow.h      src/NoteWindow.cpp
    src/UiHelpers.h       src/UiHelpers.cpp
    src/Updater.h         src/Updater.cpp
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

# LayerShellQt 6.3 (Debian 13) predates two APIs used by the notes:
# setDesiredSize() arrived in 6.4 and setActivateOnShow() in 6.5. Compile the
# calls out on older versions: resize() already propagates the surface size on
# 6.3, and activation-on-show does not exist there (and is irrelevant while the
# keyboard interactivity is None).
if(NOT LayerShellQt_VERSION)
    message(FATAL_ERROR "LayerShellQt_VERSION is not set: cannot detect the available APIs")
endif()
if(LayerShellQt_VERSION VERSION_GREATER_EQUAL 6.4)
    target_compile_definitions(sticky-notes PRIVATE SN_HAS_LSQ_DESIRED_SIZE)
endif()
if(LayerShellQt_VERSION VERSION_GREATER_EQUAL 6.5)
    target_compile_definitions(sticky-notes PRIVATE SN_HAS_LSQ_ACTIVATE_ON_SHOW)
endif()

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
install(FILES scripts/sticky-notes.desktop DESTINATION share/applications)
