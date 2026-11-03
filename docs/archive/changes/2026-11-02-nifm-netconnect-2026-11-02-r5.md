# nifm-netconnect (2026-11-02, batch r5): nifm Cancel, netConnect's storages, the real NetConnect output

Branch `claude/nifm-netconnect-2026-11-02-r5` (Mac session, base `orchestrate/2026-11-02-r5` = `2a5dab5`).

Integration of r4 found that MK8D's Online Play never opened the network-connection applet (NetConnect, applet id 0x0f). The game calls Cancel before
GetAppletInfo, and Cancel cleared the rejected request's result. With that fixed experimentally, MK8D opened NetConnect but retried about 20 times,
because our NetConnect output was wrong. Both are now fixed from evidence. **Verified** on MK8D update Online Play offline.

## Evidence (disassembly)
Tools: the module images (`<data>/modules/<m>.patched.bin`) wrapped in an AArch64 ELF with their exported symbols, `llvm-objdump` from Xcode, and PLT
stubs resolved through the `.tables` relocations. The scripts are in the batch's shared notes folder (`nifm-netconnect-evidence/`). The firmware
programs were built privately with `napple build --firmware-program <id>`.

**MK8D update sdk** (nnSdk, `fe941ed5ba14be5d`, exported symbols):
- `nn::nifm::detail::RequestClient::PrepareLibraryApplet` (+0x150e84):
  - calls IRequest::GetAppletInfo (theme color from `nn::oe::GetExpectedThemeColor`, a 0x1000-byte buffer), then `nn::applet::CreateLibraryApplet(id, mode)`;
  - walks the buffer as `{s32 size, data}*` up to size -1, and for each entry creates a storage, writes it and calls PushToInChannel;
  - pushes **nothing of its own, not even CommonArguments**. "Used" < 4 gives 2110-0200; an entry larger than what is left gives 2110-0300.
- `nn::nifm::FinishHandlingNetworkRequestResult` (+0x155c44): after Join, exit reason 0 pops one output storage through
  `nn::la::AppletToNifmArgumentsReader`. No storage gives 2110-0180; exit reasons 1 / 2 / other give 2110-0183 / 0184 / 0185.
- `nn::la::AppletToNifmArgumentsReader::ReadFromStorage` (+0x1b5374):
  - 4 bytes: `{s32 code}`, where 0 gives 2110-0190 and anything else gives 2110-2900;
  - 8 bytes or more: `{u32 header; Result}`. The Result is read from **offset 4**. Header 0 requires exactly 8 bytes; a nonzero header accepts more.
- `nn::nifm::NetworkConnection::HandleNetworkRequestResult` (+0x156ee4) turns a Result of 0 into 2110-0190 (ErrorHandlingCompleted), and MK8D
  resubmits on 0190 (main +0x86ec54 compares with 0x17c6e).
- `nn::la::NifmToNetConnectArgumentsWriter` (+0x1b58e8 .. +0x1b59f8): netConnect's argument is 0x18 bytes, `{u32 0; Result; Uuid}`.
  `CommonArgumentsWriter::Export` (+0x1b5898): `{u32 1, u32 0x20, u32 la_version, u32 theme_color, u8 play_startup_sound, pad, u64 tick}`.

**Firmware 22.1.0 netConnect** (0100000000001006, stripped; the same code shapes as the SDK's):
- output writer: `Writer(bool connected)` (+0x245d80) gives 2110-0190 when connected and 2110-2900 otherwise. `PushToOutChannel` (+0x245da0)
  creates an 8-byte storage `{u32 0 (constant +0x3fe4b4); Result}` and pushes it.
- called at its exit (+0x314cec) with `connected = (exit code == 0)`. When a version field it holds is 5 or more, it first pushes a 4-byte
  `{s32 exit code}` storage. The SDK maps that one to the same two Results.
- input readers: CommonArguments (+0x245e40) and `{u32 header; Result; Uuid}` (+0x245f80; header 0 means exactly 0x18 bytes).

So the old output `{u32 2110-1111; u32 0}` read as Result **0**, which the SDK turns into "error handling completed", so the game retried. On a
console that cannot connect, the user backs out of netConnect and the game gets 2110-2900.

## Changes
- `offline_core.c` `nifm_request_cancel`: only an **accepted** request is released (provider `net_release`) and reset. A request nifm rejected
  keeps its state (Free) and its result, so GetAppletInfo after Cancel still names netConnect.
- `offline.c` IRequest 21 GetAppletInfo: a failed request now lists the two storages netConnect reads:
  - CommonArguments: version 1, 0x20 bytes, la version 0 ([unverified]: the nifm sysmodule's value was not found), the theme color passed in,
    and the system tick;
  - netConnect's argument `{0, the request's result, uuid 0}`;
  - then -1. "Used" is 0x44 (or 4, only the end marker, when the buffer is too small).
- `applet_lib_core.[ch]` NetConnect handler:
  - output `{u32 0; 2110-2900}` (`APPLET_NETCONNECT_R_NOT_CONNECTED`), one storage, GetResult success;
  - `APPLET_NETCONNECT_R_CONNECTED` (2110-0190) is defined for reference.
- Filler: `vi.c` IApplicationDisplayService 2102 ConvertScalingMode is now real. It maps the SDK's ScalingMode (None, Freeze, ScaleToWindow,
  ScaleAndCrop, PreserveAspectRatio) to the numbering SetLayerScalingMode takes (Android's order, `VI_SCALE_*`); any other mode is an error.
  The fs.c rows 15 / 16 / 606 / 618 were left as they were: their layouts were not confirmed here.

## Verified on MK8D (update v1376256, offline)
Run: headless, `NAPPLE_INPUT_SCRIPT=15:A,19:DOWN,20.5:DOWN,22:A[,32:A]`, `NAPPLE_TRACE_IPC=all`, private save copy.
- First attempt, at 23.2 s: IRequest Submit, then GetResult (2110-1111), then Cancel.
- At 24.8 s: GetAppletInfo, **`CreateLibraryApplet id 0xf mode 0` once**, two PushInData (the listed storages), Start, GetResult, one
  PopOutData (GetSize, Read).
- No second Submit and no loop. The game returns to the main menu with Online Play selected (screenshot).
- A second attempt from the menu (32 s) repeats the same sequence once: two attempts, two applets.
- rc 0, `missing_ipc []`.

## Tests
- `test_svc_nifm_audit` (144 checks):
  - GetAppletInfo's list, byte by byte;
  - a buffer too small for the arguments gets only the end marker;
  - Cancel on a rejected request keeps its result and the applet info;
  - Cancel on an accepted request, through an installed provider that connects: the connection is released once, the request is reset,
    and IsAnyInternetRequestAccepted goes false.
- `test_svc_applet_am` (70): NetConnect with the listed storages pushed gives output `{0, 0x16a86e}`, one storage only.
- `test_svc_offline` (70): its two "Cancel clears the result" pins now expect the kept result.
- `test_svc_vi` (123): ConvertScalingMode.
- `make -C runtime GFX=napple test test-services test-ipc-reference`: rc 0, `ipc_reference` 111 fixtures, missing 0.
