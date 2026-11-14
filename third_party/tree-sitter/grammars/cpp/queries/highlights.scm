; Functions

(call_expression
  function: (qualified_identifier
    name: (identifier) @function))

(template_function
  name: (identifier) @function)

(template_method
  name: (field_identifier) @function)

(template_function
  name: (identifier) @function)

(function_declarator
  declarator: (qualified_identifier
    name: (identifier) @function))

(function_declarator
  declarator: (field_identifier) @function)

; Types

((namespace_identifier) @type
 (#match? @type "^[A-Z]"))

(auto) @type

; Constants

(this) @variable.builtin
(null "nullptr" @constant)

; Keywords

[
 "catch"
 "class"
 "co_await"
 "co_return"
 "co_yield"
 "constexpr"
 "constinit"
 "consteval"
 "delete"
 "explicit"
 "final"
 "friend"
 "mutable"
 "namespace"
 "noexcept"
 "new"
 "override"
 "private"
 "protected"
 "public"
 "template"
 "throw"
 "try"
 "typename"
 "using"
 "concept"
 "requires"
 "virtual"
] @keyword

; Strings

(raw_string_literal) @string

; ---------------------------------------------------------------------------
; LOCAL ADAPTATION (recorded in third_party/tree-sitter/VENDORED.md): the
; upstream v0.23.4 file ends above. The org repo never extended this query -
; comments, numbers, plain strings, primitive types and control-flow
; keywords stay uncolored with it (downstream editors keep their own richer
; copies). Appended below: the missing basics, upstream-style syntax and
; capture names only, every node/token/field verified against this pin's
; node-types.json. Appending at the END is load-bearing: the engine merge
; is later-pattern-wins (src/view/HighlightEngine.cpp), and the re-asserted
; @function refinements at the very bottom keep call/definition names
; winning over the generic @property/@constant catches above - the same
; relative order the c grammar's upstream query uses.

; Comments

(comment) @comment

; Preprocessor directives (#pragma/#error/#undef: not tokens of this pin)

[
 "#define"
 "#elif"
 "#else"
 "#endif"
 "#if"
 "#ifdef"
 "#ifndef"
 "#include"
] @keyword

; Numbers, chars, strings (escape LAST so it wins its units inside both)

(number_literal) @number
(char_literal) @number
(string_literal) @string
(escape_sequence) @escape
(system_lib_string) @string
(true) @constant
(false) @constant

; Control flow + storage-class keywords (the upstream list covers the
; C++-flavored ones: class, namespace, template, try, new, delete, ...)

[
 "asm"
 "break"
 "case"
 "const"
 "continue"
 "default"
 "do"
 "else"
 "enum"
 "extern"
 "for"
 "goto"
 "if"
 "inline"
 "operator"
 "register"
 "restrict"
 "return"
 "sizeof"
 "static"
 "struct"
 "switch"
 "thread_local"
 "typedef"
 "union"
 "volatile"
] @keyword

; Types and members. The identifier catch-all mirrors the c grammar's line 1
; (upstream cpp has no @variable at all, so plain locals would stay white);
; it sits BEFORE the ALL_CAPS rule and the re-asserted @function block below
; so those always win their spans under the later-wins merge.

(identifier) @variable
(primitive_type) @type
(type_identifier) @type
(field_identifier) @property

; Macros and enum constants (ALL_CAPS rule, same text as the c grammar)

((identifier) @constant
 (#match? @constant "^[A-Z][A-Z\\d_]*$"))

; Re-asserted LAST so the later-wins merge keeps function names @function
; over the generic catches above (the first seven mirror/extend the upstream
; Functions section; the last two add obj.method() calls and bare-name
; definitions - plain foo() calls - that the upstream query misses).

(call_expression
  function: (qualified_identifier
    name: (identifier) @function))

(template_function
  name: (identifier) @function)

(template_method
  name: (field_identifier) @function)

(function_declarator
  declarator: (qualified_identifier
    name: (identifier) @function))

(function_declarator
  declarator: (field_identifier) @function)

(function_declarator
  declarator: (identifier) @function)

(destructor_name
  (identifier) @function)

(call_expression
  function: (identifier) @function)

(call_expression
  function: (field_expression
    field: (field_identifier) @function))

; Macro names (@function.special like the c query's last patterns; AFTER
; the ALL_CAPS rule above so the macro name wins its own span).
(preproc_function_def
  name: (identifier) @function.special)
