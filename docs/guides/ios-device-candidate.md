# First iPhone run: which title, how to build and install it

Measured 2026-10-27 on the iOS Simulator (iPhone 17 Simulator, Apple-silicon Mac under heavy load from other sessions), branch `claude/ios-candidate-2026-10-27`. Audio off (`NAPPLE_AUDIO_OFF=1`). Data: Testing Material, keys 22.1.0, firmware 22.1.0. Nothing here has run on a physical iPhone yet.

**Recommendation: The Legend of Zelda: Link's Awakening (LAS, `01006BB00C6F0000`, base v0).** It is the only title that boots, renders correctly and plays on the Simulator. A signed device IPA is already built (see below).

## Ranked table

Footprint = `phys_footprint` of the app process (what iOS jetsam judges). The Simulator keeps Metal textures outside the process, so on a phone add the GPU share (unmeasured, `memlog:` prints it on a device).

| # | Title | Footprint peak / steady | Simulator fps | Visual quality | Input | Verdict |
|---|---|---|---|---|---|---|
| 1 | **LAS** | 640 MB peak in the intro movie, 790 MB peak in play; steady 550-620 MB (title / menus), 780 MB in play (Marin's room) | 57-60 in play and in the 1080p intro movie (`NAPPLE_PERF_LOG`), `gpu_ms_max` about 1-2 ms | Correct: intro movie, title, file select, difficulty, name entry, the opening cutscene and Marin's house all render with the tilt-shift look, no visible glitches | Scripted keys (`NAPPLE_INPUT_SCRIPT`) drive it from boot to gameplay; the on-screen pad draws correctly | **Go.** Needs the title's L+R at the title screen, name entry through the system keyboard applet |
| 2 | SMO | 743 MB steady, 861 MB peak at the menu (35 s after launch) | menu only | Start / Assist Mode menu renders correctly | A on the menu starts "Compiling shaders" and the process sits at 0 % CPU (Simulator GPU: 32-byte colour attachment budget, device A11+ has 64; known from the earlier run, which aborted with the 48-byte assertion) | Not testable on the Simulator past the menu. Second choice for a phone only if LAS fails; a 5 GB game and 1 GB+ footprint once in the world are likely |
| 3 | ACNH 3.0.3 + DLC | none: aborts in 3 s | - | no frame | - | Blocked on the Simulator: `Texture Cube Array is not supported on this device` (`mtl_tex_get`). A-series devices support cube arrays, so a phone may pass; unverified. 11 GB of data |
| 4 | MK8D (latest update, AArch64) | none: aborts in 4 s | - | no frame | - | Blocked on the Simulator: `source texture pixelFormat RGBA8Unorm not compatible with texture view pixelFormat RGBA32Uint` (a texture-view usage flag missing in `mtl_tex.m`, gfx owner). 11 GB of data. Not a first-run candidate |

Screenshots (`docs/screenshots/ios-candidate/`): `las-intro`, `las-title`, `las-file-select`, `las-device-layout-difficulty`, `las-marin-room-1`, `las-marin-room-2`, `smo-menu`, `smo-after-A-compiling-shaders-hang`.

## Memory against the iPhone limit

* LAS in play: **782 MB footprint** (guest-mapped 3.28 GB of virtual size, almost all untouched), 60 fps. Far under the 1.8 GB target. No pool or budget change was needed for LAS; the footprint is dominated by the guest's own touched pages and the 5-6 GB of address space is free (only touched pages count).
* The device layout was exercised on the Simulator: `NAPPLE_HEAP_CAP_MB=3328 NAPPLE_ALIAS_GIB=1` reproduces the 6 GiB device window (3.25 GiB heap cap, 1 GiB alias). LAS boots, plays the intro movie, reaches the title and the new-game flow with it (the old note that LAS aborts in its movie library with the 3.25 GiB cap did not reproduce). Footprint 551-622 MB.
* The paired phone is an **iPhone 16 Pro Max (8 GB)**; the profiles on this Mac belong to a free account (no `increased-memory-limit`), so the default limit applies. A 790 MB peak plus the GPU share leaves a very large margin.
* New: a **memory log** (`runtime/src/memlog.m`): every 20 s on iOS (`NAPPLE_MEMLOG=1` elsewhere, `NAPPLE_MEMLOG_SECS`) one `memlog:` line with footprint, footprint peak, guest-mapped bytes, `MTLDevice.currentAllocatedSize` (0 on the Simulator) and, on iOS, `os_proc_available_memory()`. It is the first thing to read after the first phone run (Files > Napple > logs, or `devicectl device process launch --console`).
* New cheap optimisation: **iPhone / iPad default to handheld mode (720p)**, via the input binding default (`input/src/bindings.c`); a docked 1080p render target costs GPU memory and heat a phone does not need. Verified on the Simulator (`operation mode: handheld (720p)`). A saved `input.json` or `NAPPLE_OP_MODE=docked` overrides it.

## Input

* On the Simulator the Mac cannot deliver touches to the app (`simctl` / tool taps never reach it, same as the earlier notes), so the touch pad is exercised only visually here. Use a physical controller (the pad hides when one is connected) or the touch pad on the phone itself.
* Scripted keys work and were used for all runs. LAS: `NAPPLE_INPUT_SCRIPT="100:L+R,112:A,119:A,126:A,133:DOWN,138:A,..."` with `NAPPLE_SWKBD_ANSWER=Link` reaches the game; on a phone press L+R (the touch L and R buttons together) at the title, A through the menus, A on the name field (keyboard applet) then down to OK.

## Build and install the LAS IPA (free account, this Mac)

Already built here (signed with `Apple Development: er.talevi@duocuc.cl (7HV79NKWDA)`, profile `iOS Team Provisioning Profile: com.napple.las`, team P2CG46TQ57, device 00008140-001421A2219B001C = the paired iPhone 16 Pro Max; `codesign --verify --deep --strict` passes; profile expires **2026-11-02**):

`~/Desktop/NappleIOS/LAS-com.napple.las.ipa` (4.0 GB). Not installed on the phone by this session.

Install it (phone unlocked, Developer Mode on, trusted):

```bash
xcrun devicectl device install app --device 00008140-001421A2219B001C ~/Desktop/NappleIOS/LAS-com.napple.las.ipa
xcrun devicectl device process launch --console --device 00008140-001421A2219B001C com.napple.las
```

First launch of a free-account app: Settings > General > VPN & Device Management > trust the developer certificate. Studio route: iPhone & iPad pane, drop the LAS NSP, Target iPhone, pick identity `6C04BD3C...` and profile `com.napple.las`, Build + Install.

Rebuild from scratch (needed after 2026-11-02 or a code change; 10 GB of scratch disk, about 4 minutes):

```bash
cd Napple
VPX=1 tools/build_ios_deps.sh && tools/build_ios.sh                   # libopus + libvpx, runtime/build-ios/napple-runtime
TM="<Testing Material>"
target/release/napple build "$TM/The Legend of Zelda Link’s Awakening [01006BB00C6F0000][v0].nsp" \
  -k "$TM/Keys 22.1.0/prod.keys" --firmware "$TM/Firmware.22.1.0.zip" --target ios \
  --runtime runtime/build-ios/napple-runtime --signing-identity 6C04BD3CC87D879A77B9C36B557ABB477557D69F \
  --provisioning-profile "$HOME/Library/Developer/Xcode/UserData/Provisioning Profiles/3c8c76a4-0b33-402a-bc38-95996824b97f.mobileprovision" \
  --bundle-id com.napple.las --ipa -o out/las
```

`napple build --list-signing` shows the identities and profiles. Two other profiles exist (`com.napple.smo`, `com.napple.mk8d`) for the other titles. Identity 4 (`E2817BD6...`) is a different certificate that those profiles may or may not carry; `6C04BD3C...` is the one in the LAS profile.

## What to check on the phone, in order

1. The app opens and reaches the intro movie (a `cannot load ... dlerror` line means dyld refused a hand-built dylib; see `docs/architecture/ios.md` "Risks").
2. `memlog:` lines: footprint should stay under 1.5 GB; `metal ... MB` is the GPU share; `system still allows` should stay above 1 GB.
3. Title (L+R), file select, new game, Link wakes up in Marin's house, walk.
4. Thermals after 10 minutes; 60 fps at 720p is the expectation.

## Not covered / open

* Everything physical: exec-page rules, dyld on a phone, jetsam, thermals. LAS's `memlog` numbers on the device replace the estimates above.
* The Simulator-only GPU limits that block SMO, ACNH and MK8D are gfx work (colour attachment 48 -> 32 bytes, cube arrays, texture-view usage), left to the gfx owners; not touched here.
* `memlog` GPU figure is 0 on the Simulator.
