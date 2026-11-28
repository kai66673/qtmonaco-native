# Layer rule (PLAN.md GATE 4 car I2, AGENTS.md): QtCore ONLY — the fuzzy
# scorer is headless, property-testable and pure like core/diff/search
# (its ctest entry tst_fuzzy is guiless). No view, no widgets, ever.
add_library(qmn_fuzzy STATIC
    FuzzyMatcher.hpp
    FuzzyMatcher.cpp)

target_include_directories(qmn_fuzzy PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/qmn>)
target_link_libraries(qmn_fuzzy PUBLIC Qt6::Core)

# A REAL export set, the I1/search shape and its reasoning: the scorer is
# a top-level shelf member the app links directly (quick-open AND the
# palette are its two app-side users), NOT a link-only internal mirror.
# The header installs with it (fuzzy/FuzzyMatcher.hpp), and the records
# speak plain Qt types (struct + QVector<int>) — wrappable from birth,
# but the wheel crossing is NOT taken: no Python consumer has asked.
set_target_properties(qmn_fuzzy PROPERTIES EXPORT_NAME fuzzy)
install(TARGETS qmn_fuzzy EXPORT qmnTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES FuzzyMatcher.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/qmn/fuzzy)
