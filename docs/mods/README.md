# Mods and fixes (`.napmod`)

A `.napmod` is a small zip that anyone can create and share: a game fix, a widescreen patch, a cheat list, replacement files, per-title settings. Napple reads it, shows exactly what it contains, and applies it only when you say so.

| Page | For | What it covers |
|---|---|---|
| [Format](format.md) | everyone | The layout of a `.napmod`, `mod.json`, every component, the cheat opcode table, limits and the trust model |
| [Authoring](authoring.md) | mod makers | Write a pchtxt or IPS fix, find a build id, test it, pack and publish |
| [Integration](integration.md) | contributors | The library API, the `napple-build` hook with its exact call site, and the Mac follow-up list |
| [Example mod](examples/synthetic-fix/mod.json) | everyone | A made-up fix for a made-up title with every component (`examples/synthetic-fix/`, plus `examples/synthetic-module.bin` to try it on) |

Quick look, with the example mod:

```
napple mods validate docs/mods/examples/synthetic-fix
napple mods pack     docs/mods/examples/synthetic-fix -o synthetic-fix.napmod
napple mods inspect  synthetic-fix.napmod
napple mods apply    synthetic-fix.napmod --image docs/mods/examples/synthetic-module.bin \
    --build-id 0123456789ABCDEF0123456789ABCDEF --layout 0x1000,0x1800 --enable-code --dry-run
```

Three rules sit under everything: mods are **unsigned by default** (trust the author, not the file), **code patches stay off until you enable them**, and Napple **never downloads a mod by itself**. A mod must not contain game data or keys; `napple mods validate` warns about extracted assets and refuses key files.

Status: the portable core (parsers, validation, packing, patch application, CLI) is implemented and unit tested on Linux. The build integration, the runtime romfs lookup, the in-app UI and the shader hook are Mac follow-ups listed in [Integration](integration.md).
