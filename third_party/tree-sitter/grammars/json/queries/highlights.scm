(string) @string

(number) @number

[
  (null)
  (true)
  (false)
] @constant.builtin

(escape_sequence) @escape

(comment) @comment

; Pair keys capture the SAME node as (string) above. Under our merge
; convention (src/view/HighlightEngine.cpp) the LAST matching pattern per
; node wins - tree-sitter master's convention - so this block sits after
; (string) on purpose; the upstream v0.27.2-era file placed it first for
; the old first-wins convention (see VENDORED.md).
(pair
  key: (_) @string.special.key)
