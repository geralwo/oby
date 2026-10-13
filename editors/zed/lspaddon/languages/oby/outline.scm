; What the outline panel shows. `@name` is the text, `@item` the range.

; `X = type { ... }`, `X = type<T> { ... }`
(assignment
  target: (identifier) @name
  value: (type_literal)) @item

; `X = enum { ... }`
(assignment
  target: (identifier) @name
  value: (enum_literal)) @item

; `X = packed T[N] { ... }`
(assignment
  target: (identifier) @name
  value: (packed_literal)) @item

; `name = (params) -> ret { ... }` and `name = <T>(params) -> ret { ... }`
(assignment
  target: (identifier) @name
  value: (function_literal)) @item

; `Name = (T) -> R`: a name for a function type.
(assignment
  target: (identifier) @name
  value: (function_type)) @item

; `name = extern c_name(...)`
(assignment
  target: (identifier) @name
  value: (extern_definition)) @item

; `Type name = value`: a field or a typed local.
(binding
  name: (identifier) @name) @item

(local_binding
  name: (identifier) @name) @item

; `::name = value` in a type body.
(static_member
  name: (_) @name) @item

; `A = 1` in an enum body.
(enum_item
  name: (identifier) @name) @item

; Anything else assigned to a name: a global.
(assignment
  target: (identifier) @name
  value: [
    (number)
    (string)
    (char_literal)
    (identifier)
    (generic_expression)
    (array_literal)
    (table_literal)
    (array_type)
    (call_expression)
    (member_expression)
    (index_expression)
    (binary_expression)
    (unary_expression)
    (parenthesized_expression)
    (global_variable)
  ]) @item
