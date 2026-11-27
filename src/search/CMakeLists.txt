# Layer rule (PLAN.md GATE 4 car I1, AGENTS.md): QtCore ONLY — the search
# engine is headless, fuzzable and benchmarkable like core/diff (its ctest
# entry tst_search is guiless). No view, no widgets, ever.
add_library(qmn_search STATIC
    SearchEngine.hpp
    SearchEngine.cpp)

target_include_directories(qmn_search PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/qmn>)
target_link_libraries(qmn_search PUBLIC Qt6::Core)

# A REAL export set (unlike internal_diff / internal_tree_sitter): the
# export mechanism is a LINK-only mirror only when the shipped library
# links it PRIVATE; here the ENGINE is a top-level shelf member the app
# links directly — the same shape as qmn::core / qmn::glue. The header
# installs with it (search/SearchEngine.hpp), so an external consumer
# can drive the engine door-to-door without the IDE (examples/consumer
# may grow a search leg later; the wrappability law is why a future
# wheel crossing is a three-copies entry, not a surgery).
set_target_properties(qmn_search PROPERTIES EXPORT_NAME search)
install(TARGETS qmn_search EXPORT qmnTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES SearchEngine.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/qmn/search)
