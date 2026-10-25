# 2026-10-24 NVN getters/setters: scanner-visible definitions, transform-feedback and GL-sync entries

## What changed
- The `percent-analysis` scan listed 101 of 523 NVN entry points as missing. 72 were already implemented but declared with `NVN_NOPROTO(` (invisible to the scanner's `^NVN\(` match), and 20 sampler getters came from a macro (`SAMPLER_GETTERS`). They are now plain `NVN(name, ...)` definitions, so the scan sees them.
- `nvnapi/nvn_protos_ext.h` (new, included at the end of `nvn_protos.h`) carries prototypes for the entries the bundled NVN header lacks. 28 prototypes in `nvn_protos.h` that disagreed with the implementations (the `NVN_NOPROTO` ones had hidden the mismatch, e.g. `MemoryPoolBuilderGetMemory` returned `void`) were replaced by the implemented signature, marked "corrected".
- New definitions: `nvnCommandBuffer{Bind,Bind...Buffers,Begin,End,Pause,Resume,Draw}TransformFeedback` and `nvnSyncInitializeFromFencedGLSync` / `nvnSyncCreateGLSync`. These are accepted and dropped (warn once); they have no Metal side. **Still missing functionally.**
- `internal.h`: `NVN_IMPL_SECTION` is overridable (`#ifndef`) so an ELF host can build the API files.
- multi-draw-indirect-count (`nvnCommandBufferMultiDraw{Arrays,Elements}IndirectCount`) already existed in `api_cmd.c` (`OP_DRAW_IND_COUNT`); no change needed.

## Verified vs not
- tested (Linux): `sh Napple/gfx/tests/t_getters_linux.sh` -> 33 checks, 0 failed (blend/color/channel-mask/multisample/sampler-builder/sampler round trips).
- scan.py NVN domain: 422/523 -> 523/523 (missing 101 -> 0 by its definition; 9 of those are no-op entries above).
- unbuilt on macOS: everything (`make -C gfx` not run); `-fsyntax-only` with gcc passes for all `api_*.c` (api_memory.c only with stub Mach headers outside the tree, which cannot prove its Mach calls).
