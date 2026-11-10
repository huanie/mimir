# MimIR Language Server {#lsp}

[TOC]

Use `mim-lsp` to see Mim compiler errors and warnings in an editor that supports language servers. The server only reports diagnostics.

## Build

From the MimIR source tree, run:

```sh
cmake --build build --target mim-lsp
```

The executable is `build/bin/mim-lsp`. If CMake did not create the target, configure with `-DMIM_BUILD_LSP=ON` and build again.

## Connect your editor

Configure your editor's language-server client to start `mim-lsp` and use standard input/output (stdio) for communication. Attach it to `.mim` files. The exact settings depend on your editor.

The server needs an entry-point file to compile and, ideally, a `mim.toml` manifest for the search paths and plugins that the entry point needs. If you open the project folder as the editor workspace, the server picks up its `mim.toml` automatically and uses the first `.mim` file you open as the entry point. If your editor cannot pass a workspace folder, or you want to pin the entry point or manifest yourself, pass them on the command line:

```sh
# pin the entry-point file
build/bin/mim-lsp path/to/main.mim

# pin the manifest; the first .mim file you open becomes the entry point
build/bin/mim-lsp --config path/to/mim.toml

# pin both
build/bin/mim-lsp --config path/to/mim.toml path/to/main.mim
```

To see the available command-line options, run `build/bin/mim-lsp --help`.

## Work with diagnostics

The compiler reads files from disk, so save your changes to compile them. Saving any file recompiles the entry point and updates diagnostics: a `.mim` source file, including an imported one, or `mim.toml`, which the server detects through the editor's file-change notifications. While you edit, the server clears the previous diagnostics; they return after the next compilation.

If you start `mim-lsp` without a file argument, the first `.mim` file you open becomes the project's entry point. Pass a file on the command line when you want to choose the entry point yourself.

## Optimization

The server skips optimization and backend runs by default. Add `--optimize` to enable them. A backend can write files or start external tools, so use this option only when your project needs those steps.
