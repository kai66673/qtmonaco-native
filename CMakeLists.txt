cmake_minimum_required(VERSION 3.22)

project(qtmonaco-native
    VERSION 1.5.0
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

# The extension is a shared MODULE that links our STATIC archives in, so in a
# binding build every archive has to be position independent. Measured in the
# RHEL9 binding container: without this the link of _qmn.abi3.so dies on
# "relocation R_X86_64_32S against `.rodata.str1.1' ... recompile with -fPIC"
# (tree-sitter + query_data + qmn_core/qmn_view objects). The Arch host hid the
# same gap because its GCC is configured --enable-default-pie, so the archives
# were already PIC there. Scoped to the binding build on purpose: a plain C++
# build (CI, the benchmark) keeps non-PIC code, which is what the per-keystroke
# perf budgets were measured with.
if(QMN_BUILD_PYTHON)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

find_package(Qt6 6.6 REQUIRED COMPONENTS Core Gui Widgets Test)

qt_standard_project_setup()

# Layering rules (see README):
#   core  -> QtCore only (headless, fully unit-testable)
#   view  -> core + QtWidgets + (PRIVATE) vendored tree-sitter
#   app   -> view only (demo/spike harness)
add_subdirectory(third_party/tree-sitter)
add_subdirectory(src)

# Needs qmn_view, so it must come after src/.
if(QMN_BUILD_PYTHON)
    add_subdirectory(bindings)
endif()

enable_testing()
add_subdirectory(tests)

# ---- C++ packaging (PLAN.md Phase F): make "usable from C++" real. ----------
# One export set, namespaced qmn::core / qmn::view (EXPORT_NAMEs set in the
# layer CMakeLists) plus qmn::internal_tree_sitter: qmn_view links the vendored
# tree-sitter PRIVATE, and a STATIC private dependency is dropped from the
# export (undefined symbols for consumers), so the archive ships in the package
# and qmn_view re-references it through $<INSTALL_INTERFACE:...> — see
# src/view/CMakeLists.txt. Internal: consumers never link it themselves.
#
# SOVERSION note (deviation from PLAN's "bump SOVERSION" freeze line): the C++
# package ships STATIC archives — the wheel's _qmn module links qmn_view
# statically and a static package sidesteps symbol-visibility/Qt-plugin
# surgery; SOVERSION is a no-op for .a, so the freeze carries the package
# VERSION only. Deliberate, documented.

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set_target_properties(qmn_ts PROPERTIES EXPORT_NAME internal_tree_sitter)
install(TARGETS qmn_ts EXPORT qmnTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(EXPORT qmnTargets
    NAMESPACE qmn::
    FILE qmnTargets.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qmn)

configure_package_config_file(
    "${CMAKE_CURRENT_LIST_DIR}/cmake/qmnConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/qmnConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/qmn")
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/qmnConfigVersion.cmake"
    COMPATIBILITY SameMajorVersion)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/qmnConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/qmnConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qmn)
