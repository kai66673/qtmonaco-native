# Layer rule: QtCore ONLY. No Gui/Widgets here — the model must stay headless.
add_library(qmn_core STATIC
    TextBuffer.hpp
    TextBuffer.cpp)

target_include_directories(qmn_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(qmn_core PUBLIC Qt6::Core)
