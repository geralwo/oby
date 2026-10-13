#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/array.h"
#include "core/version.h"
#include "lsp/json.h"
#include "lsp/query.h"
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

/* Some frame (any position) contains `needle`. Used where the order of
   published diagnostics is not the point. */
static void expect_any_frame(Array *frames, const char *needle, const char *what) {
    for (size_t i = 0; i < array_length(frames); i++) {
        const char *got = frame_at(frames, i);
        if (got && strstr(got, needle)) return;
    }
    printf("FAIL: %s (no frame contains '%s')\n", what, needle);
    failures++;
}

/* Write `text` to `path`; the parent directory must exist. */
static void write_file_at(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("FAIL: cannot write %s\n", path);
        failures++;
        return;
    }
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}

int main(void) {
    const char *init_request =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
        "\"params\":{\"capabilities\":{}}}";
    char init_expected[512];
    snprintf(init_expected, sizeof init_expected,
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"capabilities\":{"
             "\"textDocumentSync\":1,\"hoverProvider\":true,\"definitionProvider\":true,"
             "\"completionProvider\":{\"triggerCharacters\":[\".\",\":\"]},"
             "\"documentSymbolProvider\":true,"
             "\"positionEncoding\":\"utf-16\"},"
             "\"serverInfo\":{\"name\":\"oby\",\"version\":\"%s\"}}}",
             oby_version_str);
    const char *publish_clean =
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\","
        "\"params\":{\"uri\":\"file:///tmp/t.oby\",\"diagnostics\":[]}}";

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
        check(array_length(frames) == 6, "open, change and close each publish once");
        expect_frame(frames, 0, init_expected, "the initialize response");
        expect_frame(frames, 1, publish_clean, "opening publishes the clean list");
        expect_frame(frames, 2, publish_clean, "a change publishes again");
        expect_frame(frames, 3, publish_clean, "closing clears them");
        expect_frame(frames, 4, "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":null}",
                     "shutdown responds null");
        expect_frame(frames, 5,
                     "{\"jsonrpc\":\"2.0\",\"id\":3,\"error\":{\"code\":-32600,"
                     "\"message\":\"server is shutting down\"}}",
                     "requests after shutdown are refused");
        free_frames(frames);
        free(s.data);
    }

    /* ---- diagnostics: an error appears, a fix clears it ---- */
    {
        Session s = {0};
        const char *publish_clear =
            "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\","
            "\"params\":{\"uri\":\"file:///tmp/bad.oby\",\"diagnostics\":[]}}";
        session_add(&s, init_request);
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":"
                        "{\"textDocument\":{\"uri\":\"file:///tmp/bad.oby\",\"languageId\":\"oby\","
                        "\"version\":1,\"text\":\"x = nope\\n\"}}}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\",\"params\":"
                        "{\"textDocument\":{\"uri\":\"file:///tmp/bad.oby\",\"version\":2},"
                        "\"contentChanges\":[{\"text\":\"x = 1\\n\"}]}}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didClose\",\"params\":"
                        "{\"textDocument\":{\"uri\":\"file:///tmp/bad.oby\"}}}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"shutdown\"}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");

        Array *frames;
        int status = run_session(&s, &frames);
        check(status == 0, "the diagnostics session shuts down cleanly");
        check(array_length(frames) == 5, "diagnostics are published per change");
        expect_frame(frames, 0, init_expected, "the initialize response");
        expect_contains(frames, 1, "\"uri\":\"file:///tmp/bad.oby\"",
                        "the diagnostics name the document");
        expect_contains(frames, 1, "\"range\":{\"start\":{\"line\":0,\"character\":4},"
                                   "\"end\":{\"line\":0,\"character\":8}}",
                        "the range covers the token");
        expect_contains(frames, 1, "\"severity\":1", "the severity is error");
        expect_contains(frames, 1, "\"source\":\"oby\"", "the source is oby");
        expect_contains(frames, 1, "unknown name 'nope'", "the message survives");
        expect_frame(frames, 2, publish_clear, "a fix clears the diagnostics");
        expect_frame(frames, 3, publish_clear, "closing clears them too");
        free_frames(frames);
        free(s.data);
    }

    /* ---- hover and definition through a real session ---- */
    {
        Session s = {0};
        const char *publish_clean_h =
            "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\","
            "\"params\":{\"uri\":\"file:///tmp/h.oby\",\"diagnostics\":[]}}";
        session_add(&s, init_request);
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":"
                        "{\"textDocument\":{\"uri\":\"file:///tmp/h.oby\",\"languageId\":\"oby\","
                        "\"version\":1,\"text\":\"f = (int a) -> int {\\n\\treturn a\\n}\\nx = f(1)\\n\"}}}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"textDocument/hover\","
                        "\"params\":{\"textDocument\":{\"uri\":\"file:///tmp/h.oby\"},"
                        "\"position\":{\"line\":1,\"character\":8}}}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"textDocument/definition\","
                        "\"params\":{\"textDocument\":{\"uri\":\"file:///tmp/h.oby\"},"
                        "\"position\":{\"line\":1,\"character\":8}}}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"textDocument/hover\","
                        "\"params\":{\"textDocument\":{\"uri\":\"file:///tmp/h.oby\"},"
                        "\"position\":{\"line\":3,\"character\":4}}}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"textDocument/definition\","
                        "\"params\":{\"textDocument\":{\"uri\":\"file:///tmp/h.oby\"},"
                        "\"position\":{\"line\":3,\"character\":4}}}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"textDocument/hover\","
                        "\"params\":{\"textDocument\":{\"uri\":\"file:///tmp/h.oby\"},"
                        "\"position\":{\"line\":2,\"character\":0}}}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"shutdown\"}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");

        Array *frames;
        int status = run_session(&s, &frames);
        check(status == 0, "the hover session shuts down cleanly");
        check(array_length(frames) == 8, "one frame per request");
        expect_frame(frames, 0, init_expected, "the initialize response names the providers");
        expect_frame(frames, 1, publish_clean_h, "the opened document is clean");
        expect_frame(frames, 2,
                     "{\"jsonrpc\":\"2.0\",\"id\":5,\"result\":{\"contents\":{"
                     "\"kind\":\"markdown\",\"value\":\"```oby\\nint\\n```\"}}}",
                     "hover shows a parameter's type");
        expect_frame(frames, 3,
                     "{\"jsonrpc\":\"2.0\",\"id\":6,\"result\":"
                     "{\"uri\":\"file:///tmp/h.oby\",\"range\":{"
                     "\"start\":{\"line\":0,\"character\":9},"
                     "\"end\":{\"line\":0,\"character\":10}}}}",
                     "definition jumps to the parameter");
        expect_frame(frames, 4,
                     "{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{\"contents\":{"
                     "\"kind\":\"markdown\",\"value\":\"```oby\\n(int) -> int\\n```\"}}}",
                     "hover shows a signature");
        expect_frame(frames, 5,
                     "{\"jsonrpc\":\"2.0\",\"id\":8,\"result\":"
                     "{\"uri\":\"file:///tmp/h.oby\",\"range\":{"
                     "\"start\":{\"line\":0,\"character\":0},"
                     "\"end\":{\"line\":0,\"character\":1}}}}",
                     "definition jumps to the declaration");
        expect_frame(frames, 6, "{\"jsonrpc\":\"2.0\",\"id\":9,\"result\":null}",
                     "hover over punctuation is null");
        expect_frame(frames, 7, "{\"jsonrpc\":\"2.0\",\"id\":10,\"result\":null}",
                     "shutdown responds null");
        free_frames(frames);
        free(s.data);
    }

    /* ---- completion: a bare name ---- */
    {
        Session s = {0};
        session_add(&s, init_request);
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":"
                        "{\"textDocument\":{\"uri\":\"file:///tmp/c.oby\",\"languageId\":\"oby\","
                        "\"version\":1,\"text\":\"alpha = 1\\nbeta = 2\\nal\\n\"}}}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"textDocument/completion\","
                        "\"params\":{\"textDocument\":{\"uri\":\"file:///tmp/c.oby\"},"
                        "\"position\":{\"line\":2,\"character\":2}}}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"shutdown\"}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");

        Array *frames;
        int status = run_session(&s, &frames);
        check(status == 0, "the completion session shuts down cleanly");
        check(array_length(frames) == 4, "one frame per request");
        expect_contains(frames, 1, "unknown name 'al'", "the incomplete name is diagnosed");
        expect_frame(frames, 2,
                     "{\"jsonrpc\":\"2.0\",\"id\":5,\"result\":[{\"label\":\"alpha\",\"kind\":6}]}",
                     "the prefix filters the candidates");
        free_frames(frames);
        free(s.data);
    }

    /* ---- completion: after a `.` (the receiver is an array) ---- */
    {
        Session s = {0};
        session_add(&s, init_request);
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":"
                        "{\"textDocument\":{\"uri\":\"file:///tmp/c2.oby\",\"languageId\":\"oby\","
                        "\"version\":1,\"text\":\"xs = [1, 2]\\nxs.\\n\"}}}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"textDocument/completion\","
                        "\"params\":{\"textDocument\":{\"uri\":\"file:///tmp/c2.oby\"},"
                        "\"position\":{\"line\":1,\"character\":3}}}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"shutdown\"}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");

        Array *frames;
        int status = run_session(&s, &frames);
        check(status == 0, "the member session shuts down cleanly");
        check(array_length(frames) == 4, "one frame per request");
        expect_frame(frames, 2,
                     "{\"jsonrpc\":\"2.0\",\"id\":5,\"result\":["
                     "{\"label\":\"insert\",\"kind\":2},"
                     "{\"label\":\"pop\",\"kind\":2},"
                     "{\"label\":\"push\",\"kind\":2},"
                     "{\"label\":\"remove\",\"kind\":2}]}",
                     "an array receiver offers the intrinsics");
        free_frames(frames);
        free(s.data);
    }

    /* ---- completion: after `::` (an enum's constants) ---- */
    {
        Session s = {0};
        session_add(&s, init_request);
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":"
                        "{\"textDocument\":{\"uri\":\"file:///tmp/c3.oby\",\"languageId\":\"oby\","
                        "\"version\":1,\"text\":\"Color = enum {\\n\\tRED = 1\\n\\tGREEN = 2\\n}\\nColor::\\n\"}}}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"textDocument/completion\","
                        "\"params\":{\"textDocument\":{\"uri\":\"file:///tmp/c3.oby\"},"
                        "\"position\":{\"line\":4,\"character\":7}}}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"shutdown\"}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");

        Array *frames;
        int status = run_session(&s, &frames);
        check(status == 0, "the static session shuts down cleanly");
        check(array_length(frames) == 4, "one frame per request");
        expect_frame(frames, 2,
                     "{\"jsonrpc\":\"2.0\",\"id\":5,\"result\":["
                     "{\"label\":\"GREEN\",\"kind\":20},"
                     "{\"label\":\"RED\",\"kind\":20}]}",
                     "an enum's constants are its statics");
        free_frames(frames);
        free(s.data);
    }

    /* ---- document symbols: the flat shape ---- */
    {
        Session s = {0};
        const char *publish_clean_s =
            "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\","
            "\"params\":{\"uri\":\"file:///tmp/s.oby\",\"diagnostics\":[]}}";
        session_add(&s, init_request);
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":"
                        "{\"textDocument\":{\"uri\":\"file:///tmp/s.oby\",\"languageId\":\"oby\","
                        "\"version\":1,\"text\":\"f = (int a) -> int {\\n\\treturn a\\n}\\ny = 1\\n\"}}}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"textDocument/documentSymbol\","
                        "\"params\":{\"textDocument\":{\"uri\":\"file:///tmp/s.oby\"}}}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"shutdown\"}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");

        Array *frames;
        int status = run_session(&s, &frames);
        check(status == 0, "the outline session shuts down cleanly");
        check(array_length(frames) == 4, "one frame per request");
        expect_frame(frames, 1, publish_clean_s, "the outline document is clean");
        expect_frame(frames, 2,
                     "{\"jsonrpc\":\"2.0\",\"id\":5,\"result\":["
                     "{\"name\":\"f\",\"kind\":12,\"location\":{\"uri\":\"file:///tmp/s.oby\","
                     "\"range\":{\"start\":{\"line\":0,\"character\":0},"
                     "\"end\":{\"line\":0,\"character\":1}}}},"
                     "{\"name\":\"y\",\"kind\":13,\"location\":{\"uri\":\"file:///tmp/s.oby\","
                     "\"range\":{\"start\":{\"line\":3,\"character\":0},"
                     "\"end\":{\"line\":3,\"character\":1}}}}]}",
                     "a flat outline without the hierarchy capability");
        free_frames(frames);
        free(s.data);
    }

    /* ---- document symbols: the hierarchical shape, with members ---- */
    {
        Session s = {0};
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"
                        "{\"capabilities\":{\"textDocument\":{\"documentSymbol\":"
                        "{\"hierarchicalDocumentSymbolSupport\":true}}}}}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":"
                        "{\"textDocument\":{\"uri\":\"file:///tmp/h.oby\",\"languageId\":\"oby\","
                        "\"version\":1,\"text\":\"Point = type {\\n\\tx = 0\\n\\t::count = 3\\n\\tget = () -> int { return x }\\n}\\n\"}}}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"textDocument/documentSymbol\","
                        "\"params\":{\"textDocument\":{\"uri\":\"file:///tmp/h.oby\"}}}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"shutdown\"}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");

        Array *frames;
        int status = run_session(&s, &frames);
        check(status == 0, "the hierarchical session shuts down cleanly");
        check(array_length(frames) == 4, "one frame per request");
        const char *body = frame_at(frames, 2);
        check(body && strstr(body, "\"name\":\"Point\",\"kind\":5") != NULL,
              "the type is a class");
        check(body && strstr(body, "\"children\":[{\"name\":\"x\",\"kind\":8") != NULL,
              "the fields are children");
        check(body && strstr(body, "{\"name\":\"count\",\"kind\":8") != NULL,
              "a static member is listed");
        check(body && strstr(body, "{\"name\":\"get\",\"kind\":6") != NULL,
              "a method is a method");
        check(body && strstr(body, "\"selectionRange\":{\"start\":{\"line\":3,\"character\":1},"
                                   "\"end\":{\"line\":3,\"character\":4}}") != NULL,
              "the method's selection covers its name");
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
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"id\":\"a\\\"b\",\"method\":\"foo/bar\"}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"method\":\"$/setTrace\",\"params\":{\"value\":\"off\"}}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"method\":\"my/note\"}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"shutdown\"}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");

        Array *frames;
        int status = run_session(&s, &frames);
        check(status == 0, "unknown methods do not disturb the session");
        check(array_length(frames) == 5, "unknown notifications are dropped");
        expect_frame(frames, 1,
                     "{\"jsonrpc\":\"2.0\",\"id\":5,\"error\":{\"code\":-32601,"
                     "\"message\":\"unknown method 'foo/bar'\"}}",
                     "an unknown request is MethodNotFound");
        expect_frame(frames, 2,
                     "{\"jsonrpc\":\"2.0\",\"id\":6,\"error\":{\"code\":-32601,"
                     "\"message\":\"unknown method '$/setTrace'\"}}",
                     "a `$/` request is MethodNotFound too");
        expect_frame(frames, 3,
                     "{\"jsonrpc\":\"2.0\",\"id\":\"a\\\"b\",\"error\":{\"code\":-32601,"
                     "\"message\":\"unknown method 'foo/bar'\"}}",
                     "a string id echoes back");
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
        expect_contains(frames, 0, "at byte ", "the parse error names the byte");
        expect_contains(frames, 0, "(line 1, column ", "the parse error names the place");
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

    /* ---- the escaping and URI layers underneath ---- */
    {
        char escaped[64];
        json_escape(escaped, sizeof escaped, "a\"b\\c\nd");
        check(strcmp(escaped, "a\\\"b\\\\c\\nd") == 0, "escaping round-trips");

        char path[256];
        check(uri_to_path("file:///tmp/a%20b.oby", path, sizeof path) &&
              strcmp(path, "/tmp/a b.oby") == 0, "a %20 URI decodes");
        check(!uri_to_path("http://x/y", path, sizeof path), "a non-file URI is refused");
        check(path_to_uri("/tmp/a b.oby", path, sizeof path) &&
              strcmp(path, "file:///tmp/a%20b.oby") == 0, "a path becomes a URI");
    }

    /* ---- position conversion, both encodings ---- */
    {
        /* A 2-byte, 1-unit character: byte and unit columns diverge after it
           (`s = "ß"`, with the offset just past the ß). */
        const char *src = "s = \"\xc3\x9f\"\n";
        size_t len = strlen(src);
        LspPosition p = lsp_position_of(src, len, 7, "utf-8");
        check(p.line == 0 && p.character == 7, "utf-8 counts bytes");
        p = lsp_position_of(src, len, 7, "utf-16");
        check(p.line == 0 && p.character == 6, "utf-16 counts code units");
        check(lsp_offset_of(src, len, p, "utf-16") == 7, "a utf-16 position converts back");
        p = lsp_position_of(src, len, len, "utf-8");
        check(p.line == 1 && p.character == 0, "the text's end is the next line");

        /* An emoji is one code point, four bytes, two UTF-16 units. */
        const char *emoji = "\xf0\x9f\x98\x80";
        size_t elen = strlen(emoji);
        p = lsp_position_of(emoji, elen, elen, "utf-16");
        check(p.line == 0 && p.character == 2, "an emoji is two utf-16 units");
        p = lsp_position_of(emoji, elen, elen, "utf-8");
        check(p.line == 0 && p.character == 4, "an emoji is four bytes");
        LspPosition pair = { 0, 2 };
        check(lsp_offset_of(emoji, elen, pair, "utf-16") == 4,
              "an emoji position converts back");
        LspPosition inside = { 0, 1 };
        check(lsp_offset_of(emoji, elen, inside, "utf-16") == 0,
              "a position inside a pair rounds down");

        /* A character past the line's content clamps to its end. */
        const char *short_line = "ab\n";
        size_t slen = strlen(short_line);
        p = lsp_position_of(short_line, slen, 2, "utf-8");
        check(p.line == 0 && p.character == 2, "a newline clamps to the line's end");
        LspPosition far = { 0, 9 };
        check(lsp_offset_of(short_line, slen, far, "utf-8") == 2,
              "an over-long character clamps");
        check(lsp_line_start(short_line, slen, 1) == 3, "line starts are found");
    }

    /* ---- the same-file test for tokens ---- */
    {
        const char *text = "abc";
        const char *other = "zzz";
        Token inside = {0};
        inside.start = text + 1;
        inside.length = 1;
        Token outside = {0};
        outside.start = other;
        outside.length = 1;
        check(lsp_token_in_source(text, 3, inside), "a token inside the source");
        check(!lsp_token_in_source(text, 3, outside),
              "a token from another buffer is refused");
    }

    /* ---- diagnostics for an imported file ---- */
    {
        const char *dir = "/tmp/oby_lsp_xfile";
        mkdir(dir, 0755);
        write_file_at("/tmp/oby_lsp_xfile/main.oby", "use \"lib.oby\"\nx = 1\n");
        write_file_at("/tmp/oby_lsp_xfile/lib.oby", "y = nope\n");

        Session s = {0};
        session_add(&s, init_request);
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":"
                        "{\"textDocument\":{\"uri\":\"file:///tmp/oby_lsp_xfile/main.oby\","
                        "\"languageId\":\"oby\",\"version\":1,"
                        "\"text\":\"use \\\"lib.oby\\\"\\nx = 1\\n\"}}}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"shutdown\"}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");

        Array *frames;
        int status = run_session(&s, &frames);
        check(status == 0, "the imported-diagnostics session shuts down cleanly");
        expect_any_frame(frames, "\"uri\":\"file:///tmp/oby_lsp_xfile/lib.oby\"",
                         "an import's diagnostics name its own URI");
        expect_any_frame(frames, "unknown name 'nope'", "the import's error survives");
        expect_any_frame(frames, "\"uri\":\"file:///tmp/oby_lsp_xfile/main.oby\"",
                         "the entry is still published");
        free_frames(frames);
        free(s.data);
    }

    /* ---- a definition that lives in an imported file ---- */
    {
        const char *dir = "/tmp/oby_lsp_xfile";
        mkdir(dir, 0755);
        write_file_at("/tmp/oby_lsp_xfile/def_main.oby", "use \"def_lib.oby\"\nx = helper(1)\n");
        write_file_at("/tmp/oby_lsp_xfile/def_lib.oby", "helper = (int a) -> int { return a }\n");

        Session s = {0};
        session_add(&s, init_request);
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":"
                        "{\"textDocument\":{\"uri\":\"file:///tmp/oby_lsp_xfile/def_main.oby\","
                        "\"languageId\":\"oby\",\"version\":1,"
                        "\"text\":\"use \\\"def_lib.oby\\\"\\nx = helper(1)\\n\"}}}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"textDocument/definition\","
                        "\"params\":{\"textDocument\":{\"uri\":\"file:///tmp/oby_lsp_xfile/def_main.oby\"},"
                        "\"position\":{\"line\":1,\"character\":4}}}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"shutdown\"}");
        session_add(&s, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");

        Array *frames;
        int status = run_session(&s, &frames);
        check(status == 0, "the cross-file definition session shuts down cleanly");
        expect_any_frame(frames,
                         "{\"jsonrpc\":\"2.0\",\"id\":5,\"result\":"
                         "{\"uri\":\"file:///tmp/oby_lsp_xfile/def_lib.oby\",\"range\":{"
                         "\"start\":{\"line\":0,\"character\":0},"
                         "\"end\":{\"line\":0,\"character\":6}}}}",
                         "definition jumps into the imported file");
        free_frames(frames);
        free(s.data);
    }

    remove("/tmp/oby_lsp_xfile/main.oby");
    remove("/tmp/oby_lsp_xfile/lib.oby");
    remove("/tmp/oby_lsp_xfile/def_main.oby");
    remove("/tmp/oby_lsp_xfile/def_lib.oby");
    rmdir("/tmp/oby_lsp_xfile");

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("lsp ok\n");
    return 0;
}
