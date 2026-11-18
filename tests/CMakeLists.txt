qt_add_executable(tst_textbuffer tst_textbuffer.cpp)
target_link_libraries(tst_textbuffer PRIVATE qmn_core Qt6::Test)
add_test(NAME textbuffer COMMAND tst_textbuffer)

# The 4th ctest entry (M10): headless property + golden tests over the
# pure line-diff engine (src/diff is QtCore-only, like core).
qt_add_executable(tst_diff tst_diff.cpp)
target_link_libraries(tst_diff PRIVATE qmn_diff Qt6::Test)
add_test(NAME diff COMMAND tst_diff)

# The 5th ctest entry (M8 machine 1, PLAN amended 2026-11-17): one binary
# that grows machine by machine — protocol framing/transports/RpcClient
# now, the QProcess fakeserver + LspClient tests with machine 2 into THIS
# binary (no further ctest entries inside M8). Machine 4 (the app-layer
# LspHub) makes it widget-carrying: the hub's doors take a real CodeEditor
# and its diagnostics feed lands in CodeEditor::setDiagnostics, so the
# binary links qmn_glue + qmn_view and runs offscreen (QTEST_MAIN below).
qt_add_executable(tst_lsp tst_lsp.cpp)
target_link_libraries(tst_lsp PRIVATE qmn_glue qmn_view qmn_lsp Qt6::Test)
add_dependencies(tst_lsp fakeserver) # the spawn target must exist at run time
add_test(NAME lsp COMMAND tst_lsp)
set_tests_properties(lsp PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# The LSP fakeserver (machine 2): a tests support binary, NOT a ctest entry.
# It speaks our own JsonFraming over stdio (stdout = PROTOCOL, stderr = the
# separate log/corpus-cannery channel), spawned by tst_lsp's factory through
# ProcessTransport; the path is injected here, the mode travels in argv.
qt_add_executable(fakeserver fakeserver/main.cpp)
target_link_libraries(fakeserver PRIVATE qmn_protocol Qt6::Core)
set_target_properties(fakeserver PROPERTIES WIN32_EXECUTABLE FALSE
                                            MACOSX_BUNDLE FALSE)
target_compile_definitions(tst_lsp PRIVATE
    QMN_TEST_LSP_SERVER=\"$<TARGET_FILE:fakeserver>\")

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
