# The glue shelf (G1 promotion, PLAN "The glue shelf + ProjectSession"):
# the old src/app/glue hub, now a REAL layer and a REAL export. Layer rule
# (the retired M8 amendment inverted): src/glue is a CLIENT of
# core+view+protocol+lsp and knows NO app/demo type; app targets link this
# target, and no glue type travels further. Tested by tst_lsp (the hub
# machines ride that binary, the 5th ctest entry unchanged) and by the
# examples/consumer glue TU against the INSTALLED shelf.
add_library(qmn_glue STATIC
    LspHub.hpp
    LspHub.cpp
    ProjectSession.hpp
    ProjectSession.cpp)
target_include_directories(qmn_glue PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/qmn>)
target_link_libraries(qmn_glue PUBLIC qmn_view qmn_lsp)
# The initialize clientInfo version the hub announces rides the PACKAGE
# version: the 1.8.0 close found a hand-maintained literal drifting (the
# project() line still said 1.6.1 while pyproject rode 1.7.0), so this is
# configured, not typed. THIS stays the package PROJECT_VERSION even though
# the shelf now versions itself (below): clientInfo.version reports the
# editor package, not the shelf — which is also why the shelf version is a
# plain variable, NOT a nested project() (that would reset PROJECT_VERSION).
target_compile_definitions(qmn_glue PRIVATE
    QMN_PACKAGE_VERSION="${PROJECT_VERSION}")

# ---- the shelf's own package surface (its own semver) ---------------------
# Third version literal in the repo (AGENTS: the two package literals ride
# the close ritual; this one bumps ONLY when the shelf itself breaks). It
# opens at 1.0.0: the hub existed since M8 but only NOW becomes public.
set(QMN_GLUE_VERSION 1.0.0)

set_target_properties(qmn_glue PROPERTIES EXPORT_NAME glue)
install(TARGETS qmn_glue EXPORT qmnGlueTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES LspHub.hpp ProjectSession.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/qmn/glue)

# Second export set, SAME qmn:: namespace (qmn::glue): its targets carry
# link refs to qmn::view/qmn::lsp/qmn::protocol from the qmnTargets set, so
# the config MUST find_dependency(qmn) BEFORE including the target file
# (load order is the whole contract; PLAN records this surgery).
include(CMakePackageConfigHelpers)
install(EXPORT qmnGlueTargets
    NAMESPACE qmn::
    FILE qmnGlueTargets.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qmnGlue)
configure_package_config_file(
    "${CMAKE_CURRENT_LIST_DIR}/../../cmake/qmnGlueConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/qmnGlueConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/qmnGlue")
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/qmnGlueConfigVersion.cmake"
    VERSION ${QMN_GLUE_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/qmnGlueConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/qmnGlueConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qmnGlue)
