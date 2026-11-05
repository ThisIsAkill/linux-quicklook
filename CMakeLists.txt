cmake_minimum_required(VERSION 3.22)
project(quicklook VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets DBus Multimedia MultimediaWidgets Concurrent)
find_package(KF6KIO REQUIRED)
find_package(KF6Service REQUIRED)
find_package(KF6SyntaxHighlighting REQUIRED)
find_package(LayerShellQt REQUIRED)
find_package(PkgConfig REQUIRED)
# GLib headers (pulled in by AT-SPI) use "signals" as an identifier, hence
# QT_NO_KEYWORDS below.
if(PROJECT_IS_TOP_LEVEL)
    pkg_check_modules(ATSPI REQUIRED IMPORTED_TARGET atspi-2 gobject-2.0)
endif()
pkg_check_modules(POPPLER REQUIRED IMPORTED_TARGET poppler-qt6)
pkg_check_modules(ARCHIVE REQUIRED IMPORTED_TARGET libarchive)

# The preview panel and viewers, also used by the Lumen launcher.
add_library(quicklook-preview STATIC
    src/chrome.cpp
    src/panel.cpp
    src/viewers.cpp
)
target_include_directories(quicklook-preview PUBLIC src)
target_compile_definitions(quicklook-preview PUBLIC QT_NO_KEYWORDS)
target_link_libraries(quicklook-preview PUBLIC
    Qt6::Widgets Qt6::Multimedia Qt6::MultimediaWidgets Qt6::Concurrent
    KF6::KIOGui KF6::Service KF6::SyntaxHighlighting LayerShellQt::Interface
    PkgConfig::POPPLER PkgConfig::ARCHIVE)

# The Dolphin daemon and command-line previewer. Skipped when another
# project includes this one only for the library.
if(PROJECT_IS_TOP_LEVEL)
    add_executable(quicklook
        src/main.cpp
        src/focustracker.cpp
        src/globalshortcut.cpp
    )
    target_link_libraries(quicklook PRIVATE quicklook-preview Qt6::DBus PkgConfig::ATSPI)
    install(TARGETS quicklook RUNTIME DESTINATION bin)
endif()
