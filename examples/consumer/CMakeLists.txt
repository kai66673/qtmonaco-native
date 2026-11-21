# C++ consumer smoke (PLAN.md Phase F): an external project that ONLY does
# find_package(qmn) against the INSTALLED package — proves the export set,
# qmnConfig, the header layout and the internal-tree-sitter link re-fix all
# hold outside the build tree. Build:
#
#   cmake --install build --prefix /tmp/qmn-prefix
#   cmake -S examples/consumer -B build-consumer \
#         -DCMAKE_PREFIX_PATH=/tmp/qmn-prefix
#   cmake --build build-consumer
#   QT_QPA_PLATFORM=offscreen ./build-consumer/qmn-consumer-smoke

cmake_minimum_required(VERSION 3.22)
project(qmn_consumer_smoke LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(qmn REQUIRED) # pulls Qt6 (Core/Gui/Widgets) via find_dependency
# The G1 glue shelf: a SEPARATE find, separate decision (PLAN). Its config
# find_dependency(qmn)s on its own — pointing at the SAME prefix makes that
# second find resolve to the same installed package (the load-order proof).
find_package(qmnGlue REQUIRED)

qt_standard_project_setup()
qt_add_executable(qmn-consumer-smoke main.cpp)
target_link_libraries(qmn-consumer-smoke PRIVATE qmn::view)

# The shelf's own smoke TU: the qmn::glue doors compile+link across the
# export boundary and NOTHING spawns (clangd is not a CI guarantee — every
# door here answers without a server).
qt_add_executable(qmn-consumer-glue-smoke glue_smoke.cpp)
target_link_libraries(qmn-consumer-glue-smoke PRIVATE qmn::glue)
