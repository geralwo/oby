#ifndef OBY_LSP_SERVER_H
#define OBY_LSP_SERVER_H

#include <stdio.h>

/* Run the language server over `in`/`out` until end of input or the `exit`
   notification. Returns the process status: 0 after a `shutdown`, 1 otherwise
   (including a client that drops the connection first). */
int lsp_server_run(FILE *in, FILE *out);

#endif // OBY_LSP_SERVER_H
