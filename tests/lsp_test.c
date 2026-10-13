#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/array.h"
#include "core/version.h"
#include "lsp/json.h"
#include "lsp/server.h"
#include "lsp/uri.h"

/* The server is driven in-process: client frames into a temp file, server
   frames out of a temp file, compared byte for byte. Nothing here spawns a
   process or touches the network. */

static int failures = 0;

static void check(int cond, const char *what) {
    if (!cond) {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

/* ---- a scripted client session ---- */

typedef struct {
    char *data;
    size_t len, cap;
} Session;

static void session_add(Session *s, const char *json) {
    char header[64];
    int n = snprintf(header, sizeof header, "Content-Length: %zu\r\n\r\n", strlen(json));
    size_t need = (size_t)n + strlen(json);
    if (s->len + need + 1 > s->cap) {
        size_t cap = s->cap ? s->cap : 1024;
        while (cap < s->len + need + 1) cap *= 2;
        char *grown = (char *)realloc(s->data, cap);
        if (!grown) {
            printf("FAIL: out of memory\n");
            exit(1);
        }
        s->data = grown;
        s->cap = cap;
    }
    memcpy(s->data + s->len, header, (size_t)n);
    s->len += (size_t)n;
    memcpy(s->data + s->len, json, strlen(json));
    s->len += strlen(json);
}

/* Replay `s`; the frames the server wrote come back in `*out` (char*). */
static int run_session(const Session *s, Array **out) {
    FILE *in = tmpfile();
    FILE *outf = tmpfile();
    if (!in || !outf) {
        printf("FAIL: cannot create temp files\n");
        exit(1);
    }
    if (s->len > 0) fwrite(s->data, 1, s->len, in);
    rewind(in);

    int status = lsp_server_run(in, outf);

    rewind(outf);
    Array *frames = array_new(sizeof(char *));
    for (;;) {
        char *body = frame_read(outf, NULL);
        if (!body) break;
        array_push(frames, &body);
    }
    fclose(in);
    fclose(outf);
    *out = frames;
    return status;
}

static const char *frame_at(Array *frames, size_t i) {
    char **slot = (char **)array_get(frames, i);
    return slot ? *slot : NULL;
}

static void free_frames(Array *frames) {
    for (size_t i = 0; i < array_length(frames); i++)
        free(*(char **)array_get(frames, i));
    array_free(frames);
}

static void expect_frame(Array *frames, size_t i, const char *expected, const char *what) {
    const char *got = frame_at(frames, i);
    if (!got || strcmp(got, expected) != 0) {
        printf("FAIL: %s\n  expected: %s\n  got:      %s\n",
               what, expected, got ? got : "(no frame)");
        failures++;
    }
}

static void expect_contains(Array *frames, size_t i, const char *needle, const char *what) {
    const char *got = frame_at(frames, i);
    if (!got || !strstr(got, needle)) {
        printf("FAIL: %s (missing '%s')\n  got: %s\n",
               what, needle, got ? got : "(no frame)");
        failures++;
    }
}

int main(void) {
    const char *init_request =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
        "\"params\":{\"capabilities\":{}}}";
    char init_expected[512];
    snprintf(init_expected, sizeof init_expected,
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"capabilities\":{"
             "\"textDocumentSync\":1,\"positionEncoding\":\"utf-16\"},"
             "\"serverInfo\":{\"name\":\"oby\",\"version\":\"%s\"}}}",
             oby_version_str);

    /* ---- the full handshake: documents sync silently, shutdown answers ---- */
    {
        Session s = {0};
        session_add(&s, init_request);
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":"
                        "{\"textDocument\":{\"uri\":\"file:///tmp/t.oby\",\"languageId\":\"oby\","
                        "\"version\":1,\"text\":\"x = 1\\n\"}}}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\",\"params\":"
                        "{\"textDocument\":{\"uri\":\"file:///tmp/t.oby\",\"version\":2},"
                        "\"contentChanges\":[{\"text\":\"x = 2\\n\"}]}}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didClose\",\"params\":"
                        "{\"textDocument\":{\"uri\":\"file:///tmp/t.oby\"}}}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"shutdown\"}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"textDocument/hover\",\"params\":{}}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");

        Array *frames;
        int status = run_session(&s, &frames);
        check(status == 0, "exit after shutdown is status 0");
        check(array_length(frames) == 3, "document sync is silent");
        expect_frame(frames, 0, init_expected, "the initialize response");
        expect_frame(frames, 1, "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":null}",
                     "shutdown responds null");
        expect_frame(frames, 2,
                     "{\"jsonrpc\":\"2.0\",\"id\":3,\"error\":{\"code\":-32600,"
                     "\"message\":\"server is shutting down\"}}",
                     "requests after shutdown are refused");
        free_frames(frames);
        free(s.data);
    }

    /* ---- position encoding negotiation ---- */
    {
        Session s = {0};
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"
                        "{\"capabilities\":{\"general\":{\"positionEncodings\":[\"utf-8\",\"utf-16\"]}}}}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");
        Array *frames;
        run_session(&s, &frames);
        expect_contains(frames, 0, "\"positionEncoding\":\"utf-8\"",
                        "utf-8 is chosen when the client offers it");
        free_frames(frames);
        free(s.data);
    }
    {
        Session s = {0};
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"
                        "{\"capabilities\":{\"general\":{\"positionEncodings\":[\"utf-32\"]}}}}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");
        Array *frames;
        run_session(&s, &frames);
        expect_contains(frames, 0, "\"positionEncoding\":\"utf-16\"",
                        "an unsupported offer falls back to utf-16");
        free_frames(frames);
        free(s.data);
    }

    /* ---- before initialize: requests refused, notifications dropped ---- */
    {
        Session s = {0};
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":"
                        "{\"textDocument\":{}}}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"textDocument/hover\",\"params\":{}}");
        Array *frames;
        int status = run_session(&s, &frames);
        check(status == 1, "a session that never shuts down exits 1");
        check(array_length(frames) == 1, "the pre-initialize notification is dropped");
        expect_frame(frames, 0,
                     "{\"jsonrpc\":\"2.0\",\"id\":7,\"error\":{\"code\":-32002,"
                     "\"message\":\"server not initialized\"}}",
                     "a pre-initialize request is refused");
        free_frames(frames);
        free(s.data);
    }

    /* ---- unknown methods and `$`-prefixed ones ---- */
    {
        Session s = {0};
        session_add(&s, init_request);
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"foo/bar\"}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"$/setTrace\",\"params\":{\"value\":\"off\"}}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"method\":\"$/setTrace\",\"params\":{\"value\":\"off\"}}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"method\":\"my/note\"}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"shutdown\"}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");

        Array *frames;
        int status = run_session(&s, &frames);
        check(status == 0, "unknown methods do not disturb the session");
        check(array_length(frames) == 4, "unknown notifications are dropped");
        expect_frame(frames, 1,
                     "{\"jsonrpc\":\"2.0\",\"id\":5,\"error\":{\"code\":-32601,"
                     "\"message\":\"unknown method 'foo/bar'\"}}",
                     "an unknown request is MethodNotFound");
        expect_frame(frames, 2,
                     "{\"jsonrpc\":\"2.0\",\"id\":6,\"error\":{\"code\":-32601,"
                     "\"message\":\"unknown method '$/setTrace'\"}}",
                     "a `$/` request is MethodNotFound too");
        free_frames(frames);
        free(s.data);
    }

    /* ---- a malformed frame is answered, and the session survives ---- */
    {
        Session s = {0};
        session_add(&s, "{\"jsonrpc\": \"2.0\", oops}");
        session_add(&s, init_request);
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"shutdown\"}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");

        Array *frames;
        int status = run_session(&s, &frames);
        check(status == 0, "the session survives a malformed frame");
        check(array_length(frames) == 3, "the parse error is answered");
        expect_contains(frames, 0, "\"id\":null", "a parse error has a null id");
        expect_contains(frames, 0, "\"code\":-32700", "a parse error is -32700");
        expect_frame(frames, 1, init_expected, "initialize still works afterwards");
        free_frames(frames);
        free(s.data);
    }

    /* ---- exit without shutdown ---- */
    {
        Session s = {0};
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");
        Array *frames;
        int status = run_session(&s, &frames);
        check(status == 1, "exit without shutdown is status 1");
        check(array_length(frames) == 0, "a bare exit writes nothing");
        free_frames(frames);
        free(s.data);
    }

    /* ---- the JSON and URI layers underneath ---- */
    {
        Arena arena;
        arena_init(&arena);
        const char *err = NULL;

        JsonValue *root = json_parse(&arena,
            "{\"a\":[1,2,{\"b\":\"x\\ny\"}],\"n\":null,\"t\":true}", &err);
        check(root != NULL, "a document parses");
        JsonValue *a = json_get(root, "a");
        check(a && a->kind == JSON_ARRAY && array_length(a->items) == 3, "the array member");
        long long i = 0;
        check(a && json_int(*(JsonValue **)array_get(a->items, 0), &i) && i == 1,
              "integers read back");
        JsonValue *third = a ? *(JsonValue **)array_get(a->items, 2) : NULL;
        const char *b = json_string(json_get(third, "b"));
        check(b && strcmp(b, "x\ny") == 0, "string escapes decode");
        check(json_get(root, "n") && json_get(root, "n")->kind == JSON_NULL,
              "null reads back");
        check(json_get(root, "missing") == NULL, "a missing member is NULL");

        JsonValue *emoji = json_parse(&arena, "\"\\ud83d\\ude00\"", &err);
        check(emoji && emoji->string_len == 4 &&
              (unsigned char)emoji->string[0] == 0xF0,
              "a surrogate pair becomes UTF-8");

        check(json_parse(&arena, "{\"a\":}", &err) == NULL, "malformed JSON is refused");
        check(json_parse(&arena, "1 2", &err) == NULL, "trailing characters are refused");

        char escaped[64];
        json_escape(escaped, sizeof escaped, "a\"b\\c\nd");
        check(strcmp(escaped, "a\\\"b\\\\c\\nd") == 0, "escaping round-trips");

        char path[256];
        check(uri_to_path("file:///tmp/a%20b.oby", path, sizeof path) &&
              strcmp(path, "/tmp/a b.oby") == 0, "a %20 URI decodes");
        check(!uri_to_path("http://x/y", path, sizeof path), "a non-file URI is refused");
        check(path_to_uri("/tmp/a b.oby", path, sizeof path) &&
              strcmp(path, "file:///tmp/a%20b.oby") == 0, "a path becomes a URI");
        arena_free(&arena);
    }

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("lsp ok\n");
    return 0;
}
