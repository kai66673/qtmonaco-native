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
