/*
 * Standalone test for the JSON natives (backed by vendor/json.h), through the
 * native registry. Built and run by `./compile.sh test` (every C file under
 * tests/ becomes its own binary and must exit 0).
 */
#include <stdio.h>
#include <string.h>

#include "core/string.h"
#include "native/native.h"

static int failures = 0;

static void check(const char *what, long long got, long long want) {
    if (got != want) {
        printf("  FAIL %s: got %lld, want %lld\n", what, got, want);
        failures++;
    } else {
        printf("  ok   %s\n", what);
    }
}

static Value str_value(const char *text) {
    return val_string(string_new_len(text, strlen(text)));
}

static Value call1(int id, Value a) {
    return native_call(id, &a, 1);
}

static Value call2(int id, Value a, Value b) {
    Value args[2] = { a, b };
    return native_call(id, args, 2);
}

/* The C string of a String value (the test's own strings are NUL-terminated). */
static const char *text_of(Value v) {
    return v.tag == VAL_STRING ? v.as.str->data : "";
}

int main(void) {
    native_init();
    native_register_std();

    int parse = native_find("json_parse");
    int free_ = native_find("json_free");
    int kind = native_find("json_kind");
    int len = native_find("json_len");
    int key_at = native_find("json_key_at");
    int at = native_find("json_at");
    int get = native_find("json_get");
    int as_string = native_find("json_as_string");
    int as_int = native_find("json_as_int");
    int as_float = native_find("json_as_float");
    int as_bool = native_find("json_as_bool");
    int minified = native_find("json_to_minified");
    int pretty = native_find("json_to_pretty");
    int error = native_find("json_error");
    check("the json natives are registered",
          parse >= 0 && free_ >= 0 && kind >= 0 && len >= 0 && key_at >= 0 &&
              at >= 0 && get >= 0 && as_string >= 0 && as_int >= 0 &&
              as_float >= 0 && as_bool >= 0 && minified >= 0 && pretty >= 0 &&
              error >= 0,
          1);

    /* Every kind, an embedded NUL, a fractional and an out-of-range number. */
    Value doc = call1(parse, str_value(
        "{\"s\":\"a\\u0000b\",\"n\":1.9,\"big\":1e30,\"t\":true,\"nil\":null,"
        "\"xs\":[10,20]}"));
    check("parse yields a document", doc.as.i != 0, 1);
    check("kind is object", call1(kind, doc).as.i, 1);
    check("len counts members", call1(len, doc).as.i, 6);
    check("key_at finds a member name",
          (long long)(strcmp(text_of(call2(key_at, doc, val_int(1))), "n") == 0), 1);

    Value s = call2(get, doc, str_value("s"));
    Value s_text = call1(as_string, s);
    check("a string keeps its length", (long long)s_text.as.str->length, 3);
    check("... and its embedded NUL",
          (long long)(memcmp(s_text.as.str->data, "a\0b", 3) == 0), 1);
    check("the writer escapes the NUL",
          (long long)(strcmp(text_of(call1(minified, s)), "\"a\\u0000b\"") == 0), 1);

    check("as_int truncates", call1(as_int, call2(get, doc, str_value("n"))).as.i, 1);
    check("an out-of-range number reads 0",
          call1(as_int, call2(get, doc, str_value("big"))).as.i, 0);
    double f = call1(as_float, call2(get, doc, str_value("n"))).as.f;
    check("as_float reads the number", f > 1.89 && f < 1.91, 1);
    check("as_string of a number is its text",
          (long long)(strcmp(text_of(call1(as_string,
                                           call2(get, doc, str_value("big")))),
                             "1e30") == 0),
          1);

    check("as_bool of true", call1(as_bool, call2(get, doc, str_value("t"))).as.i, 1);
    check("as_bool of a string", call1(as_bool, call2(get, doc, str_value("s"))).as.i, 0);

    Value nil = call2(get, doc, str_value("nil"));
    check("null is a real value", nil.as.i != 0, 1);
    check("... of kind null", call1(kind, nil).as.i, 6);
    Value missing = call2(get, doc, str_value("nope"));
    check("a missing member is handle 0", missing.as.i, 0);
    check("... of kind none", call1(kind, missing).as.i, 0);

    Value xs = call2(get, doc, str_value("xs"));
    check("an array's element count", call1(len, xs).as.i, 2);
    check("at reads an element", call1(as_int, call2(at, xs, val_int(1))).as.i, 20);
    check("at out of range is handle 0", call2(at, xs, val_int(9)).as.i, 0);
    Value xs_min = call1(minified, xs);
    check("a value minifies",
          (long long)(strcmp(text_of(xs_min), "[10,20]") == 0), 1);
    check("... with an exact length", (long long)xs_min.as.str->length, 7);
    Value xs_pretty = call1(pretty, xs);
    check("pretty output is multi-line",
          (long long)(strchr(text_of(xs_pretty), '\n') != NULL), 1);
    check("... and carries no terminator",
          (long long)(xs_pretty.as.str->length == strlen(text_of(xs_pretty))), 1);

    /* A failed parse: handle 0, and the error names the place. */
    Value bad = call1(parse, str_value("{\"a\": [1, 2,]}"));
    check("a trailing comma is refused", bad.as.i, 0);
    Value message = native_call(error, NULL, 0);
    check("the error names the byte",
          (long long)(strstr(text_of(message), "byte ") != NULL), 1);
    printf("       parse error: %s\n", text_of(message));

    call1(free_, doc);
    call1(free_, val_int(0));
    printf("  ok   freeing a document and a handle-0 Json\n");

    /* A fresh successful parse clears the error. */
    Value again = call1(parse, str_value("7"));
    check("a scalar document parses", call1(as_int, again).as.i, 7);
    check("a success clears the error",
          (long long)(text_of(native_call(error, NULL, 0))[0] == '\0'), 1);
    call1(free_, again);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("json natives ok\n");
    return 0;
}
