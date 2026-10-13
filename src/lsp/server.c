#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "core/json_error.h"
#include "core/version.h"
#include "compiler/frontend.h"
#include "compiler/type.h"
#include "lsp/json.h"
#include "lsp/query.h"
#include "lsp/server.h"
#include "lsp/uri.h"

#include "json.h"   /* vendor/json.h — request parsing, public domain */

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
    bool hierarchical_symbols;   /* the client's documentSymbol shape */
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
/* Reading requests (vendor/json.h)                                   */
/* ------------------------------------------------------------------ */

typedef struct json_value_s JValue;

/* The member `key` of an object value, or NULL. */
static JValue *jmem(JValue *value, const char *key) {
    struct json_object_s *object = value ? json_value_as_object(value) : NULL;
    if (!object) return NULL;
    for (struct json_object_element_s *e = object->start; e; e = e->next) {
        if (e->name && strcmp(e->name->string, key) == 0) return e->value;
    }
    return NULL;
}

/* A string's bytes, or NULL. */
static const char *jstr(JValue *value) {
    struct json_string_s *s = value ? json_value_as_string(value) : NULL;
    return s ? s->string : NULL;
}

/* An array, or NULL — json.h's `json_value_as_*` helpers dereference without a
   NULL check, so every one goes through a guard like this. */
static struct json_array_s *jarr(JValue *value) {
    return value ? json_value_as_array(value) : NULL;
}

/* A boolean, when it is one. */
static bool jbool(JValue *value, bool *out) {
    if (!value) return false;
    if (json_value_is_true(value)) {
        *out = true;
        return true;
    }
    if (json_value_is_false(value)) {
        *out = false;
        return true;
    }
    return false;
}

/* The value of a number that is an exact integer. */
static bool jint(JValue *value, long long *out) {
    struct json_number_s *number = value ? json_value_as_number(value) : NULL;
    if (!number || number->number_size == 0 || number->number_size >= 32) return false;
    char buf[32];
    memcpy(buf, number->number, number->number_size);
    buf[number->number_size] = '\0';
    double d = strtod(buf, NULL);
    long long i = (long long)d;
    if ((double)i != d) return false;
    *out = i;
    return true;
}

/* The parsed body (one allocation; the caller frees it), or NULL with `note`
   naming the failure and where it is. */
static JValue *jparse(const char *body, size_t len, char *note, size_t cap) {
    struct json_parse_result_s result;
    memset(&result, 0, sizeof result);
    JValue *root = json_parse_ex(body, len, json_parse_flags_default, NULL, NULL, &result);
    if (!root) {
        char message[128];
        oby_json_error_message(message, sizeof message, result.error, result.error_offset,
                               result.error_line_no, result.error_row_no + 1);
        snprintf(note, cap, "invalid JSON: %s", message);
    }
    return root;
}

/* The request id to echo: a number keeps its source text; anything else is
   written back out (a client correlates an id by value, not by bytes). */
static void jid(JValue *value, JsonBuf *out) {
    if (!value) {
        json_buf_printf(out, "null");
        return;
    }
    struct json_number_s *number = json_value_as_number(value);
    if (number && number->number) {
        json_buf_printf(out, "%.*s", (int)number->number_size, number->number);
        return;
    }
    size_t len = 0;
    char *text = (char *)json_write_minified(value, &len);
    if (!text) {
        json_buf_printf(out, "null");
        return;
    }
    json_buf_printf(out, "%s", text);
    free(text);
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

/* Does this path name a directory? Imports resolve against a document's
   directory, which is there even for a file the editor has not saved yet. */
static bool dir_exists(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

/* `path`'s directory, in `buf`. */
static void parent_dir(const char *path, char *buf, size_t cap) {
    snprintf(buf, cap, "%s", path);
    char *slash = strrchr(buf, '/');
    if (slash) *slash = '\0';
    else buf[0] = '\0';
}

/* `/C:/Users/x` (how a Windows client's `file:///c%3A/…` URI decodes here)
   names `/mnt/c/Users/x` when this side is WSL. False when the path is not a
   drive path. */
static bool wsl_drive_path(const char *path, char *buf, size_t cap) {
    if (path[0] != '/' || path[1] == '\0' || path[2] != ':' || path[3] != '/') return false;
    char drive = path[1];
    if (drive >= 'A' && drive <= 'Z') drive = (char)(drive - 'A' + 'a');
    if (drive < 'a' || drive > 'z') return false;
    int n = snprintf(buf, cap, "/mnt/%c/%s", drive, path + 4);
    return n > 0 && (size_t)n < cap;
}

/* The filesystem path a document's URI names on this side: the decoded URI,
   or its WSL spelling when only that one has a directory here. */
static char *document_path(const char *uri) {
    char path[4096];
    if (!uri_to_path(uri, path, sizeof path)) return NULL;

    char dir[4096];
    parent_dir(path, dir, sizeof dir);
    if (!dir_exists(dir)) {
        char translated[4096];
        char translated_dir[4096];
        if (wsl_drive_path(path, translated, sizeof translated)) {
            parent_dir(translated, translated_dir, sizeof translated_dir);
            if (dir_exists(translated_dir)) return copy_string(translated);
        }
    }
    return copy_string(path);
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

static void doc_open(LspServer *s, JValue *params) {
    JValue *td = jmem(params, "textDocument");
    const char *uri = jstr(jmem(td, "uri"));
    const char *text = jstr(jmem(td, "text"));
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

    doc->path = document_path(uri);

    long long version = 0;
    jint(jmem(td, "version"), &version);
    doc->version = version;

    doc->next = s->docs;
    s->docs = doc;

    doc_check(doc, text);
    send_diagnostics(s, doc->uri, doc);
}

static void doc_change(LspServer *s, JValue *params) {
    JValue *td = jmem(params, "textDocument");
    const char *uri = jstr(jmem(td, "uri"));
    if (!uri) return;
    LspDocument *doc = doc_find(s, uri);
    if (!doc) return;   /* changes only arrive for open documents */

    struct json_array_s *changes = jarr(jmem(params, "contentChanges"));
    if (!changes || changes->length == 0) return;

    /* We advertise Full sync, so an event is the whole document. A ranged
       edit here would silently go unapplied; drop the change instead. */
    JValue *last = NULL;
    for (struct json_array_element_s *e = changes->start; e; e = e->next) {
        if (jmem(e->value, "range")) return;
        last = e->value;
    }
    const char *text = jstr(jmem(last, "text"));
    if (!text) return;

    /* The spec: this version is the one after all the changes in the list. */
    long long version = doc->version;
    jint(jmem(td, "version"), &version);
    doc->version = version;

    doc_check(doc, text);
    send_diagnostics(s, doc->uri, doc);
}

static void doc_close(LspServer *s, JValue *params) {
    JValue *td = jmem(params, "textDocument");
    const char *uri = jstr(jmem(td, "uri"));
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
                              JValue *params) {
    /* Position encoding (3.17): the client lists its preference in
       `general.positionEncodings`; only an encoding it offered is legal to
       return. Without a list, utf-16 is the only valid answer. */
    const char *encoding = "utf-16";
    JValue *capabilities = jmem(params, "capabilities");
    JValue *general = jmem(capabilities, "general");
    struct json_array_s *offered = jarr(jmem(general, "positionEncodings"));
    for (struct json_array_element_s *e = offered ? offered->start : NULL; e; e = e->next) {
        const char *name = jstr(e->value);
        if (name && strcmp(name, "utf-8") == 0) {
            encoding = "utf-8";
            break;
        }
        if (name && strcmp(name, "utf-16") == 0) break;   /* the default */
    }
    /* The documentSymbol shape depends on what the client can take. */
    JValue *text_caps = jmem(capabilities, "textDocument");
    JValue *ds_caps = jmem(text_caps, "documentSymbol");
    bool hierarchical = false;
    if (jbool(jmem(ds_caps, "hierarchicalDocumentSymbolSupport"), &hierarchical))
        s->hierarchical_symbols = hierarchical;

    s->encoding = encoding;
    s->initialized = true;

    frame_writef(s->out,
                 "{\"jsonrpc\":\"2.0\",\"id\":%.*s,\"result\":{\"capabilities\":{"
                 "\"textDocumentSync\":1,"
                 "\"hoverProvider\":true,"
                 "\"definitionProvider\":true,"
                 "\"completionProvider\":{\"triggerCharacters\":[\".\",\":\"]},"
                 "\"documentSymbolProvider\":true,"
                 "\"positionEncoding\":\"%s\"},"
                 "\"serverInfo\":{\"name\":\"oby\",\"version\":\"%s\"}}}",
                 (int)id_len, id, s->encoding, oby_version_str);
}

/* The document and byte offset a position request names. False when the
   document is not open or the params do not parse. */
static bool position_params(LspServer *s, JValue *params,
                            LspDocument **out_doc, size_t *out_offset) {
    JValue *td = jmem(params, "textDocument");
    const char *uri = jstr(jmem(td, "uri"));
    if (!uri) return false;
    LspDocument *doc = doc_find(s, uri);
    if (!doc) return false;

    long long line = 0, character = 0;
    JValue *pos = jmem(params, "position");
    jint(jmem(pos, "line"), &line);
    jint(jmem(pos, "character"), &character);
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
                         JValue *params) {
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
                              JValue *params) {
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

/* ------------------------------------------------------------------ */
/* Completion                                                         */
/* ------------------------------------------------------------------ */

/* CompletionItemKind values for the items the server mints. */
enum {
    ITEM_METHOD = 2,
    ITEM_FUNCTION = 3,
    ITEM_FIELD = 5,
    ITEM_VARIABLE = 6,
    ITEM_CLASS = 7,
    ITEM_KEYWORD = 14,
    ITEM_ENUM_MEMBER = 20,
    ITEM_CONSTANT = 21
};

typedef struct {
    const char *name;
    int kind;
} LspItem;

static int item_cmp(const void *a, const void *b) {
    const LspItem *x = (const LspItem *)a;
    const LspItem *y = (const LspItem *)b;
    return strcmp(x->name, y->name);
}

/* Add one candidate, keeping the first sighting of a name. */
static void item_add(Array *items, const char *name, int kind) {
    if (!name || !name[0]) return;
    for (size_t i = 0; i < array_length(items); i++) {
        LspItem *item = (LspItem *)array_get(items, i);
        if (strcmp(item->name, name) == 0) return;
    }
    LspItem item = { name, kind };
    array_push(items, &item);
}

static int value_item_kind(const Symbol *sym) {
    return (sym->type && sym->type->kind == TYPE_FUNC) ? ITEM_FUNCTION : ITEM_VARIABLE;
}

/* A static member that names the receiver: `find = (String s, ...)` is called
   as `s.find(...)`. */
static bool receiver_static(const Symbol *sym, const Type *type) {
    return sym->is_static && sym->type && sym->type->kind == TYPE_FUNC &&
           sym->type->param_count >= 1 && sym->type->params[0] == type;
}

/* A bare name's symbol: locals and parameters shadow the globals. */
static Symbol *resolve_name(Scope *global, const char *source, size_t source_len,
                            size_t offset, const char *name, ASTNode *program) {
    Symbol *found = NULL;
    Array *locals = array_new(sizeof(Symbol *));
    if (locals) {
        lsp_visible_bindings(program, source, source_len, offset, locals);
        for (size_t i = 0; i < array_length(locals); i++) {
            Symbol *sym = *(Symbol **)array_get(locals, i);
            if (sym && sym->name && strcmp(sym->name, name) == 0) found = sym;
        }
        array_free(locals);
    }
    if (!found) found = scope_lookup(global, name);
    return found;
}

/* The checked program to answer a completion against. When the document's own
   pass stopped at the parser, its program has no types; re-checking a copy
   with a sentinel byte spliced in at the cursor (`if x {` stays broken, but
   `xs.` becomes `xs._`) recovers the types for everything before it. Returns
   true when `repaired` must be freed. */
static bool completion_program(LspDocument *doc, size_t offset, Frontend *repaired,
                               const char **source, size_t *source_len,
                               ASTNode **program, Scope **global) {
    /* A global scope means sema ran, so the document parses. */
    if (doc->fe.program && doc->fe.sema.global) {
        *source = doc->source;
        *source_len = doc->source_len;
        *program = doc->fe.program;
        *global = doc->fe.sema.global;
        return false;
    }

    size_t len = doc->source_len;
    char *patched = (char *)malloc(len + 2);
    if (patched) {
        memcpy(patched, doc->source, offset);
        patched[offset] = '_';
        memcpy(patched + offset + 1, doc->source + offset, len - offset + 1);

        frontend_init(repaired);
        diag_set_capture(&repaired->diag, true);
        frontend_run(repaired, doc_label(doc), patched);

        *source = repaired->source ? repaired->source : doc->source;
        *source_len = repaired->source ? strlen(repaired->source) : 0;
        *program = repaired->program ? repaired->program : doc->fe.program;
        *global = repaired->program ? repaired->sema.global : doc->fe.sema.global;
        return repaired->program != NULL;
    }

    *source = doc->source;
    *source_len = doc->source_len;
    *program = doc->fe.program;
    *global = doc->fe.sema.global;
    return false;
}

static void handle_completion(LspServer *s, const char *id, size_t id_len,
                              JValue *params) {
    LspDocument *doc = NULL;
    size_t offset = 0;
    if (!position_params(s, params, &doc, &offset)) {
        send_result(s, id, id_len, "null");
        return;
    }

    Frontend repaired;
    const char *source = NULL;
    size_t source_len = 0;
    ASTNode *program = NULL;
    Scope *global = NULL;
    bool must_free = completion_program(doc, offset, &repaired, &source, &source_len,
                                        &program, &global);
    if (!program) {
        send_result(s, id, id_len, "null");
        if (must_free) frontend_free(&repaired);
        return;
    }

    LspCompletion c = lsp_completion_at(doc->source, doc->source_len, offset);
    Array *items = array_new(sizeof(LspItem));
    bool resolved = true;

    if (c.kind == LSP_COMPLETE_MEMBER) {
        /* The receiver's type: the AST node ending at the dot, or — when the
           text is too broken even for the repaired pass — a bare name
           resolved like any use. */
        Type *type = NULL;
        if (c.receiver_end > 0) {
            LspTarget t = lsp_target_at(program, source, source_len, c.receiver_end - 1);
            type = t.type ? t.type : (t.sym ? t.sym->type : NULL);
        }
        if ((!type || type->kind == TYPE_UNKNOWN) && c.receiver_len) {
            Symbol *sym = resolve_name(global, source, source_len, offset, c.receiver,
                                       program);
            if (sym) type = sym->type;
        }

        if (!type || type->kind == TYPE_UNKNOWN) {
            resolved = false;
        } else if (type->kind == TYPE_ARRAY) {
            item_add(items, "insert", ITEM_METHOD);
            item_add(items, "pop", ITEM_METHOD);
            item_add(items, "push", ITEM_METHOD);
            item_add(items, "remove", ITEM_METHOD);
        } else if (type->members) {
            for (size_t i = 0; i < array_length(type->members->symbols); i++) {
                Symbol *m = *(Symbol **)array_get(type->members->symbols, i);
                if (!m || !m->name) continue;
                bool func = m->type && m->type->kind == TYPE_FUNC;
                if (!m->is_static)
                    item_add(items, m->name, func ? ITEM_METHOD : ITEM_FIELD);
                else if (receiver_static(m, type))
                    item_add(items, m->name, ITEM_FUNCTION);
            }
        }
    } else if (c.kind == LSP_COMPLETE_STATIC) {
        Symbol *sym = c.receiver_len
                          ? resolve_name(global, source, source_len, offset,
                                         c.receiver, program)
                          : NULL;
        if (!sym || sym->kind != SYM_TYPE || !sym->type || !sym->type->members) {
            resolved = false;
        } else {
            for (size_t i = 0; i < array_length(sym->type->members->symbols); i++) {
                Symbol *m = *(Symbol **)array_get(sym->type->members->symbols, i);
                if (!m || !m->name || !m->is_static) continue;
                int kind = m->is_enum_item
                               ? ITEM_ENUM_MEMBER
                               : (m->type && m->type->kind == TYPE_FUNC ? ITEM_FUNCTION
                                                                        : ITEM_CONSTANT);
                item_add(items, m->name, kind);
            }
        }
    } else {
        static const char *const keywords[] = {
            "break", "case", "continue", "default", "else", "enum", "extern",
            "for", "if", "local", "null", "packed", "return", "switch", "type",
            "use", "while"
        };
        for (size_t i = 0; i < sizeof keywords / sizeof keywords[0]; i++)
            item_add(items, keywords[i], ITEM_KEYWORD);

        if (global) {
            for (size_t i = 0; i < array_length(global->symbols); i++) {
                Symbol *sym = *(Symbol **)array_get(global->symbols, i);
                if (!sym || !sym->name) continue;
                if (sym->kind == SYM_TYPE) item_add(items, sym->name, ITEM_CLASS);
                else if (sym->kind == SYM_FUNC) item_add(items, sym->name, ITEM_FUNCTION);
                else if (sym->kind == SYM_VALUE)
                    item_add(items, sym->name, value_item_kind(sym));
            }
        }

        Array *locals = array_new(sizeof(Symbol *));
        if (locals) {
            lsp_visible_bindings(program, source, source_len, offset, locals);
            for (size_t i = 0; i < array_length(locals); i++) {
                Symbol *sym = *(Symbol **)array_get(locals, i);
                if (sym && sym->name)
                    item_add(items, sym->name, value_item_kind(sym));
            }
            array_free(locals);
        }
    }

    if (must_free) frontend_free(&repaired);

    if (!resolved) {
        array_free(items);
        send_result(s, id, id_len, "null");
        return;
    }

    /* Keep the prefix's candidates and sort for a stable list. */
    size_t kept = 0;
    LspItem *data = (LspItem *)array_data(items);
    for (size_t i = 0; i < array_length(items); i++) {
        if (strncmp(data[i].name, c.prefix, c.prefix_len) == 0) data[kept++] = data[i];
    }
    array_set_length(items, kept);
    if (kept > 0) qsort(data, kept, sizeof(LspItem), item_cmp);

    JsonBuf b;
    json_buf_init(&b);
    json_buf_printf(&b, "[");
    for (size_t i = 0; i < kept; i++) {
        json_buf_printf(&b, "%s{\"label\":\"", i ? "," : "");
        json_buf_escape(&b, data[i].name);
        json_buf_printf(&b, "\",\"kind\":%d}", data[i].kind);
    }
    json_buf_printf(&b, "]");
    if (!b.oom) send_result(s, id, id_len, b.data);
    json_buf_free(&b);
    array_free(items);
}

/* ------------------------------------------------------------------ */
/* Document symbols                                                   */
/* ------------------------------------------------------------------ */

/* SymbolKind values — a different enum from the completion item kinds. */
enum {
    SYMBOL_CLASS = 5,
    SYMBOL_METHOD = 6,
    SYMBOL_FIELD = 8,
    SYMBOL_ENUM = 10,
    SYMBOL_FUNCTION = 12,
    SYMBOL_VARIABLE = 13,
    SYMBOL_ENUM_MEMBER = 22,
    SYMBOL_STRUCT = 23
};

/* The token's byte span inside the document; false when it is from elsewhere. */
static bool token_span(LspDocument *doc, Token tok, size_t *start, size_t *end) {
    if (!lsp_token_in_source(doc->source, doc->source_len, tok)) return false;
    *start = (size_t)(tok.start - doc->source);
    *end = *start + (tok.length > 0 ? tok.length : 1);
    if (*end > doc->source_len) *end = doc->source_len;
    return true;
}

static void emit_range(JsonBuf *b, LspServer *s, LspDocument *doc, bool have,
                       size_t start, size_t end) {
    if (!have) {
        start = 0;
        end = 0;
    }
    LspPosition a = lsp_position_of(doc->source, doc->source_len, start, s->encoding);
    LspPosition z = lsp_position_of(doc->source, doc->source_len, end, s->encoding);
    json_buf_printf(b,
                    "{\"start\":{\"line\":%d,\"character\":%d},"
                    "\"end\":{\"line\":%d,\"character\":%d}}",
                    a.line, a.character, z.line, z.character);
}

/* One outline entry. `children_json` is a prebuilt array for hierarchical
   clients; flat clients get the members as siblings with `containerName`. */
static void emit_symbol(JsonBuf *b, LspServer *s, LspDocument *doc, const char *name,
                        int kind, Token tok, const char *container,
                        const char *children_json, bool first) {
    size_t start = 0, end = 0;
    bool have = token_span(doc, tok, &start, &end);

    json_buf_printf(b, "%s{\"name\":\"", first ? "" : ",");
    json_buf_escape(b, name);
    json_buf_printf(b, "\",\"kind\":%d", kind);
    if (s->hierarchical_symbols) {
        json_buf_printf(b, ",\"range\":");
        emit_range(b, s, doc, have, start, end);
        json_buf_printf(b, ",\"selectionRange\":");
        emit_range(b, s, doc, have, start, end);
        if (children_json && children_json[0])
            json_buf_printf(b, ",\"children\":%s", children_json);
    } else {
        json_buf_printf(b, ",\"location\":{\"uri\":\"");
        json_buf_escape(b, doc->uri);
        json_buf_printf(b, "\",\"range\":");
        emit_range(b, s, doc, have, start, end);
        json_buf_printf(b, "}");
        if (container && container[0]) {
            json_buf_printf(b, ",\"containerName\":\"");
            json_buf_escape(b, container);
            json_buf_printf(b, "\"");
        }
    }
    json_buf_printf(b, "}");
}

/* The last name of a path node, or NULL. */
static const char *path_tail(ASTNode *path) {
    if (!path || path->kind != AST_PATH) return NULL;
    Array *parts = path->as.path.parts;
    if (array_length(parts) == 0) return NULL;
    PathSegment *seg = (PathSegment *)array_get(parts, array_length(parts) - 1);
    return seg->name;
}

/* The outline entry a top-level statement makes, or false when it names
   nothing. */
static bool decl_head(ASTNode *stmt, const char **name, int *kind) {
    if (!stmt) return false;
    if (stmt->kind == AST_TYPE_DEF || stmt->kind == AST_PACKED_DEF ||
        stmt->kind == AST_ENUM_DEF) {
        /* A type definition keeps no symbol on the statement (unlike binds),
           so the name comes from the type or from the target path. */
        ASTNode *target = stmt->kind == AST_TYPE_DEF     ? stmt->as.type_def.target
                          : stmt->kind == AST_PACKED_DEF ? stmt->as.packed_def.target
                                                         : stmt->as.enum_def.target;
        const char *n = (stmt->sym && stmt->sym->name) ? stmt->sym->name : NULL;
        if (!n && stmt->type) n = stmt->type->name;
        if (!n) n = path_tail(target);
        if (!n || !n[0]) return false;
        *name = n;
        *kind = stmt->kind == AST_TYPE_DEF     ? SYMBOL_CLASS
                : stmt->kind == AST_PACKED_DEF ? SYMBOL_STRUCT
                                               : SYMBOL_ENUM;
        return true;
    }
    if (stmt->kind == AST_BIND || stmt->kind == AST_ASSIGN) {
        if (!stmt->sym || !stmt->sym->name) return false;
        *name = stmt->sym->name;
        *kind = (stmt->sym->type && stmt->sym->type->kind == TYPE_FUNC)
                    ? SYMBOL_FUNCTION
                    : SYMBOL_VARIABLE;
        return true;
    }
    return false;
}

/* One member of a type body as an outline entry. */
static void emit_member(JsonBuf *b, LspServer *s, LspDocument *doc, ASTNode *field,
                        const char *container, bool *first) {
    if (!field) return;
    const char *name = NULL;
    int kind = SYMBOL_FIELD;
    Token tok = field->token;

    if (field->kind == AST_ASSIGN || field->kind == AST_BIND) {
        if (!field->sym || !field->sym->name) return;
        name = field->sym->name;
        kind = (field->sym->type && field->sym->type->kind == TYPE_FUNC)
                   ? SYMBOL_METHOD
                   : SYMBOL_FIELD;
    } else if (field->kind == AST_MEMBER_ASSIGN) {
        name = field->as.member_assign.member;
        if (field->as.member_assign.member_tok.start)
            tok = field->as.member_assign.member_tok;
        bool func = field->sym && field->sym->type &&
                    field->sym->type->kind == TYPE_FUNC;
        kind = func ? SYMBOL_METHOD : SYMBOL_FIELD;
    } else {
        return;
    }

    emit_symbol(b, s, doc, name, kind, tok, container, NULL, *first);
    *first = false;
}

/* A declaration's members, in source order. */
static void emit_children(JsonBuf *b, LspServer *s, LspDocument *doc, ASTNode *stmt,
                          const char *container, bool *first) {
    if (stmt->kind == AST_ENUM_DEF) {
        Array *items = stmt->as.enum_def.items;
        for (size_t i = 0; items && i < array_length(items); i++) {
            EnumItem *item = (EnumItem *)array_get(items, i);
            if (!item->name) continue;
            emit_symbol(b, s, doc, item->name, SYMBOL_ENUM_MEMBER, item->tok, container,
                        NULL, *first);
            *first = false;
        }
        return;
    }

    ASTNode *body = stmt->kind == AST_TYPE_DEF       ? stmt->as.type_def.body
                    : stmt->kind == AST_PACKED_DEF   ? stmt->as.packed_def.body
                                                     : NULL;
    if (!body || body->kind != AST_TABLE_LITERAL) return;
    Array *fields = body->as.table.fields;
    for (size_t i = 0; fields && i < array_length(fields); i++) {
        emit_member(b, s, doc, *(ASTNode **)array_get(fields, i), container, first);
    }
}

static void handle_document_symbols(LspServer *s, const char *id, size_t id_len,
                                    JValue *params) {
    JValue *td = jmem(params, "textDocument");
    const char *uri = jstr(jmem(td, "uri"));
    LspDocument *doc = uri ? doc_find(s, uri) : NULL;
    if (!doc || !doc->fe.program) {
        send_result(s, id, id_len, "null");
        return;
    }

    JsonBuf out;
    json_buf_init(&out);
    json_buf_printf(&out, "[");
    bool first = true;
    Array *stmts = doc->fe.program->as.program.stmts;
    for (size_t i = 0; stmts && i < array_length(stmts); i++) {
        ASTNode *stmt = *(ASTNode **)array_get(stmts, i);
        const char *name = NULL;
        int kind = 0;
        if (!decl_head(stmt, &name, &kind)) continue;

        if (s->hierarchical_symbols) {
            JsonBuf kids;
            json_buf_init(&kids);
            json_buf_printf(&kids, "[");
            bool kids_first = true;
            emit_children(&kids, s, doc, stmt, NULL, &kids_first);
            json_buf_printf(&kids, "]");
            emit_symbol(&out, s, doc, name, kind, stmt->token, NULL,
                        kids_first ? NULL : kids.data, first);
            json_buf_free(&kids);
        } else {
            emit_symbol(&out, s, doc, name, kind, stmt->token, NULL, NULL, first);
            emit_children(&out, s, doc, stmt, name, &first);
        }
        first = false;
    }
    json_buf_printf(&out, "]");

    if (!out.oom) send_result(s, id, id_len, out.data);
    json_buf_free(&out);
}

/* Handle one parsed message. Returns true when `exit` arrived. */
static bool handle_message(LspServer *s, JValue *msg) {
    JValue *method_value = jmem(msg, "method");
    const char *method = jstr(method_value);
    JValue *id_value = jmem(msg, "id");
    bool is_request = id_value != NULL;

    /* The id to echo; `jid` writes "null" when there is none, which only a
       parse error has. */
    JsonBuf id_buf;
    json_buf_init(&id_buf);
    jid(id_value, &id_buf);
    const char *id = id_buf.data;
    size_t id_len = id_buf.len;

    if (!method) {
        if (is_request) send_error(s, id, id_len, -32600, "a request needs a method");
        json_buf_free(&id_buf);
        return false;
    }

    /* Before `initialize`: requests are answered with ServerNotInitialized,
       notifications are dropped — except `exit`, which must always work. */
    if (!s->initialized &&
        strcmp(method, "initialize") != 0 && strcmp(method, "exit") != 0) {
        if (is_request) send_error(s, id, id_len, -32002, "server not initialized");
        json_buf_free(&id_buf);
        return false;
    }

    /* After `shutdown` the only useful message is `exit`. */
    if (s->shutdown_received && strcmp(method, "exit") != 0) {
        if (is_request) send_error(s, id, id_len, -32600, "server is shutting down");
        json_buf_free(&id_buf);
        return false;
    }

    bool quit = false;
    if (strcmp(method, "initialize") == 0) {
        if (is_request && !s->initialized)
            handle_initialize(s, id, id_len, jmem(msg, "params"));
        else if (is_request)
            send_error(s, id, id_len, -32600, "the server is already initialized");
    } else if (strcmp(method, "initialized") == 0) {
        /* a notification: nothing to do */
    } else if (strcmp(method, "shutdown") == 0) {
        if (is_request) {
            s->shutdown_received = true;
            send_result(s, id, id_len, "null");
        }
    } else if (strcmp(method, "exit") == 0) {
        quit = true;
    } else if (strcmp(method, "textDocument/didOpen") == 0) {
        doc_open(s, jmem(msg, "params"));
    } else if (strcmp(method, "textDocument/didChange") == 0) {
        doc_change(s, jmem(msg, "params"));
    } else if (strcmp(method, "textDocument/didClose") == 0) {
        doc_close(s, jmem(msg, "params"));
    } else if (strcmp(method, "textDocument/hover") == 0) {
        if (is_request) handle_hover(s, id, id_len, jmem(msg, "params"));
    } else if (strcmp(method, "textDocument/definition") == 0) {
        if (is_request) handle_definition(s, id, id_len, jmem(msg, "params"));
    } else if (strcmp(method, "textDocument/completion") == 0) {
        if (is_request) handle_completion(s, id, id_len, jmem(msg, "params"));
    } else if (strcmp(method, "textDocument/documentSymbol") == 0) {
        if (is_request) handle_document_symbols(s, id, id_len, jmem(msg, "params"));
    } else if (is_request) {
        /* `$/`-prefixed requests included, the spec insists. */
        char note[256];
        snprintf(note, sizeof note, "unknown method '%s'", method);
        send_error(s, id, id_len, -32601, note);
    }

    json_buf_free(&id_buf);
    return quit;
}

/* ------------------------------------------------------------------ */
/* The loop                                                           */
/* ------------------------------------------------------------------ */

int lsp_server_run(FILE *in, FILE *out) {
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

        char note[256];
        JValue *msg = jparse(body, len, note, sizeof note);

        if (!msg) {
            send_error(&srv, NULL, 0, -32700, note);
        } else if (!json_value_as_object(msg)) {
            send_error(&srv, NULL, 0, -32600, "a JSON-RPC message must be an object");
        } else {
            quit = handle_message(&srv, msg);
        }

        free(msg);   /* the whole parsed message, one allocation */
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
