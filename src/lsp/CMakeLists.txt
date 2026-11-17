# Layer rule (PLAN.md GATE 2 / M8, AGENTS.md): depends ONLY on src/protocol
# — never view, never core. The LSP client lifecycle; its UI glue lives in
# the app layer (M4+). Tested by tst_lsp (the 5th ctest entry, grows with
# this car machine by machine).
add_library(qmn_lsp STATIC
    LspClient.hpp
    LspClient.cpp)

target_include_directories(qmn_lsp PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/qmn>)
target_link_libraries(qmn_lsp PUBLIC qmn_protocol)

# Packaging: a PUBLIC export set (qmn::lsp), same pattern as qmn::protocol —
# the app-layer glue links it directly (PLAN: lsp glue lives in src/app).
set_target_properties(qmn_lsp PROPERTIES EXPORT_NAME lsp)
install(TARGETS qmn_lsp EXPORT qmnTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES LspClient.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/qmn/lsp)
