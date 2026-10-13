#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/version.h"
#include "lsp/json.h"
#include "lsp/server.h"
#include "lsp/uri.h"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

/* An open document: the URI exactly as the client sent it (it goes back out
   verbatim), the path behind it when it is a file, and the editor's text. The
   text is the truth while the document is open — the file on disk may be
   stale. */
typedef struct LspDocument {
    char *uri;
    char *path;
    long long version;
    char *source;         /* owned */
    size_t source_len;
    struct LspDocument *next;
} LspDocument;

typedef struct {
    FILE *in;
    FILE *out;
    bool initialized;
    bool shutdown_received;
    const char *encoding;   /* the negotiated position encoding, §4.1 */
    LspDocument *docs;
} LspServer;

/* A malloc'd copy of `s`. */
static char *copy_string(const char *s) {
    size_t len = strlen(s);
    char *copy = (char *)malloc(len + 1);
    if (copy) memcpy(copy, s, len + 1);
    return copy;
}

/* ------------------------------------------------------------------ */
/* Responses                                                          */
/* ------------------------------------------------------------------ */

/* `id` is the request id's source text, echoed verbatim so any id shape
   (number, string with escapes, ...) survives. NULL means "no id at all",
   which only a parse error can have: it is reported as `"id":null`. */
static void send_result(LspServer *s, const char *id, size_t id_len,
                        const char *result_json) {
    if (!id) {
        id = "null";
        id_len = 4;
    }
    frame_writef(s->out, "{\"jsonrpc\":\"2.0\",\"id\":%.*s,\"result\":%s}",
                 (int)id_len, id, result_json);
}

static void send_error(LspServer *s, const char *id, size_t id_len, int code,
                       const char *message) {
    if (!id) {
        id = "null";
        id_len = 4;
    }
    char escaped[768];
    json_escape(escaped, sizeof escaped, message);
    frame_writef(s->out,
                 "{\"jsonrpc\":\"2.0\",\"id\":%.*s,"
                 "\"error\":{\"code\":%d,\"message\":\"%s\"}}",
                 (int)id_len, id, code, escaped);
}

/* ------------------------------------------------------------------ */
/* Documents                                                          */
/* ------------------------------------------------------------------ */

static LspDocument *doc_find(LspServer *s, const char *uri) {
    for (LspDocument *doc = s->docs; doc; doc = doc->next) {
        if (strcmp(doc->uri, uri) == 0) return doc;
    }
    return NULL;
}

static void doc_free(LspDocument *doc) {
    free(doc->uri);
    free(doc->path);
    free(doc->source);
    free(doc);
}

static void doc_remove(LspServer *s, const char *uri) {
    LspDocument **link = &s->docs;
    while (*link) {
        if (strcmp((*link)->uri, uri) == 0) {
            LspDocument *dead = *link;
            *link = dead->next;
            doc_free(dead);
            return;
        }
        link = &(*link)->next;
    }
}

static void doc_open(LspServer *s, const JsonValue *params) {
    const JsonValue *td = json_get(params, "textDocument");
    const char *uri = json_string(json_get(td, "uri"));
    const char *text = json_string(json_get(td, "text"));
    if (!uri || !text) return;   /* a malformed notification is dropped */

    /* A re-open without a close starts fresh. */
    doc_remove(s, uri);

    LspDocument *doc = (LspDocument *)calloc(1, sizeof *doc);
    if (!doc) return;
    doc->uri = copy_string(uri);
    if (!doc->uri) {
        doc_free(doc);
        return;
    }

    char path[4096];
    if (uri_to_path(uri, path, sizeof path)) doc->path = copy_string(path);

    long long version = 0;
    json_int(json_get(td, "version"), &version);
    doc->version = version;

    doc->source = copy_string(text);
    if (!doc->source) {
        doc_free(doc);
        return;
    }
    doc->source_len = strlen(doc->source);

    doc->next = s->docs;
    s->docs = doc;
}

static void doc_change(LspServer *s, const JsonValue *params) {
    const JsonValue *td = json_get(params, "textDocument");
    const char *uri = json_string(json_get(td, "uri"));
    if (!uri) return;
    LspDocument *doc = doc_find(s, uri);
    if (!doc) return;   /* changes only arrive for open documents */

    const JsonValue *changes = json_get(params, "contentChanges");
    if (!changes || changes->kind != JSON_ARRAY || array_length(changes->items) == 0) return;

    /* We advertise Full sync, so an event is the whole document. A ranged
       edit here would silently go unapplied; drop the change instead. */
    const JsonValue *last = NULL;
    for (size_t i = 0; i < array_length(changes->items); i++) {
        const JsonValue *event = *(JsonValue **)array_get(changes->items, i);
        if (json_get(event, "range")) return;
        last = event;
    }
    const char *text = json_string(json_get(last, "text"));
    if (!text) return;

    char *copy = copy_string(text);
    if (!copy) return;
    free(doc->source);
    doc->source = copy;
    doc->source_len = strlen(copy);

    /* The spec: this version is the one after all the changes in the list. */
    long long version = doc->version;
    json_int(json_get(td, "version"), &version);
    doc->version = version;
}

static void doc_close(LspServer *s, const JsonValue *params) {
    const JsonValue *td = json_get(params, "textDocument");
    const char *uri = json_string(json_get(td, "uri"));
    if (uri) doc_remove(s, uri);
    /* Phase 1b publishes an empty diagnostics list here: clearing on close is
       the spec's rule for a single-file language. */
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */

static void handle_initialize(LspServer *s, const char *id, size_t id_len,
                              const JsonValue *params) {
    /* Position encoding (3.17): the client lists its preference in
       `general.positionEncodings`; only an encoding it offered is legal to
       return. Without a list, utf-16 is the only valid answer. */
    const char *encoding = "utf-16";
    const JsonValue *capabilities = json_get(params, "capabilities");
    const JsonValue *general = json_get(capabilities, "general");
    const JsonValue *offered = json_get(general, "positionEncodings");
    if (offered && offered->kind == JSON_ARRAY) {
        for (size_t i = 0; i < array_length(offered->items); i++) {
            const char *name = json_string(*(JsonValue **)array_get(offered->items, i));
            if (name && strcmp(name, "utf-8") == 0) {
                encoding = "utf-8";
                break;
            }
            if (name && strcmp(name, "utf-16") == 0) break;   /* the default */
        }
    }
    s->encoding = encoding;
    s->initialized = true;

    /* The providers grow as the phases land: hover and definition in Phase 2,
       completion and documentSymbol in Phase 3. */
    frame_writef(s->out,
                 "{\"jsonrpc\":\"2.0\",\"id\":%.*s,\"result\":{\"capabilities\":{"
                 "\"textDocumentSync\":1,"
                 "\"positionEncoding\":\"%s\"},"
                 "\"serverInfo\":{\"name\":\"oby\",\"version\":\"%s\"}}}",
                 (int)id_len, id, s->encoding, oby_version_str);
}

/* Handle one parsed message. Returns true when `exit` arrived. */
static bool handle_message(LspServer *s, JsonValue *msg) {
    const char *method = json_string(json_get(msg, "method"));
    const JsonValue *idv = json_get(msg, "id");
    bool is_request = idv != NULL;
    const char *id = is_request ? idv->raw : NULL;
    size_t id_len = is_request ? idv->raw_len : 0;

    if (!method) {
        if (is_request) send_error(s, id, id_len, -32600, "a request needs a method");
        return false;
    }

    /* Before `initialize`: requests are answered with ServerNotInitialized,
       notifications are dropped — except `exit`, which must always work. */
    if (!s->initialized &&
        strcmp(method, "initialize") != 0 && strcmp(method, "exit") != 0) {
        if (is_request) send_error(s, id, id_len, -32002, "server not initialized");
        return false;
    }

    /* After `shutdown` the only useful message is `exit`. */
    if (s->shutdown_received && strcmp(method, "exit") != 0) {
        if (is_request) send_error(s, id, id_len, -32600, "server is shutting down");
        return false;
    }

    if (strcmp(method, "initialize") == 0) {
        if (!is_request) return false;
        if (s->initialized) {
            send_error(s, id, id_len, -32600, "the server is already initialized");
            return false;
        }
        handle_initialize(s, id, id_len, json_get(msg, "params"));
        return false;
    }
    if (strcmp(method, "initialized") == 0) return false;

    if (strcmp(method, "shutdown") == 0) {
        if (!is_request) return false;
        s->shutdown_received = true;
        send_result(s, id, id_len, "null");
        return false;
    }
    if (strcmp(method, "exit") == 0) return true;

    if (strcmp(method, "textDocument/didOpen") == 0) {
        doc_open(s, json_get(msg, "params"));
        return false;
    }
    if (strcmp(method, "textDocument/didChange") == 0) {
        doc_change(s, json_get(msg, "params"));
        return false;
    }
    if (strcmp(method, "textDocument/didClose") == 0) {
        doc_close(s, json_get(msg, "params"));
        return false;
    }

    /* Anything else: a request gets MethodNotFound — `$/`-prefixed requests
       included, the spec insists — and a notification is dropped. */
    if (is_request) {
        char note[256];
        snprintf(note, sizeof note, "unknown method '%s'", method);
        send_error(s, id, id_len, -32601, note);
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* The loop                                                           */
/* ------------------------------------------------------------------ */

int lsp_server_run(FILE *in, FILE *out) {
#if defined(_WIN32)
    /* Framing is byte-exact: text mode would rewrite the CRLFs. */
    _setmode(_fileno(in), _O_BINARY);
    _setmode(_fileno(out), _O_BINARY);
#endif

    LspServer srv;
    memset(&srv, 0, sizeof srv);
    srv.in = in;
    srv.out = out;
    srv.encoding = "utf-16";

    bool quit = false;
    while (!quit) {
        size_t len = 0;
        char *body = frame_read(in, &len);
        if (!body) break;   /* the client went away, or the frame was broken */

        Arena arena;
        arena_init(&arena);
        const char *err = NULL;
        JsonValue *msg = json_parse(&arena, body, &err);

        if (!msg) {
            char note[256];
            snprintf(note, sizeof note, "invalid JSON: %s", err ? err : "parse error");
            send_error(&srv, NULL, 0, -32700, note);
        } else if (msg->kind != JSON_OBJECT) {
            send_error(&srv, NULL, 0, -32600, "a JSON-RPC message must be an object");
        } else {
            quit = handle_message(&srv, msg);
        }

        arena_free(&arena);
        free(body);
    }

    LspDocument *doc = srv.docs;
    while (doc) {
        LspDocument *next = doc->next;
        doc_free(doc);
        doc = next;
    }
    return srv.shutdown_received ? 0 : 1;
}
