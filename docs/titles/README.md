# Titles

Per-title notes for the four reference titles: what each title needs from Napple, what is open, measured numbers, and title-specific findings that the tool pages only describe in general terms.
Numbers are scripted/measured (headless unless said) on an Apple-silicon Mac, never "played" unless a note says a person did. The generated per-title tables are the [support matrix](../reference/support-matrix.md) and [online support](../reference/online-support.md); the full stage matrix is [COMPATIBILITY.md](../../COMPATIBILITY.md).
Symptom lookup: [../symptoms.md](../symptoms.md). Dated history: [change archive](../archive/changes/README.md). How to add a title: [bring up a new title](../guides/add-game-support.md) and [document a title](../guides/contributing-document-a-title.md).

| Title | Id | macOS | iOS Simulator | Note |
|---|---|---|---|---|
| Link's Awakening (LAS) | 01006BB00C6F0000 | plays, ~58 fps headless, 60 fps live | title screen (2026-10-26 re-run) | [01006BB00C6F0000](01006BB00C6F0000.md) |
| Super Mario Odyssey (SMO) | 0100000000010000 | plays, 60 fps live | start menu; abort on A (48-byte colour attachments) | [0100000000010000](0100000000010000.md) |
| Animal Crossing: New Horizons (ACNH) | 01006F8002326000 | boots through the opening and shows its picture through the game's own window pass, ~30 fps | abort at ~2 s (48-byte colour attachments) | [01006F8002326000](01006F8002326000.md) |
| Mario Kart 8 Deluxe (MK8D) | 0100152000022000 | update + DLC runs races at 40-47 fps | 3.0.5 aborts at ~2 s (48-byte colour attachments; time:u fixed) | [0100152000022000](0100152000022000.md) |

## Building test data without disk pain (all titles)
The in-place mode writes a `romfs.map` (extents of the NSP files plus AES-CTR parameters) instead of a RomFS copy: about 250 MB per title instead of 5-19 GB. Development only: the map holds keys and host paths, never ship it, and an iOS device bundle needs the real RomFS.
```bash
cargo build --release -p napple-cli
target/release/napple build <nsp files or folder> -k "<Testing Material>/prod.keys" --firmware "<Testing Material>/Firmware.zip" --updates latest --dlc all --data-only --update-romfs in-place -o <scratch dir>
```
Run headless (the data dir is `<scratch>/<title id>/<build id>`; take the newest version dir for an update):
```bash
runtime/build/napple-runtime <data dir> --headless --seconds 45 --save-root <tmp save dir>
```
Passing: exit 0, a `stats: frames N` line with N in the thousands, no `CRASH`. Useful env: `NAPPLE_PERF_LOG=1` (fps, GPU ms, RSS), `NAPPLE_INPUT_SCRIPT="15:A,19:A:0.3"` (seconds after HID is configured, or frames with an `f` prefix : button [: hold seconds]), `NAPPLE_SCREENSHOT` (see the [subsystem map](../reference/subsystems.md)).
`tools/e2e/run.sh` takes the short names `las`, `smo`, `acnh`, `mk8d` or `all` ([testing guide](../guides/testing.md#end-to-end-toolse2erunsh)).
Game files, keys and firmware stay outside the repository (`<Testing Material>` is any folder you choose; never commit it). Delete scratch data when done.

## Graphics measurements

Cross-title graphics data measured on the reference titles. The mechanisms are described in [gfx.md](../architecture/gfx.md).

### Translator evaluation
When the native translator was chosen, it translated 671 of 679 Link's Awakening shaders (8 had unknown opcodes at the time) and 128 of 128 sampled Super Mario Odyssey shaders; 295 of 295 sampled results compiled with `metal -std=metal3.2`.

### Shader corpus
Every distinct program of base + updates + DLC, extracted with `tools/gpu/scan_stages.py extract` and run through `t_xlate`'s corpus mode ([gpu-title-scan.md](../guides/gpu-title-scan.md)):

| Title | Programs | Translate and compile |
|---|--:|---|
| Link's Awakening | 5,767 | all |
| Animal Crossing: New Horizons | 8,577 | all |
| Mario Kart 8 Deluxe | 26,544 | all |
| Super Mario Odyssey | 31,750 | all |

None of the 72,638 programs writes the viewport index or the layer.

### GPU stage scan

| Title (versions scanned) | Programs (distinct vs / fs / cs) | Geometry | Tess control / eval | Transform feedback | Where the geometry programs are |
|---|---|---|---|---|---|
| Link's Awakening 01006BB00C6F0000 (v0) | 6429 (2881 / 2858 / 26) | 0 | 0 | 0 | - |
| Super Mario Odyssey 0100000000010000 (v0) | 13910 (5045 / 8107 / 68) | 2 distinct (line strip, 64 and 2 vertices) | 0 | 0 | `ShaderData/alThunderShader.szs`; 3 hashes created at boot |
| Mario Kart 8 Deluxe 0100152000022000 (v1376256 patched + Booster DLC) | 36275 (10402 / 13239 / 24) | 1 (code 0x300) | 0 | 0 | `Common/Gfx/gsys_resource.Nin_NX_NVN.release.sarc`; not created in boot runs |
| Animal Crossing 01006F8002326000 (3.0.3 patched + 3 DLC) | 9720 (2533 / 3505 / 56) | 8 distinct (triangle strip, 6 vertices) | 0 | 0 | `Swkbd/shader/*OceanShader*`; 2 created at boot |

Cover: geometry from any of Super Mario Odyssey, Mario Kart 8 Deluxe or Animal Crossing; tess control, tess eval, isoline / triangle / quad domains and transform feedback: no title on hand. A control-section dump of 45 s boots of all four titles (20,624 sections) parsed with no transform feedback layout or tessellation word reported, so the candidate control-section offsets stay unproven (not contradicted).

### Guest object sizes
Gaps measured with `NAPPLE_GFX_OBJTRACE=1` over 90 s headless boots (hex bytes, with the neighbour's type and how often each gap was seen; bold = a gap equal to the minimum, i.e. the exact size; "-" = no reading). The rule and the resulting `NVN_MIN` are in [gfx.md](../architecture/gfx.md#guest-object-sizes).

| Object | nvn.h | ACNH 20.5.4 | MK8D 0.12.16 (a32) | SMO 3.5.1 | LAS 8.2.0 | NVN_MIN |
|---|---|---|---|---|---|---|
| NVNdevice | 0x3000 | 0x3078 (texture pool) | 0x3040 (texture pool) | 0x3078 (texture pool) | 0x3020 (queue) | 0x3000 |
| NVNqueue | 0x2000 | 0x20a0 (pool) | 0x2060 (pool) | 0x20a0 (pool) | 0x2028 (window) | 0x2000 |
| NVNsync | 0x40 | 0x60 (window) | 0x50 (command buffer) | 0x60 (command buffer) | 0x58 (sync) x5 | 0x40 |
| NVNevent | 0x40 | not used | not used | not used | not used | 0x40 |
| NVNmemoryPool | 0x100 | **0x100** (buffer) x17 | **0x100** x12 | **0x100** x28 | 0x120 x2 | 0x100 |
| NVNbuffer | 0x30 | **0x30** (pool) x24 | **0x30** (buffer) x3 | **0x30** (pool) x24 | 0x48 (buffer) x50 | 0x30 |
| NVNtexture | 0x80 | 0x88 (pool) x22498 | no credible reading | 0xe0 (texture) x3 | 0xe0 (texture) x619 | 0x80 |
| NVNsampler | 0x30 | 0x78 (sampler) x8691 | 0x78 x2214 | 0x78 x376 | 0x78 x36 | 0x30 |
| NVNtexturePool | 0x20 | **0x20** (sampler pool) | **0x20** (sampler pool) | **0x20** (sampler pool) | 0x138 | 0x20 |
| NVNsamplerPool | 0x20 | **0x20** (sampler) | 0x3140 | **0x20** (sampler) | 0x1430 | 0x20 |
| NVNprogram | 0xC0 | **0x80** (buffer, program alive) x2 | 0x100 (program) x3661 | 0xc0 (buffer) x1 | 0x100 (program) x133 | **0x80** |
| NVNwindow | 0x180 | 0x1a0 | 0x190 | 0x1a0 | 0x1a0 | 0x180 |
| NVNcommandBuffer | 0xA0 | 0xc0 (pool) | 0xb0 (pool) | 0xc0 (pool) | 0x140 (command buffer) x19 | 0xA0 |
| NVNvertexAttribState | 0x4 | **0x4** array x370179 | **0x4** x93678 | - | **0x4** x56 | 0x4 |
| NVNvertexStreamState | 0x8 | **0x8** array x191214 | **0x8** x71962 | - | **0x8** (to attrib) x148 | 0x8 |
| NVNcolorState / NVNchannelMaskState / NVNpolygonState | 0x4 | **0x4** (next state) | **0x4** | - | **0x4** | 0x4 |
| NVNblendState / NVNdepthStencilState | 0x8 | weak | weak | weak | weak | 0x8 |
| NVNmultisampleState | 0x18 | 0x30 | 0x2c | - | 0x2c | 0x18 |
| NVNtextureView | 0x10 | weak | weak | weak | weak | 0x10 |
| builders | as nvn.h | weak | weak | weak | weak | as nvn.h |

Notes: Mario Kart 8 Deluxe's texture gaps are stack temporaries of a 32-bit title (see its page). Animal Crossing's NVNprogram is followed by an NVNbuffer at +0x80 (see its page). Builders, views and most state objects are stack locals and the "recent objects" pairing mixes frames of different calls (for example a texture view 8 bytes from another cannot be a real size), so for them only array strides count; the other minimums are nvn.h's and no title contradicts them.

### Golden-frame routes
`tools/golden/routes.json` defines one rough route per title (input times are guesses until a person has watched them): Link's Awakening boot to the village, Super Mario Odyssey title and first kingdom (frames 8000 and 9000 are the sleep scene that showed exploded geometry), Animal Crossing airport counter, Mario Kart 8 Deluxe title to a race. Routes still to add: beach / forest (Link's Awakening), name entry and island arrival (Animal Crossing), cup select (Mario Kart 8 Deluxe).

### Media devices
No reference title opens `/dev/nvhost-nvjpg` or `/dev/nvhost-msenc`, so the X1 offsets of those engines are confirmed by no real stream yet.
