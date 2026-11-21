qt_add_executable(tst_textbuffer tst_textbuffer.cpp)
target_link_libraries(tst_textbuffer PRIVATE qmn_core Qt6::Test)
add_test(NAME textbuffer COMMAND tst_textbuffer)

# The grammar fixtures further down compile C (bogus.c + the vendored
# parser.c recompiled SHARED): third_party/tree-sitter enable_language(C)s
# INSIDE its own directory scope, and a subdirectory's language-rule
# variables (CMAKE_C_COMPILE_OBJECT) do not reach sibling subdirs - the
# re-enable here (idempotent) is what lets those SHARED C fixtures
# configure. The C compiler was already a hard requirement via qmn_ts.
enable_language(C)

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

# The 6th ctest entry (G2 of the glue shelf, PLAN "the glue shelf +
# ProjectSession"): the document session over REAL CodeEditors. Offscreen
# (the editors are real widgets); NO server ever spawns — the claimed-
# server machinery is tst_lsp's fakeserver turf, clangd is not a CI
# guarantee. Links qmn_glue (its PUBLIC interface carries qmn_view).
qt_add_executable(tst_glue tst_glue.cpp)
target_link_libraries(tst_glue PRIVATE qmn_glue Qt6::Test)
add_test(NAME glue COMMAND tst_glue)
set_tests_properties(glue PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# The 7th ctest entry (PLAN car 4, machine D1): the EnvelopeCodec seam —
# same grow-machine-by-machine pattern as tst_lsp (D2 src/dap + DapClient,
# D3 the DapHub glue (flips the main to QTEST_MAIN/offscreen), D4 the demo
# door). D1 needed NO src/dap code: the second envelope was a test-local
# codec; the byte-identity half of the tripwire is the untouched tst_lsp
# binary passing green. Since machine D2 this binary also links qmn_dap
# (the production DapCodec + DapClient) and spawns the dapfake adapter.
qt_add_executable(tst_dap tst_dap.cpp)
target_link_libraries(tst_dap PRIVATE qmn_glue qmn_dap qmn_protocol Qt6::Test)
add_dependencies(tst_dap dapfake) # the spawn target must exist at run time
add_test(NAME dap COMMAND tst_dap)
set_tests_properties(dap PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# The DAP fake adapter (machine D2): a tests support binary, NOT a ctest
# entry — the dapfake sibling of the fakeserver (same stdio JsonFraming
# both wires: LSP and DAP share the Content-Length framing, only the
# envelope differs — which is the whole D1 thesis in one build line).
qt_add_executable(dapfake dapfake/main.cpp)
target_link_libraries(dapfake PRIVATE qmn_protocol Qt6::Core)
set_target_properties(dapfake PROPERTIES WIN32_EXECUTABLE FALSE
                                         MACOSX_BUNDLE FALSE)
target_compile_definitions(tst_dap PRIVATE
    QMN_TEST_DAP_SERVER=\"$<TARGET_FILE:dapfake>\")

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

# Grammar-registry fixtures (post-1.8.0 grammar-extensibility sliver),
# loaded at RUN time by path - neither is a ctest entry, and neither
# links the tree-sitter runtime: generated parsers are self-contained C.
# 1) qmn_fakegrammar: exports tree_sitter_bogus returning a ZEROED
#    TSLanguage-sized buffer (version 0, outside the runtime's 13-15
#    range) - the door's scratch-parser validation must refuse it with
#    false. The OUTPUT name keeps the loader's symbol-derivation
#    (libbogus.so -> tree_sitter_bogus) on the happy naming path.
# 2) qmn_pygrammar: the vendored python parser.c + scanner.c compiled a
#    SECOND time as a shared library - a language pointer DISTINCT from
#    qmn_view's statically linked copy, for the real loadGrammar +
#    live language-swap path. Named away from "python" on purpose: the
#    test passes the exported symbol explicitly (also pinning that
#    door). Warning-free like the third_party parsers.
add_library(qmn_fakegrammar SHARED fakegrammar/bogus.c)
set_target_properties(qmn_fakegrammar PROPERTIES OUTPUT_NAME "bogus") # libbogus.so
add_library(qmn_pygrammar SHARED
    ../third_party/tree-sitter/grammars/python/src/parser.c
    ../third_party/tree-sitter/grammars/python/src/scanner.c)
target_include_directories(qmn_pygrammar PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../third_party/tree-sitter/runtime/include
    ${CMAKE_CURRENT_LIST_DIR}/../third_party/tree-sitter/grammars/python/src)
set_target_properties(qmn_pygrammar PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
target_compile_options(qmn_fakegrammar PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-w>)
target_compile_options(qmn_pygrammar PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-w>)
target_compile_definitions(tst_codeeditor PRIVATE
    QMN_TEST_FAKE_GRAMMAR=\"$<TARGET_FILE:qmn_fakegrammar>\"
    QMN_TEST_PY_GRAMMAR=\"$<TARGET_FILE:qmn_pygrammar>\")
add_dependencies(tst_codeeditor qmn_fakegrammar qmn_pygrammar)

add_test(NAME codeeditor COMMAND tst_codeeditor)
set_tests_properties(codeeditor PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Drives the qmn-demo app surface (MainWindow + dialog) through the same
# programmatic doors its menu slots call - no QFileDialog/QDialog exec,
# so it runs fully offscreen.
qt_add_executable(tst_demo tst_demo.cpp)
target_link_libraries(tst_demo PRIVATE qmn_demo_app Qt6::Test)
add_test(NAME demo COMMAND tst_demo)
set_tests_properties(demo PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
