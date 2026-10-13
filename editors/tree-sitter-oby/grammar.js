/**
 * Tree-sitter grammar for OBY.
 *
 * Written from `src/compiler/parser.c` and `src/compiler/lexer.c`, so it matches
 * what the compiler accepts rather than what the docs describe. Four things are
 * worth knowing before reading it:
 *
 *  - OBY is newline-terminated, but a newline is dropped when the token *before*
 *    it cannot end a statement: an operator, a comma, an opening bracket, `.`,
 *    `::`, `=`, `->`. That is `cont()` here, and it appears in exactly those
 *    places — after the operator, never before it. `a = 1\n-2` is therefore two
 *    statements, while `a = 1 -\n2` is one; and `x\n= 1` is not a thing.
 *  - There are no reserved words. `if`, `type`, `local` and friends are
 *    identifiers matched by text, so `x.if` is legal; they are tokens here
 *    (statement parsing needs them) and are also accepted as member names.
 *  - `X = type { ... }`, `X = enum { ... }` and a type alias are assignments
 *    whose value is a type expression here, where the compiler has dedicated
 *    statement nodes. That keeps the `X = value` shape free of ambiguity, and
 *    the outline query looks at the value's node kind instead.
 *  - Types are written exactly like expressions (`int[] xs` and `xs[0]` cannot
 *    be told apart until the token after the `]`), so there is one expression
 *    grammar and no separate type grammar. Whether a name is a type is said by
 *    where it sits — a binding's `type` field, a `->` result — and the queries
 *    read it from there.
 */

const PREC = {
  OR: 1,
  AND: 2,
  COMPARE: 3,
  BIT_OR: 4,
  BIT_XOR: 5,
  BIT_AND: 6,
  SHIFT: 7,
  SUM: 8,
  TERM: 9,
  UNARY: 10,
  POSTFIX: 11,
  CALL: 12,
  ARROW: 13,
};

/* Contextual keywords: identifiers to the lexer, keywords to the parser. */
const KEYWORDS = [
  'use', 'type', 'packed', 'enum', 'local', 'extern', 'return',
  'if', 'else', 'switch', 'case', 'default', 'while', 'for',
  'break', 'continue', 'null', 'and', 'or', 'not',
];

/* A newline the compiler drops, because the token before it cannot end a
   statement. Inlined rather than a rule: tree-sitter rejects a rule that
   matches the empty string. */
const cont = $ => optional($._newline);

module.exports = grammar({
  name: 'oby',

  extras: $ => [
    /[ \t\r]/,
    $.comment,
  ],

  word: $ => $.identifier,

  supertypes: $ => [
    $._expression,
  ],

  /* Generics against comparisons. A type argument list is written tight
     (`Box<int>`, `Map<String, int>`, `xs.map<int>(f)`) while a comparison
     spaces its `<` out (`i < 5`), so the `<` that opens type arguments is
     immediate: it cannot follow whitespace. That is a rule the compiler does
     not have — it tries the type arguments and rewinds — but it is one a
     parser without unbounded lookahead can keep, and it is how OBY is written.
     The one casualty is a comparison spelled tight (`i<5`), which reads as the
     start of type arguments and reports an error. */
  conflicts: $ => [
    [$.index_expression, $.unary_expression, $.array_type],
    [$.index_expression, $.binary_expression, $.array_type],
    [$.parameter, $.parenthesized_expression, $.function_type],
    [$.parameter, $.function_type],
    [$.function_type, $.function_literal],
  ],

  rules: {
    source_file: $ => repeat(choice($._statement, $._newline)),

    comment: _ => token(seq('//', /[^\n]*/)),

    _newline: _ => /\n/,

    identifier: _ => /[a-zA-Z_][a-zA-Z0-9_]*/,

    /* ---------------------------------------------------------------- */
    /* Literals                                                         */
    /* ---------------------------------------------------------------- */

    number: _ => token(choice(
      /0[xX][0-9a-fA-F_]+/,
      /0[bB][01_]+/,
      /\d[\d_]*(\.\d[\d_]*)?([eE][+-]?\d[\d_]*)?/,
    )),

    char_literal: _ => token(seq("'", choice(
      seq('\\', choice(/x[0-9a-fA-F]{2}/, /./)),
      /[^'\\\n]/,
    ), "'")),

    /* One rule for both, since a plain string is an interpolation-free one. */
    string: $ => seq(
      '"',
      repeat(choice($.escape_sequence, $._string_content, $.interpolation)),
      '"',
    ),

    interpolation: $ => seq('${', $._expression, '}'),

    /* Longest match settles `${` against a lone `$`: both start at the `$`, and
       the two-character opener wins. */
    _string_content: _ => token.immediate(choice(/[^"\\$]+/, '$')),

    escape_sequence: _ => token.immediate(seq('\\', choice(/x[0-9a-fA-F]{2}/, /./))),

    /* ---------------------------------------------------------------- */
    /* Statements                                                       */
    /* ---------------------------------------------------------------- */

    _statement: $ => choice(
      $._simple_statement,
      $.if_statement,
      $.while_statement,
      $.for_statement,
      $.switch_statement,
    ),

    /* `prec.right` settles the terminator against the next statement: a newline
       after a complete statement ends it, which is what OBY does. */
    _simple_statement: $ => prec.right(seq($._simple_body, optional($._terminator))),

    _terminator: $ => choice($._newline, ';'),

    _simple_body: $ => choice(
      $.use_statement,
      $.return_statement,
      $.break_statement,
      $.continue_statement,
      $.local_binding,
      $.binding,
      $.static_member,
      $.compound_assignment,
      $.assignment,
      $._expression,
    ),

    use_statement: $ => seq('use', $.string),

    /* `prec.right` settles `return value` against a bare `return` followed by
       another statement: on the same line the expression wins, and across a
       newline it cannot start, so the bare form does. */
    return_statement: $ => prec.right(seq('return', optional($._expression))),

    break_statement: _ => 'break',

    continue_statement: _ => 'continue',

    /* `::name = value` inside a type body: a static member, which may be an
       operator (`::+`, `::==`, `::<`) or any other name. */
    static_member: $ => seq(
      '::',
      field('name', $._member_name),
      '=', cont($),
      field('value', $._expression),
    ),

    /* `local name = value`, or `local T name = value`. */
    local_binding: $ => prec.right(1, seq(
      'local',
      choice(
        seq(field('type', $._expression), field('name', $.identifier)),
        field('name', $.identifier),
      ),
      '=', cont($), field('value', $._expression),
    )),

    /* `T name = value`, where T may be spelled like anything else. A bare
       `name = value` is an assignment, which is what the compiler parses. */
    binding: $ => prec.right(1, seq(
      field('type', $._expression),
      field('name', $.identifier),
      '=', cont($),
      field('value', $._expression),
    )),

    assignment: $ => prec.right(seq(
      field('target', $._expression),
      '=', cont($),
      field('value', $._expression),
    )),

    compound_assignment: $ => prec.right(seq(
      field('target', $._expression),
      field('operator', $.compound_operator),
      cont($),
      field('value', $._expression),
    )),

    compound_operator: _ => choice(
      '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=', '<<=', '>>=',
    ),

    /* `prec.right` puts a following `else` with the `if` it belongs to, and
       settles it against the statement terminator. The block already swallowed
       the newline, so `else` needs none of its own. */
    if_statement: $ => prec.right(seq(
      'if', field('condition', $._expression), cont($),
      field('consequence', $.block),
      optional(seq(
        'else',
        choice(seq(cont($), $.block), $.if_statement),
      )),
    )),

    while_statement: $ => seq(
      'while', field('condition', $._expression), cont($), field('body', $.block),
    ),

    /* C-style header: `for (local i = 0; i < n; i += 1) { ... }`. */
    for_statement: $ => seq(
      'for', '(', cont($),
      field('initializer', choice(
        $.local_binding, $.binding, $.compound_assignment, $.assignment, $._expression,
      )),
      ';', cont($),
      field('condition', $._expression),
      ';', cont($),
      field('step', choice($.compound_assignment, $.assignment, $._expression)),
      ')', cont($),
      field('body', $.block),
    ),

    switch_statement: $ => seq(
      'switch', field('value', $._expression), cont($),
      '{',
      repeat(choice($._newline, $.switch_case, $.switch_default)),
      '}',
    ),

    switch_case: $ => seq(
      'case', field('value', $._expression),
      optional(seq('..', field('range_end', $._expression))),
      optional(seq('if', field('guard', $._expression))),
      cont($), field('body', $.block),
    ),

    switch_default: $ => seq('default', cont($), field('body', $.block)),

    /* A block swallows the newline that ends it, so an `else` on the next line
       still belongs to its `if` (the compiler skips those newlines). The
       precedence keeps that newline with the block rather than the enclosing
       statement list. */
    block: $ => prec.right(1, seq(
      '{', repeat(choice($._statement, $._newline)), '}',
      optional($._newline),
    )),

    /* `X = extern c_name(...) -> R`: an expression, so it can be bound to a
       name (`parse_primary` reaches it the same way in the compiler). */
    extern_definition: $ => seq(
      'extern', field('name', $.identifier),
      '(', cont($),
      optional(seq($.parameter, repeat(seq(',', cont($), $.parameter)))),
      ')',
      optional(seq('->', cont($), field('result', $._expression))),
    ),

    parameter: $ => seq(
      field('type', $._expression),
      optional(field('name', $.identifier)),
    ),

    /* ---------------------------------------------------------------- */
    /* Expressions                                                      */
    /* ---------------------------------------------------------------- */

    _expression: $ => choice(
      $._primary,
      $.unary_expression,
      $.binary_expression,
      $.member_expression,
      $.call_expression,
      $.index_expression,
    ),

    _primary: $ => choice(
      $.identifier,
      $.global_variable,
      $.number,
      $.char_literal,
      $.string,
      $.null,
      $.parenthesized_expression,
      $.array_literal,
      $.table_literal,
      $.function_literal,
      $.type_literal,
      $.enum_literal,
      $.packed_literal,
      $.function_type,
      $.array_type,
      $.generic_expression,
      $.extern_definition,
    ),

    null: _ => 'null',

    /* `$name`: the explicit spelling of a global reference. */
    global_variable: $ => seq('$', $.identifier),

    parenthesized_expression: $ => seq('(', cont($), $._expression, ')'),

    /* `a.b`, `Type::name`, `xs.map<int>`: one node with a property field. */
    member_expression: $ => prec.right(PREC.POSTFIX, seq(
      field('object', $._expression),
      field('operator', choice('.', '::')),
      cont($),
      field('property', $._member_name),
      optional($.type_arguments),
    )),

    call_expression: $ => prec(PREC.CALL, seq(
      field('function', $._expression),
      '(', cont($),
      optional(seq($._expression, repeat(seq(',', cont($), $._expression)), optional(','))),
      ')',
    )),

    index_expression: $ => prec(PREC.POSTFIX, seq(
      field('object', $._expression),
      prec(PREC.POSTFIX, '['), cont($), field('index', $._expression), ']',
    )),

    /* `Box<int>`: a name with type arguments, as a call's callee (`Box<int>()`)
       or a binding's type. */
    generic_expression: $ => prec.right(PREC.POSTFIX, seq(
      field('name', $.identifier),
      $.type_arguments,
    )),

    /* `<T, U>` after a name, written tight. The closer carries a precedence so
       `Box<Box<int>>` lexes its two angles one at a time instead of as a
       shift. */
    type_arguments: $ => prec.right(PREC.POSTFIX, seq(
      token.immediate('<'),
      optional(seq($._type_argument, repeat(seq(',', cont($), $._type_argument)))),
      token(prec(1, '>')),
    )),

    /* What may sit inside `<...>`: a type, not an arbitrary expression, so the
       closing `>` closes rather than compares. */
    _type_argument: $ => choice(
      $.identifier,
      $.member_expression,
      $.generic_expression,
      $.array_type,
      $.function_type,
      $.parenthesized_expression,
    ),

    unary_expression: $ => prec.right(PREC.UNARY, seq(
      field('operator', choice('-', '~', 'not')),
      cont($),
      field('operand', $._expression),
    )),

    binary_expression: $ => {
      const table = [
        ['or', PREC.OR],
        ['and', PREC.AND],
        ['==', PREC.COMPARE],
        ['!=', PREC.COMPARE],
        ['<', PREC.COMPARE],
        ['>', PREC.COMPARE],
        ['<=', PREC.COMPARE],
        ['>=', PREC.COMPARE],
        ['|', PREC.BIT_OR],
        ['^', PREC.BIT_XOR],
        ['&', PREC.BIT_AND],
        ['<<', PREC.SHIFT],
        ['>>', PREC.SHIFT],
        ['+', PREC.SUM],
        ['-', PREC.SUM],
        ['*', PREC.TERM],
        ['/', PREC.TERM],
        ['%', PREC.TERM],
      ];
      return choice(...table.map(([op, p]) => prec.left(p, seq(
        field('left', $._expression),
        field('operator', op),
        cont($),
        field('right', $._expression),
      ))));
    },

    array_literal: $ => seq(
      '[', cont($),
      optional(choice(
        /* `[value; count]` repeats one value. */
        seq($._expression, ';', cont($), field('count', $._expression)),
        seq(
          $._expression,
          repeat(seq(',', cont($), $._expression)),
          optional(','),
        ),
      )),
      ']',
    ),

    /* `{ a = 1, b = 2 }`: fields are statements. A type body is one of these. */
    table_literal: $ => seq(
      '{',
      repeat(choice($._statement, $._newline)),
      '}',
    ),

    /* `(T a, U b) -> R { ... }`, or `<T>(T x) -> T { ... }` when it is generic
       (`name = <T>(...)`). The dynamic precedence is what picks this over a
       function type followed by a table literal when both parse. */
    function_literal: $ => prec.dynamic(1, seq(
      optional($.type_parameters),
      '(',
      optional(seq($.parameter, repeat(seq(',', cont($), $.parameter)))),
      ')',
      optional(seq('->', cont($), field('result', $._expression))),
      field('body', $.block),
    )),

    /* `(T, U) -> R`: a function type, and (with `Name =` in front) a type
       alias. Its low precedence is what keeps `() -> int { ... }` a function
       literal: a following `{` continues the literal rather than reducing to a
       bare type followed by a table. */
    function_type: $ => seq(
      '(',
      optional(seq(field('parameter', $._expression), repeat(seq(',', cont($), field('parameter', $._expression))))),
      ')',
      '->', cont($), field('result', $._expression),
    ),

    /* `T[]` growable, `T[n]` fixed. The low precedence lets `arr[0]` stay an
       index; `[]` has no index reading, so it is still an array type. */
    array_type: $ => prec(1, seq(
      field('element', $._expression),
      prec(PREC.POSTFIX, '['), optional(field('count', $.number)), ']',
    )),

    /* ---------------------------------------------------------------- */
    /* Type definitions                                                 */
    /* ---------------------------------------------------------------- */

    /* `X = type { ... }` and `X = type<T> { ... }` */
    type_literal: $ => seq(
      'type',
      optional($.type_parameters),
      field('body', $.table_literal),
    ),

    /* `<T, K>`: the parameter names of a generic declaration. */
    type_parameters: $ => seq(
      '<',
      optional(seq($.identifier, repeat(seq(',', cont($), $.identifier)))),
      '>',
    ),

    /* `X = enum { A = 1 }` */
    enum_literal: $ => seq(
      'enum',
      '{',
      repeat(choice($._newline, $.enum_item)),
      '}',
    ),

    enum_item: $ => seq(
      field('name', $.identifier),
      optional(seq('=', cont($), field('value', $._expression))),
      optional(','),
    ),

    /* `X = packed BASE[N] { ... }`: the body is optional. */
    packed_literal: $ => prec.right(seq(
      'packed',
      field('element', $._expression),
      optional(field('body', $.table_literal)),
    )),

    /* ---------------------------------------------------------------- */
    /* Names                                                            */
    /* ---------------------------------------------------------------- */

    /* Names after `.` and `::`. Keywords are legal there (`x.if`, `x.type`),
       and so is an operator (`vec2i::+`, `::==`, `::<`). */
    _member_name: $ => choice(
      $.identifier,
      ...KEYWORDS,
      '+', '-', '*', '/', '%', '~',
      '==', '!=', '<', '>', '<=', '>=',
      '&', '|', '^', '<<', '>>',
    ),
  },
});
