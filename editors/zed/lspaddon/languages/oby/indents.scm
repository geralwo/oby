; Indentation: a block, a table or a switch body indents its contents, with the
; range running from just after the opener to just before the closer.

(block
  "{" @start
  "}" @end) @indent

(table_literal
  "{" @start
  "}" @end) @indent

(switch_statement
  "{" @start
  "}" @end) @indent
