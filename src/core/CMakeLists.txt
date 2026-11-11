# Layer rule: QtCore ONLY. No Gui/Widgets here — the model must stay headless.
add_library(qmn_core STATIC
    TextBuffer.hpp
    TextBuffer.cpp)

target_include_directories(qmn_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/qmn>)
target_link_libraries(qmn_core PUBLIC Qt6::Core)

# Public headers are relative to src/ ("core/TextBuffer.hpp"), so the package
# installs the src-relative layout under <prefix>/include/qmn.
set_target_properties(qmn_core PROPERTIES EXPORT_NAME core)
install(TARGETS qmn_core EXPORT qmnTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES TextBuffer.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/qmn/core)
