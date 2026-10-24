# Third-party code in gfx/

| Path | Origin | License |
|---|---|---|
| `src/nvnapi/nvn.h` | h1k421 (NVN types reconstructed from public DWARF) | MIT (header inside) |
| `src/shader/maxwell_translate.c` | LASRecomp runtime/nvn/maxwell (Peiikai) | MIT (`third_party/LICENSE.LASRecomp`) |
| `src/tex/napple_layout.c, napple_deswizzle.c, napple_fmt.c, napple_bcdec.c` | D2R-Mobile gfx via MacNativeGames (jhaizhou); layout derived from deko3d (fincs) | MIT (`third_party/LICENSE.d2r_gfx`), zlib (`third_party/LICENSE.deko3d`) |
| `third_party/bcdec` | iOrange bcdec | MIT / public domain |
All reused code was renamed, stripped of per-game logic and re-tested here.
