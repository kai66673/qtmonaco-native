cmake_minimum_required(VERSION 3.22)

project(qtmonaco-native
    VERSION 0.1.0
    DESCRIPTION "Native code editor widget for Qt6, built from scratch on a raw QWidget"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The Shiboken6/PySide6 extension is opt-in: it needs the container environment
# from bindings/Dockerfile (the pip Shiboken generator cannot run on an Arch
# host, see AGENTS.md). Default OFF keeps the C++ spike and CI Python-free.
option(QMN_BUILD_PYTHON "Build the Shiboken6 Python extension module" OFF)

find_package(Qt6 6.6 REQUIRED COMPONENTS Core Gui Widgets Test)

qt_standard_project_setup()

# Layering rules (see README):
#   core  -> QtCore only (headless, fully unit-testable)
#   view  -> core + QtWidgets (the raw QWidget canvas)
#   app   -> view only (demo/spike harness)
add_subdirectory(src)

# Needs qmn_view, so it must come after src/.
if(QMN_BUILD_PYTHON)
    add_subdirectory(bindings)
endif()

enable_testing()
add_subdirectory(tests)
