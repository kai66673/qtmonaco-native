# Shiboken6/PySide6 extension for CodeEditor (PLAN.md: the Phase B "thin spine"
# spike, now grown into the frozen Gate 1 binding surface of Phase F). Only
# reached when the root option QMN_BUILD_PYTHON=ON, so the C++-only build and CI
# stay Python-free.
#
# Everything here is written against facts read out of the installed 6.12.0
# wheels (see AGENTS.md):
#  * shiboken_generator_create_binding() comes from find_package(Shiboken6Tools);
#    its command is Shiboken6::shiboken6 (an IMPORTED executable pointing at
#    site-packages/shiboken6_generator/shiboken6), it hardcodes
#    --output-directory=${CMAKE_CURRENT_BINARY_DIR}, -I/-T
#    ${CMAKE_CURRENT_SOURCE_DIR} and -T${PYSIDE_TYPESYSTEMS}, creates a MODULE
#    library with PREFIX "" and OUTPUT_NAME <target>${SHIBOKEN_PYTHON_EXTENSION_SUFFIX}
#    (= ".abi3"), and it does NOT compute GENERATED_SOURCES for us.
#  * Generated file names, as produced by an actual run for module _qmn with the
#    classes qmn::view::CodeEditor / qmn::view::TextPosition.
#  * PySide6::pyside6 and Shiboken6::libshiboken carry include dirs only; Qt
#    itself comes from a real Qt install (Qt6Config.cmake is not in the wheel).

find_package(Python 3.10 REQUIRED COMPONENTS Interpreter Development.Module)

# The CMake packages we need live inside the interpreter's site-packages, under
# <site-packages>/<pkg>/lib/cmake/<Package>. Deriving the site-packages path from
# the interpreter we are actually building for keeps this self-configuring.
execute_process(
    COMMAND "${Python_EXECUTABLE}" -c
        "import sysconfig; print(sysconfig.get_paths()['purelib'])"
    OUTPUT_VARIABLE QMN_SITE_PACKAGE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
foreach(_wheel PySide6 shiboken6 shiboken6_generator)
    list(APPEND CMAKE_PREFIX_PATH "${QMN_SITE_PACKAGE}/${_wheel}")
endforeach()

# Qt for headers, moc and link libraries: QMN_QT_PREFIX wins, else the QTDIR the
# container image exports (bindings/Dockerfile).
set(QMN_QT_PREFIX "" CACHE PATH "Qt 6 install used for headers, moc and linking")
if(QMN_QT_PREFIX)
    list(APPEND CMAKE_PREFIX_PATH "${QMN_QT_PREFIX}")
elseif(DEFINED ENV{QTDIR})
    list(APPEND CMAKE_PREFIX_PATH "$ENV{QTDIR}")
endif()

find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui Widgets)
find_package(Shiboken6 REQUIRED)
find_package(Shiboken6Tools REQUIRED)
find_package(PySide6 REQUIRED)

# -T${PYSIDE_TYPESYSTEMS} (needed to resolve load-typesystem typesystem_widgets.
# xml) is only set by the abi3 flavour of the PySide6 config, so pin it to the
# wheel's copy unless the config already provided a valid one.
if(NOT DEFINED PYSIDE_TYPESYSTEMS OR NOT EXISTS "${PYSIDE_TYPESYSTEMS}/typesystem_widgets.xml")
    set(PYSIDE_TYPESYSTEMS "${QMN_SITE_PACKAGE}/PySide6/typesystems")
endif()

# Names emitted by the generator (verified by running it, see
# bindings/build_in_container.sh --codegen). Output lands in a subdirectory
# named after the typesystem package, i.e. _qmn/. One TU per bound type: the two
# QObjects, the free TextPosition struct, and one per nested record struct
# (HighlightSpan/Diagnostic/InlayHint/MatchRange/FoldRange). The nested ENUMS
# (RevealMode, LineNumbersMode, ...) get no file of their own - since Shiboken
# 6.8 smart enums are emitted inside the owning class's TU.
set(_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/_qmn")
set(_generated
    "${_gen_dir}/_qmn_module_wrapper.cpp"
    "${_gen_dir}/qmn_wrapper.cpp"
    "${_gen_dir}/qmn_view_wrapper.cpp"
    "${_gen_dir}/qmn_view_codeeditor_wrapper.cpp"
    "${_gen_dir}/qmn_view_codeeditoroptions_wrapper.cpp"
    "${_gen_dir}/qmn_view_textposition_wrapper.cpp"
    "${_gen_dir}/qmn_view_codeeditor_highlightspan_wrapper.cpp"
    "${_gen_dir}/qmn_view_codeeditor_diagnostic_wrapper.cpp"
    "${_gen_dir}/qmn_view_codeeditor_inlayhint_wrapper.cpp"
    "${_gen_dir}/qmn_view_codeeditor_matchrange_wrapper.cpp"
    "${_gen_dir}/qmn_view_codeeditor_foldrange_wrapper.cpp")

# The macro forwards -I only for the listed QT_MODULES, and Qt6::Widgets'
# interface carries [include, include/QtWidgets]; CodeEditor.hpp itself includes
# <QElapsedTimer>/<QString>/<QPointF>, so Core and Gui dirs are added too.
# Each -I must be its own option, so build one "-I<dir>" entry per directory.
set(_qmn_qt_incs "")
foreach(_mod Core Gui Widgets)
    get_target_property(_incs Qt6::${_mod} INTERFACE_INCLUDE_DIRECTORIES)
    foreach(_dir ${_incs})
        list(APPEND _qmn_qt_incs "-I${_dir}")
    endforeach()
endforeach()

shiboken_generator_create_binding(
    EXTENSION_TARGET  _qmn
    LIBRARY_TARGET    qmn_view          # src/view, PUBLIC include dir = src/
    TYPESYSTEM_FILE   "${CMAKE_CURRENT_SOURCE_DIR}/typesystem_qmn.xml"
    # Both headers are handed to the parser: CodeEditorOptions.hpp is reachable
    # through CodeEditor.hpp's include, but naming it explicitly keeps the
    # generated set independent of that include.
    HEADERS           "${PROJECT_SOURCE_DIR}/src/view/CodeEditor.hpp"
                      "${PROJECT_SOURCE_DIR}/src/view/CodeEditorOptions.hpp"
    GENERATED_SOURCES ${_generated}
    QT_MODULES        Widgets
    SHIBOKEN_EXTRA_OPTIONS
        # Generated code includes <view/CodeEditor.hpp>, and the typesystem's
        # <include file-name> is resolved the same way.
        -I${PROJECT_SOURCE_DIR}/src
        -I${QMN_SITE_PACKAGE}/PySide6/include/PySide6
        ${_qmn_qt_incs}
)

target_include_directories(_qmn PRIVATE "${_gen_dir}")
# The macro adds <PySide6 include>/QtWidgets (the one QT_MODULES entry), but the
# generated _qmn_python.h chain includes pyside6_qtwidgets_python.h, which pulls
# <pyside6_qtgui_python.h> and <pyside6_qtcore_python.h> by name from their own
# module subdirectories.
target_include_directories(_qmn PRIVATE
    "${QMN_SITE_PACKAGE}/PySide6/include/PySide6/QtCore"
    "${QMN_SITE_PACKAGE}/PySide6/include/PySide6/QtGui"
)
set_target_properties(_qmn PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

# The wheel layout (pyproject.toml wheel.packages): the Python package qmn/ is
# shipped from bindings/python/qmn, and the extension module sits next to that
# package at site-packages root - `import _qmn` searches sys.path, not the qmn/
# subdirectory. The generated import path is _qmn.qmn.view.CodeEditor; the
# package flattens it to qmn.CodeEditor.
install(TARGETS _qmn LIBRARY DESTINATION .)
