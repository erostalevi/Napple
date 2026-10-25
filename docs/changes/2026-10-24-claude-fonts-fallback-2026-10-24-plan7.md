# pl:u placeholder font when system/fonts is missing (2026-10-24)

Running with a save root that has no `system/fonts` (for example `--save-root <empty dir>`) used to hand the guest zero-size fonts, and
LAS / SMO died with `CRASH signal=11 fault=0x38` right after the pl:u opens. `pl.c` now serves a built-in placeholder TrueType font
(one empty glyph, no text rendered) for all six shared-font types whenever no real font could be loaded, and prints one actionable line.

What mattered (found by experiment, headless LAS 10 s runs):
- zero-size fonts crash; one shared offset for all six types crashes (each type gets its own copy);
- the guest's font engine rejects a bare-minimum sfnt (head/hhea/maxp/hmtx/cmap/loca/glyf) and also old Mac fonts (Monaco, Geneva) but
  accepts Arial / Andale Mono, so the placeholder carries OS/2, name and post as well.
- With real fonts (any file loaded) behaviour is unchanged.

Files: `runtime/src/services/pl.c`, new `runtime/tests/test_svc_pl.c`.
Verified: `make test test-services` pass (test_svc_pl 44 checks); headless 45 s with an empty save root on LAS and SMO, no CRASH, rc=0.
