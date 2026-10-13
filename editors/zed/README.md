# Zed

Zed requires an **extension** to register a language server name. Its settings
schema rejects `lsp.<name>` for a name that no extension (or built-in adapter)
has registered — the editor reports `property <name> is not allowed` — and the
request for Neovim-style, settings-only servers was
[closed as not planned][issue]: *"Zed currently only allows LSP names that an
extension or built-in adapter has registered."* The launch *command* is still
ordinary settings (`lsp.<name>.binary`), so the extension is a registration
stub with no logic in it.

[issue]: https://github.com/zed-industries/zed/issues/52653

## Setup (route 1: the stub wasm — no Rust)

Everything is prepared in `editors/zed/lspaddon/`:

```
lspaddon/
├── extension.toml              # declares `oby` for the language `Oby`
├── extension.wasm              # the stub (fetched, not committed)
└── languages/oby/config.toml   # `Oby` = *.oby, no tree-sitter grammar
```

1. Fetch the stub next to its manifest (unmodified from
   <https://github.com/zhcn000000/zed-customlsp>, sha256
   `ec9df651072fcf8e57e9ddeeaac1d4eef2f31a8ac78339493798f5378bfa3914`):

   ```sh
   curl -fL -o editors/zed/lspaddon/extension.wasm \
     https://raw.githubusercontent.com/zhcn000000/zed-customlsp/master/extension.wasm
   ```

2. Copy the folder into Zed's installed extensions — the directory the other
   extensions live in (`%LOCALAPPDATA%\Zed\extensions\installed\` on Windows,
   `~/.local/share/zed/extensions/installed/` on Linux,
   `~/Library/Application Support/Zed/extensions/installed/` on macOS):

   ```sh
   cp -r editors/zed/lspaddon "$LOCALAPPDATA/Zed/extensions/installed/"
   ```

3. Add the server command to `settings.json`:

   ```json
   "lsp": {
     "oby": {
       "binary": {
         "path": "wsl.exe",
         "arguments": ["-e", "/mnt/c/Users/user/oby/build/oby", "--lsp"]
       }
     }
   }
   ```

   (With a native build it is just `"path": "oby"`, `"arguments": ["--lsp"]`.)
   `wsl.exe -e` runs the Linux binary directly, no shell and no path mangling.

4. Reload extensions (`zed: extensions`, or restart Zed) and open an `.oby`
   file: diagnostics, hover, completion, and the outline come from
   `oby --lsp`.

If the extension does not appear under `zed: extensions`, Zed also keeps a
registry of installed extensions in `extensions/index.json`; that entry is not
written yet — say so and we add it.

If Zed rejects the grammar-less `Oby` language (Zed's docs say a language in an
extension requires a tree-sitter grammar), switch `extension.toml` to the
carrier-language variant commented at its bottom — attach `oby` to a language
you never otherwise open — and add:

```json
"file_types": { "Starlark": ["oby"] },
"languages": { "Starlark": { "language_servers": ["oby"] } }
```

The caveat: the server then starts for every file of that language too, and
`.oby` files are highlighted as it. A stopgap, not the destination.

## Route 2: our own stub extension (needs Rust once)

Same shape, but ours: `extension.toml` + `Cargo.toml` + `src/lib.rs`, where
`lib.rs` returns the command directly

```rust
fn language_server_command(...) -> Result<zed::Command, String> {
    Ok(zed::Command {
        command: "wsl.exe".into(),
        args: vec!["-e".into(), "/mnt/c/…/oby".into(), "--lsp".into()],
        env: vec![],
    })
}
```

and the manifest declares the server (and a language, with the same grammar
question). Zed builds dev extensions itself (`Install Dev Extension`), which
needs a Rust toolchain with the `wasm32-wasip1` target — nothing on this
machine has Rust today. Not written; worth it only if we stop reusing the
stub.

## Route 3: the real thing — a grammar and a language extension

What a language repository eventually ships: a tree-sitter grammar for OBY
(`grammar.js` plus `highlights.scm`, `brackets.scm`, `outline.scm`,
`indents.scm`), an `extension.toml` pointing at it with `repository` + `rev`,
and the server entry. Zed fetches and compiles the grammar itself, so *users*
need no toolchain — but the grammar is a real project, and it is the only
route that gives `.oby` its own language with highlighting (it replaces the
Neovim syntax file's role in Zed). This is the "editor packages" item in the
deferred list of the LSP plan.
