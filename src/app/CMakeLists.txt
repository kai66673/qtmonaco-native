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
    MainWindow.cpp)
target_link_libraries(qmn_demo_app PUBLIC qmn_view)

qt_add_executable(qmn-demo
    demo_main.cpp)
target_link_libraries(qmn-demo PRIVATE qmn_demo_app)

# M8 machine 4: the app-layer LSP glue (PLAN: the LSP/DAP glue lives HERE —
# the view never learns about protocol/lsp, this lib is the client where
# they meet). App code, NOT an export set (same status as qmn_demo_app);
# tst_lsp links it to drive the hub headless through the fakeserver.
add_library(qmn_glue STATIC
    glue/LspHub.hpp
    glue/LspHub.cpp)
target_include_directories(qmn_glue PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>)
target_link_libraries(qmn_glue PUBLIC qmn_view qmn_lsp)
