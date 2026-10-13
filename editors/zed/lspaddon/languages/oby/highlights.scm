; Syntax highlighting for OBY (Zed capture names).
;
; More specific patterns come later: when two patterns claim the same node, the
; later one wins.

; ------------------------------------------------------------------ comments
(comment) @comment

; ------------------------------------------------------------------ keywords
[
  "use"
  "type"
  "packed"
  "enum"
  "local"
  "extern"
  "return"
  "if"
  "else"
  "switch"
  "case"
  "default"
  "while"
  "for"
  "break"
  "continue"
] @keyword

["and" "or" "not"] @keyword.operator

"null" @constant.builtin

; ------------------------------------------------------------------ literals
(number) @number
(char_literal) @string.special
(escape_sequence) @string.escape
(string) @string

(interpolation
  "${" @punctuation.special
  "}" @punctuation.special)

; ------------------------------------------------------------------ names
(global_variable
  "$" @punctuation.special
  (identifier) @variable.special)

; The name being declared.
(local_binding name: (identifier) @variable)
(parameter name: (identifier) @variable.parameter)
(binding name: (identifier) @variable)
(static_member name: (_) @property)
(enum_item name: (identifier) @constant)
(extern_definition name: (identifier) @function)
(function_literal (type_parameters (identifier) @type))
(type_literal (type_parameters (identifier) @type))

; Whichever side of a name is a type, wherever a type can be written.
(binding type: (_) @type)
(local_binding type: (_) @type)
(parameter type: (_) @type)
(function_type parameter: (_) @type)
(function_type result: (_) @type)
(function_literal result: (_) @type)
(extern_definition result: (_) @type)
(packed_literal element: (_) @type)
(generic_expression name: (identifier) @type)

; Members and calls.
(member_expression property: (_) @property)
(call_expression function: (identifier) @function)

; ------------------------------------------------------------------ operators
[
  "+"
  "-"
  "*"
  "/"
  "%"
  "&"
  "|"
  "^"
  "~"
  "<<"
  ">>"
  "and"
  "or"
  "not"
  "=="
  "!="
  "<"
  ">"
  "<="
  ">="
  "="
  "+="
  "-="
  "*="
  "/="
  "%="
  "&="
  "|="
  "^="
  "<<="
  ">>="
  "->"
  "."
  "::"
] @operator

; ------------------------------------------------------------------ punctuation
["(" ")" "[" "]" "{" "}"] @punctuation.bracket
["," ";"] @punctuation.delimiter

; ------------------------------------------------------------------ last
; The language's own types are also its constructors (`int(2)`, `byte(65)`),
; and `true`/`false` are plain identifiers resolved by the compiler. Both go
; last so they beat the variable and function captures above.
((identifier) @boolean
  (#any-of? @boolean "true" "false"))

((identifier) @type.builtin
  (#any-of? @type.builtin
     "int" "int64" "uint" "uint64" "byte" "float" "double"
     "String" "object" "void" "vec2" "vec3" "vec2i" "vec3i"))
