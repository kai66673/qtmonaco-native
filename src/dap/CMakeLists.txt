# Layer rule (PLAN car 4, AGENTS.md): src/dap depends ONLY on src/protocol
# — never view, never core, never the lsp layer (DapClient and LspClient
# are siblings sharing the RpcClient + EnvelopeCodec mechanism; the iide
# "duplicate the RPC for DAP" anti-pattern is what the D1 seam exists to
# prevent). Machine D2: the DAP envelope codec + the adapter client
# lifecycle. Tested by tst_dap (the 7th ctest entry, grows with this car).
add_library(qmn_dap STATIC
    DapCodec.hpp
    DapCodec.cpp
    DapClient.hpp
    DapClient.cpp)

target_include_directories(qmn_dap PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/qmn>)
target_link_libraries(qmn_dap PUBLIC qmn_protocol)

# Packaging: a PUBLIC export set (qmn::dap), same pattern as qmn::protocol
# and qmn::lsp — D3's glue shelf links it directly (the DAP glue lands on
# the glue shelf, the layer rule's other half).
set_target_properties(qmn_dap PROPERTIES EXPORT_NAME dap)
install(TARGETS qmn_dap EXPORT qmnTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES DapCodec.hpp DapClient.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/qmn/dap)
