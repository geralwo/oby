if exists("b:current_syntax")
  finish
endif

" 1. Keywords & Control Flow
syn keyword obyKeyword use type packed enum local extern return
syn keyword obyConditional if else switch case default
syn keyword obyRepeat while for break continue
syn keyword obyBoolean true false null
syn keyword obyOperator and or not

" 2. Built-in Types & Namespaces
syn keyword obyType int uint int64 uint64 byte float double String object void
syn keyword obyType vec2i vec3i vec2 vec3
syn keyword obyType File Terminal Input Time Socket

" 3. Comments
syn match obyComment "//.*$" contains=obyTodo
syn keyword obyTodo contained TODO FIXME XXX BUG NOTE

" 4. Numbers (Decimal, Hex, Binary, Floats)
syn match obyNumber "\<0x[0-9a-fA-F_]\+\>"
syn match obyNumber "\<0b[01_]\+\>"
syn match obyNumber "\<[0-9][0-9_]*\>"
syn match obyFloat  "\<[0-9][0-9_]*\.[0-9][0-9_]*\([eE][+-]\?[0-9]\+\)\?\>"

" 5. Characters
syn match obySpecialChar contained "\\\([\\'"0\tnre]\|x[0-9a-fA-F]\{2}\)"
syn match obyCharacter "'\([^\\']\|\\.\)'" contains=obySpecialChar

" 6. Strings & String Interpolation (${expr})
syn region obyInterpolation matchgroup=obyInterpolationDelim start="\${" end="}" contained contains=TOP
syn region obyString start=+"+ skip=+\\"+ end=+"+ contains=obySpecialChar,obyInterpolation

" 7. Explicit Global Variables ($name)
syn match obyGlobal "\$[A-Za-z_][A-Za-z0-9_]*"

" 8. Dedicated Scope Resolution (::) & Dot (.) Operator
syn match obyStaticMember "[A-Za-z_][A-Za-z0-9_]*" contained
syn match obyScopeOperator "::" nextgroup=obyStaticMember

syn match obyMember "[A-Za-z_][A-Za-z0-9_]*" contained
syn match obyDotOperator "\." nextgroup=obyMember

" 9. Field declarations inside { ... } bodies (e.g. test = 0, test2 = "...")
syn match obyFieldDecl "\<[A-Za-z_][A-Za-z0-9_]*\ze\s*=" contained containedin=obyBraces
syn region obyBraces matchgroup=Delimiter start="{" end="}" contains=TOP,obyFieldDecl,obyScopeOperator,obyStaticMember,obyDotOperator,obyMember

" 10. General Arithmetic, Logical & Comparison Operators
syn match obyOperator "[-+*/%&|^~<>=!]"
syn match obyOperator "=="
syn match obyOperator "!="
syn match obyOperator "<="
syn match obyOperator ">="
syn match obyOperator "->"
syn match obyOperator "+="
syn match obyOperator "-="
syn match obyOperator "*="
syn match obyOperator "/="
syn match obyOperator "%="
syn match obyOperator "&="
syn match obyOperator "|="
syn match obyOperator "^="
syn match obyOperator "<<="
syn match obyOperator ">>="

" ---------------------------------------------------------
" Highlight Links
" ---------------------------------------------------------
hi def link obyKeyword             Keyword
hi def link obyConditional         Conditional
hi def link obyRepeat              Repeat
hi def link obyBoolean             Boolean
hi def link obyType                Type
hi def link obyComment             Comment
hi def link obyTodo                Todo
hi def link obyNumber              Number
hi def link obyFloat               Float
hi def link obyCharacter           Character
hi def link obySpecialChar         SpecialChar
hi def link obyString              String
hi def link obyInterpolationDelim  Delimiter
hi def link obyGlobal              Identifier

" Scope Operator (::) - Yellow Operator color
hi def link obyScopeOperator       Operator
hi def link obyDotOperator         Delimiter

" General Operators (Yellow)
hi def link obyOperator            Operator

" Members & Fields (Green / Identifier)
hi def link obyMember              Identifier
hi def link obyFieldDecl           Identifier
hi def link obyStaticMember        Identifier

let b:current_syntax = "oby"
