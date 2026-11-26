qt_add_executable(qmn-spike
    main.cpp)
target_link_libraries(qmn-spike PRIVATE qmn_view)

# The full demo app (PLAN follow-up, post-1.0 polish): tabbed main window
# + settings dialog over the M19 options object. The window and dialog
# compile into a small static lib so the offscreen test can drive the
# exact doors the menu slots call, without exec()ing any dialog.
qt_add_library(qmn_demo_app STATIC
    EditorSettings.cpp
    EditorSettingsDialog.cpp
    MainWindow.cpp
    ShowcaseTour.cpp)
target_link_libraries(qmn_demo_app PUBLIC qmn_view)
# The --lsp door (the M8 popup-trigger demo path): MainWindow is a CLIENT of
# the glue shelf (src/glue, a real layer since the G1 promotion): the demo
# links it PRIVATE, tst_demo never sees an lsp type through MainWindow.hpp,
# only the hub behind a unique_ptr.
target_link_libraries(qmn_demo_app PRIVATE qmn_glue)

qt_add_executable(qmn-demo
    demo_main.cpp)
target_link_libraries(qmn-demo PRIVATE qmn_demo_app)
# S1-4 (--tour): the CLI face's build-tree fixture GUESS needs the vendored
# python .scm at run time — a configure-time path define (the tests/ face
# hands tst_demo the very same path as QMN_TEST_PY_QUERY). Kept on the
# executable: ShowcaseTour itself takes every fixture through TourConfig.
target_compile_definitions(qmn-demo PRIVATE
    QMN_TOUR_PYQUERY_DEFAULT=\"${CMAKE_SOURCE_DIR}/third_party/tree-sitter/grammars/python/queries/highlights.scm\")
