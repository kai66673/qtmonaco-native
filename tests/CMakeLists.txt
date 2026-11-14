qt_add_executable(tst_textbuffer tst_textbuffer.cpp)
target_link_libraries(tst_textbuffer PRIVATE qmn_core Qt6::Test)
add_test(NAME textbuffer COMMAND tst_textbuffer)

qt_add_executable(tst_codeeditor tst_codeeditor.cpp)
target_link_libraries(tst_codeeditor PRIVATE qmn_view Qt6::Test)
# The test drives the HighlightEngine seam, whose header includes the
# vendored <tree_sitter/api.h>; qmn_view's qmn_ts link is PRIVATE by the
# layering rule and never propagates. Without this the include falls
# through to Arch's SYSTEM libtree-sitter headers (measured: the host build
# passed for exactly that accidental reason), and fails to configure/find in
# clean environments - the wheel build builds tests too. Pinned 27.2 include:
# the same seam note as in tools/bench_highlight.cpp.
target_include_directories(tst_codeeditor PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../third_party/tree-sitter/runtime/include)
add_test(NAME codeeditor COMMAND tst_codeeditor)
set_tests_properties(codeeditor PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Drives the qmn-demo app surface (MainWindow + dialog) through the same
# programmatic doors its menu slots call - no QFileDialog/QDialog exec,
# so it runs fully offscreen.
qt_add_executable(tst_demo tst_demo.cpp)
target_link_libraries(tst_demo PRIVATE qmn_demo_app Qt6::Test)
add_test(NAME demo COMMAND tst_demo)
set_tests_properties(demo PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
