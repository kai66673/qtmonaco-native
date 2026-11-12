add_library(qmn_view STATIC
    CodeEditor.hpp
    CodeEditor.cpp
    CodeEditorOptions.hpp
    CodeEditorOptions.cpp
    CodeEditorTheme.hpp
    CodeEditorTheme.cpp
    LanguageProperties.hpp
    LanguageProperties.cpp
    CodeEditorAccessible.hpp
    CodeEditorAccessible.cpp
    HighlightEngine.hpp
    HighlightEngine.cpp
    QueryData.hpp)

target_include_directories(qmn_view PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/qmn>)
# Vendored tree-sitter stays PRIVATE: the HighlightEngine seam keeps it out
# of the public view headers, so it never leaks into qmn_core's consumers
# (layering rule, PLAN.md D2). The second line is the STATIC-private-dep
# export fix: CMake drops a PRIVATE archive from the export of another
# STATIC lib, so the installed qmn_view must name the (installed, internal)
# qmn_ts target again — $<INSTALL_INTERFACE:...> evaluates only inside the
# exported package, where qmn::internal_tree_sitter is defined.
target_link_libraries(qmn_view
    PUBLIC qmn_core Qt6::Widgets $<INSTALL_INTERFACE:qmn::internal_tree_sitter>
    PRIVATE qmn_ts)

# Public-header review (PLAN.md Phase F): the install allow-list is exactly
# the consumer-facing surface. HighlightEngine.hpp/QueryData.hpp (the
# tree-sitter seam) and CodeEditorAccessible.hpp (registered automatically
# from the CodeEditor ctor; screen readers talk to Qt's own QAccessible
# API, consumers never need the class) are deliberately NOT installed.
set_target_properties(qmn_view PROPERTIES EXPORT_NAME view)
install(TARGETS qmn_view EXPORT qmnTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES
    CodeEditor.hpp
    CodeEditorOptions.hpp
    CodeEditorTheme.hpp
    LanguageProperties.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/qmn/view)
