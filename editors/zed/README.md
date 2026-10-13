# Zed

Getting `oby --lsp` running in Zed comes down to three things you touch:

| # | What | Where |
| - | ---- | ----- |
| 1 | the stub wasm (not committed) | `editors/zed/lspaddon/extension.wasm` |
| 2 | a copy of the addon | `%LOCALAPPDATA%\Zed\extensions\installed\lspaddon\` |
| 3 | the launch command | `%APPDATA%\Zed\settings.json` |

Plus a build of `oby` itself, which you already have if you've run
`bash compile.sh`.

## Why there's a stub at all

Zed refuses to start a language server whose name no extension has registered.
Put `"lsp": { "oby": ... }` in settings without one and it complains
`property oby is not allowed`. The request to allow settings-only servers was
[closed as not planned][issue] — *"Zed currently only allows LSP names that an
extension or built-in adapter has registered."*

So we borrow someone else's no-op extension just to get the name registered. The
actual command still comes from our settings; the extension does nothing.

[issue]: https://github.com/zed-industries/zed/issues/52653

## Step 1 — build the server

```sh
bash compile.sh          # -> build/oby
```

## Step 2 — fetch the stub wasm

`editors/zed/lspaddon/extension.wasm` isn't committed. Grab the unmodified stub
from <https://github.com/zhcn000000/zed-customlsp> (sha256
`ec9df651072fcf8e57e9ddeeaac1d4eef2f31a8ac78339493798f5378bfa3914`):

```sh
curl -fL -o editors/zed/lspaddon/extension.wasm \
  https://raw.githubusercontent.com/zhcn000000/zed-customlsp/master/extension.wasm
```

## Step 3 — copy the addon into Zed

Zed only looks in its own extensions folder:

| OS | folder |
| -- | ------ |
| Windows | `%LOCALAPPDATA%\Zed\extensions\installed\` |
| Linux | `~/.local/share/zed/extensions/installed/` |
| macOS | `~/Library/Application Support/Zed/extensions/installed/` |

```sh
cp -r editors/zed/lspaddon "$LOCALAPPDATA/Zed/extensions/installed/"
```

Afterwards you should have:

```
%LOCALAPPDATA%\Zed\extensions\installed\lspaddon\
├── extension.toml
├── extension.wasm
└── languages\oby\config.toml
```

## Step 4 — edit Zed's settings

Open `%APPDATA%\Zed\settings.json` (`~/.config/zed/settings.json` on
Linux/macOS) and add a top-level `lsp` entry:

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

That's the whole Windows+WSL dance: `wsl.exe -e` runs the Linux binary straight
up — no shell, no path mangling. On a native build it's just
`"path": "oby", "arguments": ["--lsp"]`.

## Step 5 — reload and open a file

Run `zed: extensions` from the command palette (or just restart Zed), then open
any `.oby` file. Diagnostics, hover, completion, and the outline come from
`oby --lsp`.

## Optional — jump into the prelude

The prelude normally comes from the copy baked into the binary, so `print`,
`List`, and friends have no file to jump to. Point the server at the sources:

```json
"lsp": {
  "oby": {
    "binary": {
      "path": "wsl.exe",
      "arguments": [
        "-e", "env",
        "OBY_PRELUDE_SOURCE=/mnt/c/Users/user/oby/src/prelude",
        "/mnt/c/Users/user/oby/build/oby", "--lsp"
      ]
    }
  }
}
```

The `env` has to go through `wsl.exe -e env VAR=value …`; a Windows-side `env`
entry in the settings doesn't cross the WSL boundary. On a native build it's
just `"env": { "OBY_PRELUDE_SOURCE": "/path/to/oby/src/prelude" }`.

A wrong path is harmless — the override is ignored unless the directory actually
holds `core.oby`, and you fall back to the embedded copy.

## If something's off

- **`property oby is not allowed`** — the addon isn't where Zed looks, or Zed
  hasn't reloaded. Check `...\extensions\installed\lspaddon\` and run
  `zed: extensions`.
- **The addon doesn't show up in `zed: extensions`** — Zed also keeps a registry
  in `extensions/index.json`. We haven't needed to touch it, but that's the next
  place to look.
- **Zed refuses the grammar-less `Oby` language** — Zed's docs say a language in
  an extension needs a tree-sitter grammar. Fall back to a carrier language:
  uncomment the variant at the bottom of `lspaddon/extension.toml` (attach `oby`
  to a language you never open, e.g. Starlark) and add to settings:

  ```json
  "file_types": { "Starlark": ["oby"] },
  "languages": { "Starlark": { "language_servers": ["oby"] } }
  ```

  Downside: the server also starts for every file of that language, and `.oby`
  files get its highlighting. A stopgap, not the destination.

## The other routes

**Our own stub extension.** Same shape, but ours: `extension.toml` +
`Cargo.toml` + `src/lib.rs`, where `lib.rs` hands back the command directly.
Zed builds dev extensions itself (`Install Dev Extension`), which needs a Rust
toolchain with the `wasm32-wasip1` target — nothing on this machine has Rust
today, so it isn't written. Worth it only if we stop reusing the stub.

**The real thing.** A tree-sitter grammar for OBY (`grammar.js` plus
`highlights.scm`, `brackets.scm`, `outline.scm`, `indents.scm`) and an
`extension.toml` pointing at it with `repository` + `rev`. Zed fetches and
compiles the grammar itself, so users need no toolchain, and `.oby` finally gets
its own language with highlighting — it replaces the Neovim syntax file's role.
That's the "editor packages" item in the LSP plan's deferred list.
