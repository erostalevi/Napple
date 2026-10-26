# Shader ISA gaps (branch shader-isa)

Status: translator and tests written on Linux, then (round 2, same day, on an Apple M5 Mac) every emitted form compiled by Metal and both shader corpora re-run: LAS 679/679 and SMO 128/128 translate and compile. **None of the new ops occurs in either corpus, so the "layout from memory" encodings are still unverified against real shaders.**

## Verification levels used below
- **T** = regression test in `gfx/tests/t_xlate.m`, passing on Linux: the translator accepts the hand-assembled instruction and emits the asserted MSL (or refuses with the asserted message).
- **M** = the emitted MSL passes `newLibraryWithSource` on a Mac (`make -C gfx test`, Apple M5). Done for every op below except CS2R (which emits only `rN = 0u`, compiled as part of its test anyway). The Linux build still skips these checks.
- **R** = checked against real shader binaries. None of the new ops is: see "Corpus run" below. A word-level scan of both corpora found no instance of them.
- The test assembler encodes each instruction the way the translator decodes it, so a test only proves the two agree. For the ops marked **layout from memory** the bit layout comes from recollection of the public Maxwell tables, not from a disassembler or a captured shader. A wrong field would pass the tests and miscompile. Those ops need R before they are trusted.

## Ops added
| Op | Behaviour | Level |
|----|-----------|-------|
| LDS / STS (U8/S8/U16/S16/32/64/128) | `threadgroup uint smem_w[]` plus a byte pointer, bounds checked (out of range reads 0, writes dropped). Size: `ShaderKey.shared_size`, else the highest static offset, else 32 KB if addressed through a register. Over 32 KB is refused. Compute only. Reported as `ShaderInfo.shared_mem`. Works with the existing `BAR` (`threadgroup_barrier(mem_threadgroup)`); order store, barrier, load is asserted. | T, M. Encodings follow the existing LDL/STL layout (opcodes ef48/ef58). |
| TLD (1D, 1D array, 2D, 2D array, 3D; lod flag; 4-bit packed offsets) | `texture.read(coord, level)`, resolution-scale aware like TLDS. Refused: multisample, cube, bindless TLD.B (falls to "unknown instruction"). | T, M (1D, 1D array, 2D, 2D array, 3D, depth slot). **Layout from memory.** |
| TXD (2D, 2D array, 3D, cube) | `sample(..., gradient2d/3d/cube(dx, dy))`; Rb = dx components then dy components. Refused: 1D, offsets, TXD.B. | T, M (2D, 2D array, 3D, cube). **Layout from memory.** |
| ATOMS (shared), ATOM and RED (global) 32-bit add/min/max/inc/dec/and/or/xor/exchange | `atom_tg` / `atom_dev` helpers (native for add/and/or/xor/exchange, CAS loop for the rest). Global form honours both GPU-address and window gmem modes; sets `uses_gmem_store` (STG now sets it too, it was never set). Refused: CAS, 64-bit, float/f16 forms. | T, M (all nine ops for ATOMS and ATOM, RED, GPU-address and window gmem). **Layout from memory.** |
| VOTE (all / any / eq + ballot) | `simd_ballot`, `simd_all`, `simd_any`. Vertex functions have no SIMD-group ops: one lane votes alone. | T, M. **Layout from memory.** |
| CS2R | Writes 0 to the register pair (no clock source is exposed). | T, M. **Layout from memory.** |
| BFE.BREV | `bfe_*(reverse_bits(a), ctl)` (source reversed before extraction). | T, M. Reverse-then-extract order from memory. |
| PRMT modes F4E, B4E, RC8, ECL, ECR, RC16 | `prmt_m` helper. Also fixes the default mode, which ignored selector bit 3 (replicate the byte's sign). That changes output for existing PRMTs that used bit 3. | T, M. The mode tables were checked on Linux by running the generated helper as C against the PTX tables (0 mismatches); not a permanent test. |
| R2P.CC | Z, S, C flags set from the selected bits of Ra (overflow not tracked, as for P2R.CC). | T, M |
| Indexed attribute load `ALD a[Ra+imm]` | Fragment: the existing `attr_arr` path. Vertex: `vattr_arr` table built from the declared inputs. Limitation: attributes the program never names statically are not declared, so an index that selects one reads 0. Destination aliasing the index register is handled. | T, M |
| TLD4 / TLD4S with AOFFI | Metal needs constant gather offsets, so the offset is folded into the coordinate (`coord + offset / size`; a whole-texel shift keeps the 2x2 footprint, and the sampler's wrap/clamp still applies). Fragment divides by the resolution scale. 2D and 2D array only. | T, M. Unusual operand order (offset register before the depth reference; TLD4S with offsets reads x = Ra, y = Ra+1, offsets = Rb) is from memory. |

## Refused cleanly, with a named message
- DADD, DMUL, DFMA, DSETP, DMNMX: "double precision is not supported (Metal has no f64)". Emulating f64 on f32 hardware was not attempted. (T)
- OUT: geometry output, Metal has no geometry stage. (T)
- SULD / SUST / SUATOM / SURED family (top bytes 0xea, 0xeb except RED): needs an image binding in the translator contract (`ShaderTexture.is_image` exists but nothing binds it). (T)
- TLD4 PTP (per-texel offsets), TLD multisample / cube, ATOM/ATOMS CAS and 64-bit, TXD 1D. (T)

## Not done, and why
- **IMAD**: not implemented. Correction to round 1: NVIDIA's Maxwell/Pascal instruction list (CUDA binary utilities docs, from memory) does include IMAD, IMADSP and IMUL; the compiler just emits XMAD sequences instead. The corpus scan found 0 words in the IMAD (0x5a0/0x4a0/0x340/0x520 prefixes), IMAD32I (0x8) or IMUL (0x5c38/0x4c38/0x3838) encodings, so there is nothing to implement against. If one ever shows up it fails as "unknown instruction".
- **LEA**: not implemented. The corpus scan found 0 LEA words (0x5bd/0x4bd/0x36d prefixes) and 0 LEA.HI (0x18), so no real word pins the lo/hi layout down; it stays a clean "unknown instruction" failure.
- **SUST / SULD / SUATOM**: refused, see above.
- Items 3 left as fail(): PTP gather offsets, TLD4S with both depth reference and offsets, `ld a[]` in stages other than vertex and fragment.
- Real-shader check of the new decoders: not possible with the corpora at hand (none of the ops occurs). Needs a title that uses compute shared memory/atomics, TLD, TXD or VOTE.

## Other changes
- `ShaderKey.shared_size` (appended) and `ShaderInfo.shared_mem` added (`maxwell.h`). The replay layer does not set `shared_size`; compute shaders get the inferred or 32 KB default. Statically declared `threadgroup` arrays need no encoder call.
- `TRANSLATOR_VERSION` 4 to 5 in `gfx/src/mtl_shader.m` (one line, outside `gfx/src/shader/`; requested explicitly, called out here because that file is not part of the translator's directory).
- `t_xlate.m` now builds without Metal: `MSL_COMPILES` is a no-op that counts a skipped check, and `tests/t_xlate_linux.sh` builds and runs it with gcc (`sh gfx/tests/t_xlate_linux.sh`). On a Mac nothing changes: `make -C gfx test` runs every check including the Metal compile.

## Round 2 (Mac): Metal compile and corpus run
- `make -C gfx test`: all tests ok. Of the original 32 Metal checks, one failed: **TLD 1D array** (`texture1d_array::read` only takes a constant 0 lod; Metal 1D textures have one level). Fixed: 1D and 1D-array TLD now consume the lod register but pass no lod. `TRANSLATOR_VERSION` 5 to 6.
- The suspects named for this round all compiled as emitted: `(threadgroup atomic_uint *)` / `(device atomic_uint *)` casts, the threadgroup `uchar *` alias of `smem_w`, `static_cast<ulong>(simd_ballot())`, `gradient3d`, depth2d read. Tests added so each also has a Metal check: ATOMS and ATOM with every op (native and CAS-loop helpers), RED, ATOM/RED in window gmem mode, TXD 2D array, TXD cube (`gradientcube`), TLD 1D, TLD 2D array + offsets, TLD and TLD.LZ of a depth slot (`depth2d<float>`). 41 Metal checks now, all passing.

### Corpus run (`NAPPLE_SHADER_CORPUS`, Apple M5)
| Corpus | Shaders | Translate | Metal compile | Baseline 2026-10-22 |
|--------|---------|-----------|---------------|---------------------|
| LAS / Link's Awakening (`work/shaders/t_las`; 7 compute) | 679 | 679 | 679 | 671/679 translate |
| SMO sample (`work/shaders/t_smo`) | 128 | 128 | 128 | 128/128 |

No TRANSLATE FAIL and no METAL COMPILE FAIL lines in either run. The 8 LAS failures in the baseline (and the 6 still listed on 2026-10-23) are gone; the HSETP2 immediate / AL2P indexed / TEX.B fixes landed before this branch. LAS and Link's Awakening are the same corpus; there is no third one on this machine.

### Word scan of the corpora (control words skipped, code from 0x80 for graphics, bounded by the .ctl length)
- TLD (0xdc/0xdd), TXD/TXD.B (0xde0-0xde7), ATOMS (0xec), ATOM (0xed), RED (0xebf8), SULD/SUST/SUATOM (0xea/0xeb), VOTE (0x50d8), CS2R (0x50c8), LDS (0xef48), STS (0xef58), TLD4.B (0xdef8): **0 words** each. The 7 LAS compute shaders use only global memory (STG 0xeed8), no shared memory or atomics.
- TLD4: 1 word, `c8380226aff71000` (LAS `postprocess_8fa03920_ps` #6): offset bits 54-55 = 0, dc = 0, component 0, Rb = RZ. Consistent with the decoder; does not exercise AOFFI.
- TLD4S: 1 word, `df02022020970800` (LAS `postprocess_bdd6d575_ps` #19): dc = 0, AOFFI bit 51 = 0. Does not exercise the offset path.
- No decoder or test-encoder change was made in step 3: there is no real word that disagrees with (or confirms) the from-memory layouts.
