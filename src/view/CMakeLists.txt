add_library(qmn_view STATIC
    CodeEditor.hpp
    CodeEditor.cpp)

target_include_directories(qmn_view PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(qmn_view PUBLIC qmn_core Qt6::Widgets)
