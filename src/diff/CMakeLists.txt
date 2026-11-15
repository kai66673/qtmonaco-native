# Layer rule (PLAN.md GATE 2 / M10, AGENTS.md): QtCore ONLY — pure line
# diff + alignment table, headless like core (tested by tst_diff).
add_library(qmn_diff STATIC
    LineDiff.hpp
    LineDiff.cpp)

target_include_directories(qmn_diff PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/qmn>)
target_link_libraries(qmn_diff PUBLIC Qt6::Core)

# Packaging (M10 layering decision 2026-11-16): internal, link-only
# target mirroring qmn::internal_tree_sitter — qmn_view PRIVATE-links
# qmn_diff (M2), and CMake drops a PRIVATE archive from a STATIC export,
# so the installed qmn_view re-names this target through its
# $<INSTALL_INTERFACE:...> line. Consumers never link it themselves and
# LineDiff.hpp is NOT installed (same seam policy as HighlightEngine.hpp).
set_target_properties(qmn_diff PROPERTIES EXPORT_NAME internal_diff)
install(TARGETS qmn_diff EXPORT qmnTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
