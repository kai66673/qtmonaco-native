# Vendored tree-sitter (see VENDORED.md). One static library: the runtime
# compiled as its single amalgamation TU (lib/src `runtime/src/lib.c`) plus
# one generated parser.c (+ external scanner where present) per grammar,
# plus the CMake-generated query_data.cpp embedding the .scm highlights.
#
# Layering: this library is PRIVATE to qmn_view; tree-sitter must never
# reach src/core (PLAN.md D2).

cmake_minimum_required(VERSION 3.22)

# The runtime is C11; parser.c/scanner.c are generated C too.
enable_language(C)

# Embed the grammar queries as raw strings: configure_file pastes the file
# contents verbatim into the template's @VAR@ slots. The chosen raw-string
# delimiter must not occur inside any query file; assert it.
file(READ ${CMAKE_CURRENT_LIST_DIR}/grammars/python/queries/highlights.scm QMN_SCM_PYTHON)
file(READ ${CMAKE_CURRENT_LIST_DIR}/grammars/json/queries/highlights.scm QMN_SCM_JSON)
file(READ ${CMAKE_CURRENT_LIST_DIR}/grammars/javascript/queries/highlights.scm QMN_SCM_JAVASCRIPT)
foreach(scm QMN_SCM_PYTHON QMN_SCM_JSON QMN_SCM_JAVASCRIPT)
    if("${${scm}}" MATCHES [[\)QMN"]])
        message(FATAL_ERROR "query contains the raw-string delimiter )QMN\"")
    endif()
endforeach()
# configure_file runs at configure time only: without this, editing a .scm
# would silently keep the OLD embedded query in an incremental build.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_LIST_DIR}/grammars/python/queries/highlights.scm
    ${CMAKE_CURRENT_LIST_DIR}/grammars/json/queries/highlights.scm
    ${CMAKE_CURRENT_LIST_DIR}/grammars/javascript/queries/highlights.scm)
configure_file(query_data.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/query_data.cpp @ONLY)

add_library(qmn_ts STATIC
    runtime/src/lib.c
    grammars/python/src/parser.c
    grammars/python/src/scanner.c
    grammars/json/src/parser.c
    grammars/javascript/src/parser.c
    grammars/javascript/src/scanner.c
    ${CMAKE_CURRENT_BINARY_DIR}/query_data.cpp)

set_target_properties(qmn_ts PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)

# api.h for the runtime and the query shim; the generated parsers find
# their own tree_sitter/parser.h in their grammar src dir. The shim
# includes "view/QueryData.hpp" from the repo src root. BUILD_INTERFACE
# only: qmn_ts ships in the install package as an internal archive
# (top-level CMakeLists), and a raw source-tree include dir would break
# install(EXPORT).
target_include_directories(qmn_ts
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/runtime/include>
    PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/grammars/python/src
        ${CMAKE_CURRENT_LIST_DIR}/grammars/json/src
        ${CMAKE_CURRENT_LIST_DIR}/grammars/javascript/src
        ${CMAKE_CURRENT_LIST_DIR}/../../src)

# Vendored third-party + generated parser code: keep the noise out of our
# build logs (we never fix upstream warnings; the pins compile clean on CI).
target_compile_options(qmn_ts PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-w>
    $<$<COMPILE_LANGUAGE:CXX>:$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-w>>)
