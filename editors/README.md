# Editor support

`oby --lsp` is a language server: one binary, LSP over stdio, no runtime, no
daemon, no network. Any LSP-capable editor can use it — build it with
`bash compile.sh`, and the server is `build/oby --lsp`. The client's language
id is `oby` for `*.oby` files. (`editors/nvim/` holds Neovim syntax files and
an install script for them.)

| Feature | Method | Notes |
|---|---|---|
| Live diagnostics | `textDocument/publishDiagnostics` | On open, on every change; cleared on close |
| Hover | `textDocument/hover` | The resolved type, as a fenced markdown block |
| Go to definition | `textDocument/definition` | Declarations in the same file |
| Completion | `textDocument/completion` | Members after `.`, statics and enum constants after `::`, names elsewhere |
| Document symbols | `textDocument/documentSymbol` | Outline: types, functions, globals; members and enum constants as children |

What the client needs to know:

* **Full sync**: `textDocumentSync: 1` — send the whole buffer on each change;
  there is nothing incremental to configure.
* **Position encoding** is negotiated (UTF-16 unless the client offers
  `utf-8`); nothing to set on either side.
* **Diagnostics belong to the open document.** Errors inside a `use`d file (or
  the prelude) are not published against another URI yet — open that file to
  see them. A fix clears the squigglies on the next change, and closing the
  buffer clears them for good.
* **Definition stays inside the file**: it never jumps to another file or the
  prelude.
* **Completion works while typing**: `.` and `::` complete members and statics,
  and the text on the line does not have to parse.

Not there yet: formatting, rename, find references, cross-file definitions,
workspace symbols.

## Check it by hand

```sh
send() { printf 'Content-Length: %d\r\n\r\n%s' "${#1}" "$1"; }
{
  send '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"capabilities":{}}}'
  send '{"jsonrpc":"2.0","id":2,"method":"shutdown"}'
  send '{"jsonrpc":"2.0","method":"exit"}'
} | ./build/oby --lsp
```

Two framed responses (`initialize`, `shutdown`) and an exit status of 0 mean
the binary is ready; point an editor at the same command.

## Neovim (built-in LSP)

```lua
vim.filetype.add({ extension = { oby = "oby" } })

vim.api.nvim_create_autocmd("FileType", {
  pattern = "oby",
  callback = function()
    vim.lsp.start({
      name = "oby",
      cmd = { "oby", "--lsp" }, -- or an absolute path to the binary
      root_dir = vim.fs.dirname(vim.api.nvim_buf_get_name(0)),
    })
  end,
})
```

Plus the syntax file: `bash editors/nvim/install_oby_syntax.sh` (or copy
`editors/nvim/syntax/oby.vim` and `editors/nvim/ftdetect/oby.vim` into your
config by hand).

## Helix

In `~/.config/helix/languages.toml`:

```toml
[language-server.oby]
command = "oby"
args = ["--lsp"]

[[language]]
name = "oby"
file-types = ["oby"]
comment-token = "//"
indent = { tab-width = 4, unit = "\t" }
language-servers = ["oby"]
```

Highlighting needs a tree-sitter grammar, which does not exist yet; the
language server works either way.

## Emacs (eglot)

```elisp
(with-eval-after-load 'eglot
  (add-to-list 'eglot-server-programs '(prog-mode . ("oby" "--lsp"))))
(add-to-list 'auto-mode-alist '("\\.oby\\'" . prog-mode))
```

Then `M-x eglot` in an `oby` buffer.

## Others

VS Code has no built-in way to attach a server to a language it does not know,
so it needs a small client extension (`vscode-languageclient` with
`documentSelector: [{ language: "oby" }]` and the server command
`{ command: "oby", args: ["--lsp"] }`). Zed is the same story, with one
wrinkle that works in our favor: the server *command* lives in settings
(`lsp.oby.binary`), so only the name registration needs an extension.
`editors/zed/README.md` lays out the routes, including a no-Rust one that
reuses a stub wasm. Neovim, Helix, and Emacs work with no extra artifact.

## The server in WSL

`oby` builds and runs in Linux. When the editor is a Windows program and the
server is the WSL binary, launch it through `wsl.exe`:

```
wsl.exe -e /mnt/c/Users/user/oby/build/oby --lsp
```

(`wsl.exe -e` runs the Linux binary directly, no shell involved.) For Neovim
on Windows the same command goes in `cmd`:

```lua
cmd = { "wsl.exe", "-e", "/mnt/c/Users/user/oby/build/oby", "--lsp" },
```

Paths: a Windows client sends Windows paths (`file:///c%3A/...`), which decode
to `/c:/...` on the WSL side. The server prefers the `/mnt/c/...` spelling when
that is where the document's directory actually is, so `use` imports resolve
normally.
