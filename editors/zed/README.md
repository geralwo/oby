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

So we borrow someone else's no-op extension to register the name. The actual
command still comes from our settings; the stub does nothing. The *rest* of the
extension is ours: `extension.toml` also points Zed at the tree-sitter grammar
in `editors/tree-sitter-oby/`, which is what gives `.oby` its own language with
highlighting, bracket matching and an outline.

[issue]: https://github.com/zed-industries/zed/issues/52653

## Step 1 — build the server

```sh
bash compile.sh          # -> build/oby
```

## Step 2 — copy the addon into Zed

`editors/zed/lspaddon/` is ready to copy as it stands:

| file | what it is |
| ---- | ---------- |
| `extension.toml` | the manifest: the grammar, and the server name |
| `languages/oby/config.toml` | `Oby` = `*.oby`, with grammar `oby` |
| `extension.wasm` | the stub that registers the server name (sha256 `ec9df651072fcf8e57e9ddeeaac1d4eef2f31a8ac78339493798f5378bfa3914`) |
| `grammars/oby.wasm` | the compiled grammar, built from this repository |
| `languages/oby/*.scm` | the queries Zed colours with — `highlights`, `brackets`, `indents`, `outline` |

Zed reads a language's queries from the language folder, not from the grammar,
so those four files live here as well as in `editors/tree-sitter-oby/queries/`.
The copies next to `config.toml` are the ones that apply; the grammar's are for
the tree-sitter CLI and for editors that fetch a grammar whole.

Both blobs are committed and nothing is built or fetched at install time. That
is deliberate: a folder dropped into `extensions/installed/` is treated as
**prebuilt**, so Zed will not compile a grammar for it — it looks for
`grammars/<name>.wasm` and, when it is missing, fails to load the language
altogether (the server goes quiet too, not just the colours).

Rebuilding the grammar after changing it is a two-command job, in
`editors/tree-sitter-oby/README.md`.

Zed only looks in its own extensions folder:

| OS | folder |
| -- | ------ |
| Windows | `%LOCALAPPDATA%\Zed\extensions\installed\` |
| Linux | `~/.local/share/zed/extensions/installed/` |
| macOS | `~/Library/Application Support/Zed/extensions/installed/` |

On Windows, `install.ps1` next to this file does the copy. It replaces a
previous install rather than nesting inside it, which is what `Copy-Item
-Recurse` does once the folder exists:

```powershell
.\editors\zed\install.ps1
# or, when the execution policy refuses the script:
powershell -ExecutionPolicy Bypass -File editors\zed\install.ps1
```

Elsewhere it is a plain copy:

```sh
cp -r editors/zed/lspaddon "$LOCALAPPDATA/Zed/extensions/installed/"
```

Afterwards you should have:

```
%LOCALAPPDATA%\Zed\extensions\installed\lspaddon\
├── extension.toml
├── extension.wasm
├── grammars\oby.wasm
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
`oby --lsp`, and the colours come from the grammar.

Zed fetches the grammar from `https://github.com/geralwo/oby` at the revision
`extension.toml` names, and builds the copy in `editors/tree-sitter-oby/` that
that revision holds. After changing the grammar, commit it and bump `rev`, or
Zed keeps building the old one. (The repository has to be public for Zed to
clone it, or the clone needs credentials.)

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
- **`failed to load language Oby: The system cannot find the path specified`**
  in `zed: open log` — `grammars\oby.wasm` is missing from the installed addon.
  Zed does not compile a grammar for an extension it treats as prebuilt, and it
  drops the whole language when the blob is not there, which takes the server
  down with it. The copy in this repository has the blob; rebuilding it after a
  grammar change is the recipe in `editors/tree-sitter-oby/README.md`.

## The other routes

**Our own stub extension.** Same shape, but ours: `extension.toml` +
`Cargo.toml` + `src/lib.rs`, where `lib.rs` hands back the command directly.
Zed builds dev extensions itself (`Install Dev Extension`), which needs a Rust
toolchain with the `wasm32-wasip1` target — nothing on this machine has Rust
today, so it isn't written. Worth it only if we stop reusing the stub, and it
is now the *only* thing left that the stub provides (the grammar is ours).

**A grammar repository of its own.** The grammar sits in this repository and can
be reached with `path`, which is what a remote for `oby` alone buys. Worth
knowing, though: for an extension placed in `extensions/installed/` by hand, Zed
never fetches it — `repository`, `path` and `rev` are inert there, and the
committed `grammars/oby.wasm` is what actually loads. They matter once the
extension is published and Zed downloads it as a built artifact.

A `tree-sitter-oby` repository of its own is what would let Neovim and Helix use
the grammar, since they fetch one by repository without a `path` — and it would
keep the committed `src/parser.c` out of this repository's history.
