#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/version.h"
#include "compiler/frontend.h"
#include "compiler/type.h"
#include "lsp/json.h"
#include "lsp/query.h"
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
    char *source;         /* borrowed from `fe`: the buffer its tokens point into */
    size_t source_len;
    Frontend fe;          /* the checked program; its diag captures diagnostics */
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
    frontend_free(&doc->fe);   /* frees the source buffer too */
    free(doc->uri);
    free(doc->path);
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

/* The name this document is checked under: its path when it has one — so
   relative `use` imports resolve against the right directory — else its URI.
   Diagnostics recorded against this file belong to this document. */
static const char *doc_label(LspDocument *doc) {
    return doc->path ? doc->path : doc->uri;
}

/* Parse and typecheck `text` with capture on, replacing the previous run (its
   arena goes first, like the CLI's Frontend cycles). The frontend owns the
   source buffer from here on and `doc->source` borrows it: the AST's tokens
   point into that buffer, so queries must use the very same one. */
static void doc_check(LspDocument *doc, const char *text) {
    frontend_free(&doc->fe);
    diag_set_capture(&doc->fe.diag, true);
    char *copy = copy_string(text);
    if (!copy) {
        doc->source = NULL;
        doc->source_len = 0;
        return;
    }
    frontend_run(&doc->fe, doc_label(doc), copy);
    doc->source = doc->fe.source;
    doc->source_len = doc->source ? strlen(doc->source) : 0;
}

/* Publish one document's diagnostics. `doc` NULL publishes the empty list,
   which is how a closed document's squigglies are cleared. v1 publishes the
   entry document's own diagnostics only: an imported file — and the embedded
   prelude — gets its pass when it is opened itself. */
static void send_diagnostics(LspServer *s, const char *uri, LspDocument *doc) {
    const char *label = doc ? doc_label(doc) : NULL;
    Array *entries = doc ? doc->fe.diag.entries : NULL;

    JsonBuf b;
    json_buf_init(&b);
    json_buf_printf(&b,
                    "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\","
                    "\"params\":{\"uri\":\"");
    json_buf_escape(&b, uri);
    json_buf_printf(&b, "\",\"diagnostics\":[");

    bool first = true;
    for (size_t i = 0; entries && i < array_length(entries); i++) {
        DiagEntry *e = (DiagEntry *)array_get(entries, i);
        if (!e->file || strcmp(e->file, label) != 0) continue;

        /* The captured line and column are 1-based byte positions; the
           protocol wants zero-based positions in the negotiated encoding.
           Zero-length tokens get a 1-byte span so a caret can render. */
        size_t start = lsp_line_start(doc->source, doc->source_len, e->line - 1);
        if (e->column > 0) start += (size_t)(e->column - 1);
        if (start > doc->source_len) start = doc->source_len;
        size_t end = start + (e->length > 0 ? e->length : 1);
        if (end > doc->source_len) end = doc->source_len;

        LspPosition at = lsp_position_of(doc->source, doc->source_len, start, s->encoding);
        LspPosition to = lsp_position_of(doc->source, doc->source_len, end, s->encoding);

        json_buf_printf(&b,
                        "%s{\"range\":{\"start\":{\"line\":%d,\"character\":%d},"
                        "\"end\":{\"line\":%d,\"character\":%d}},"
                        "\"severity\":%d,\"source\":\"oby\",\"message\":\"",
                        first ? "" : ",",
                        at.line, at.character, to.line, to.character, e->severity);
        json_buf_escape(&b, e->message);
        json_buf_printf(&b, "\"}");
        first = false;
    }
    json_buf_printf(&b, "]}}");

    if (!b.oom) frame_write(s->out, b.data);
    json_buf_free(&b);
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

    doc->next = s->docs;
    s->docs = doc;

    doc_check(doc, text);
    send_diagnostics(s, doc->uri, doc);
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

    /* The spec: this version is the one after all the changes in the list. */
    long long version = doc->version;
    json_int(json_get(td, "version"), &version);
    doc->version = version;

    doc_check(doc, text);
    send_diagnostics(s, doc->uri, doc);
}

static void doc_close(LspServer *s, const JsonValue *params) {
    const JsonValue *td = json_get(params, "textDocument");
    const char *uri = json_string(json_get(td, "uri"));
    if (!uri) return;
    /* Clearing on close is the spec's rule for a single-file language: with
       the buffer gone the disk file is the truth, and it may differ. */
    send_diagnostics(s, uri, NULL);
    doc_remove(s, uri);
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

    /* The providers grow as the phases land: completion and documentSymbol
       arrive in Phase 3. */
    frame_writef(s->out,
                 "{\"jsonrpc\":\"2.0\",\"id\":%.*s,\"result\":{\"capabilities\":{"
                 "\"textDocumentSync\":1,"
                 "\"hoverProvider\":true,"
                 "\"definitionProvider\":true,"
                 "\"positionEncoding\":\"%s\"},"
                 "\"serverInfo\":{\"name\":\"oby\",\"version\":\"%s\"}}}",
                 (int)id_len, id, s->encoding, oby_version_str);
}

/* The document and byte offset a position request names. False when the
   document is not open or the params do not parse. */
static bool position_params(LspServer *s, const JsonValue *params,
                            LspDocument **out_doc, size_t *out_offset) {
    const JsonValue *td = json_get(params, "textDocument");
    const char *uri = json_string(json_get(td, "uri"));
    if (!uri) return false;
    LspDocument *doc = doc_find(s, uri);
    if (!doc) return false;

    long long line = 0, character = 0;
    const JsonValue *pos = json_get(params, "position");
    json_int(json_get(pos, "line"), &line);
    json_int(json_get(pos, "character"), &character);
    if (line < 0 || line > 0x7FFFFFFFLL || character < 0 || character > 0x7FFFFFFFLL)
        return false;

    LspPosition where = { (int)line, (int)character };
    *out_doc = doc;
    *out_offset = lsp_offset_of(doc->source, doc->source_len, where, s->encoding);
    return true;
}

/* The target under the request's position, with the spec's nudge: clients put
   the position just left of the character being pointed at. */
static LspTarget target_at(LspDocument *doc, size_t offset) {
    LspTarget target = lsp_target_at(doc->fe.program, doc->source, doc->source_len, offset);
    if (!target.node && offset > 0)
        target = lsp_target_at(doc->fe.program, doc->source, doc->source_len, offset - 1);
    return target;
}

static void handle_hover(LspServer *s, const char *id, size_t id_len,
                         const JsonValue *params) {
    LspDocument *doc = NULL;
    size_t offset = 0;
    if (!position_params(s, params, &doc, &offset) || !doc->fe.program) {
        send_result(s, id, id_len, "null");
        return;
    }

    LspTarget target = target_at(doc, offset);
    Type *type = target.type ? target.type : (target.sym ? target.sym->type : NULL);
    if (!type || type->kind == TYPE_UNKNOWN) {
        send_result(s, id, id_len, "null");
        return;
    }

    char name[256];
    type_str(type, name, sizeof name);

    JsonBuf b;
    json_buf_init(&b);
    json_buf_printf(&b, "{\"contents\":{\"kind\":\"markdown\",\"value\":\"```oby\\n");
    json_buf_escape(&b, name);
    json_buf_printf(&b, "\\n```\"}}");
    if (!b.oom) send_result(s, id, id_len, b.data);
    json_buf_free(&b);
}

static void handle_definition(LspServer *s, const char *id, size_t id_len,
                              const JsonValue *params) {
    LspDocument *doc = NULL;
    size_t offset = 0;
    if (!position_params(s, params, &doc, &offset) || !doc->fe.program) {
        send_result(s, id, id_len, "null");
        return;
    }

    Token tok;
    LspTarget target = target_at(doc, offset);
    /* Same-file only: the bounds check refuses tokens from imports and the
       prelude, so the jump never leaves the document. */
    if (!lsp_target_decl_token(&target, &tok) ||
        !lsp_token_in_source(doc->source, doc->source_len, tok)) {
        send_result(s, id, id_len, "null");
        return;
    }

    size_t start = (size_t)(tok.start - doc->source);
    size_t end = start + (tok.length > 0 ? tok.length : 1);
    if (end > doc->source_len) end = doc->source_len;
    LspPosition at = lsp_position_of(doc->source, doc->source_len, start, s->encoding);
    LspPosition to = lsp_position_of(doc->source, doc->source_len, end, s->encoding);

    JsonBuf b;
    json_buf_init(&b);
    json_buf_printf(&b, "{\"uri\":\"");
    json_buf_escape(&b, doc->uri);
    json_buf_printf(&b,
                    "\",\"range\":{\"start\":{\"line\":%d,\"character\":%d},"
                    "\"end\":{\"line\":%d,\"character\":%d}}}",
                    at.line, at.character, to.line, to.character);
    if (!b.oom) send_result(s, id, id_len, b.data);
    json_buf_free(&b);
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

    if (strcmp(method, "textDocument/hover") == 0) {
        if (is_request) handle_hover(s, id, id_len, json_get(msg, "params"));
        return false;
    }
    if (strcmp(method, "textDocument/definition") == 0) {
        if (is_request) handle_definition(s, id, id_len, json_get(msg, "params"));
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
