# Layer rule (PLAN.md GATE 2 / M8, AGENTS.md): QtCore + QtNetwork, NEVER
# view/core. The protocol layers never know CodeEditor/CodeDocument; LSP
# glue lives in the app layer. Tested by tst_lsp (the 5th ctest entry).
add_library(qmn_protocol STATIC
    JsonFraming.hpp
    JsonFraming.cpp
    Transport.hpp
    Transport.cpp
    ProcessTransport.hpp
    ProcessTransport.cpp
    TcpTransport.hpp
    TcpTransport.cpp
    RpcClient.hpp
    RpcClient.cpp)

target_include_directories(qmn_protocol PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/qmn>)
target_link_libraries(qmn_protocol PUBLIC Qt6::Core Qt6::Network)

# Packaging: a PUBLIC export set (qmn::protocol), unlike the internal
# qmn::internal_tree_sitter / qmn::internal_diff mechanism — the M8 glue in
# src/app links it directly, and it is the planned home of car 4's DAP
# envelope reuse. Headers install src-relative like every other layer.
set_target_properties(qmn_protocol PROPERTIES EXPORT_NAME protocol)
install(TARGETS qmn_protocol EXPORT qmnTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES
    JsonFraming.hpp
    Transport.hpp
    ProcessTransport.hpp
    TcpTransport.hpp
    RpcClient.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/qmn/protocol)
