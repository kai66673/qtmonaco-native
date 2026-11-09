qt_add_executable(tst_textbuffer tst_textbuffer.cpp)
target_link_libraries(tst_textbuffer PRIVATE qmn_core Qt6::Test)
add_test(NAME textbuffer COMMAND tst_textbuffer)

qt_add_executable(tst_codeeditor tst_codeeditor.cpp)
target_link_libraries(tst_codeeditor PRIVATE qmn_view Qt6::Test)
add_test(NAME codeeditor COMMAND tst_codeeditor)
set_tests_properties(codeeditor PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
