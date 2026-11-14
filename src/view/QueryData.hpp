#pragma once

// The vendored grammars' .scm highlights queries, embedded into the binary
// at build time by third_party/tree-sitter/CMakeLists.txt (generated
// query_data.cpp). Null-terminated UTF-8; no embedded NULs.

namespace qmn::view::queries {

const char* pythonHighlights();
const char* jsonHighlights();
const char* javascriptHighlights();
const char* cHighlights();
const char* cppHighlights();

} // namespace qmn::view::queries
