add_library(qmn_view STATIC
    CodeEditor.hpp
    CodeEditor.cpp
    HighlightEngine.hpp
    HighlightEngine.cpp
    QueryData.hpp)

target_include_directories(qmn_view PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
# Vendored tree-sitter stays PRIVATE: the HighlightEngine seam keeps it out
# of the public view headers, so it never leaks into qmn_core's consumers
# (layering rule, PLAN.md D2).
target_link_libraries(qmn_view PUBLIC qmn_core Qt6::Widgets PRIVATE qmn_ts)
