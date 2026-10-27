# ipc-depth-rest2 (2026-10-27): remaining stub IPC rows, network rows through OnlineProvider

Branch `claude/ipc-depth-rest2-2026-10-27` (base `orchestrate/2026-10-27-b3` 06a0205). Cloud (Linux) session: everything below is **tested on Linux, not verified on macOS**.

## What changed
IPC scan (`IPC_STUB }` rows, whole runtime) 1009 impl / 125 stub -> 1056 / 98. In the owned files 29 -> 2 (account 8->0, vi 8->2, mii_img 4->0, ns 3->0, settings 3->0, offline.c 3->0).
- **bsd** (`offline.c`, `bsd_core.c`): `DuplicateSocket` aliases the descriptor (`bsdg_dup`; the provider socket closes with the last alias); `SendMMsg`/`RecvMMsg` marshal FreeBSD LP64 `mmsghdr` arrays with iovecs. `RegisterClient` is untouched and still answers 0 (a nonzero id broke SMO boot).
- **OnlineProvider** (`online_provider.h`): table grown at the end (ABI version stays 1, `ONLINE_FN` hides the new entries from older modules): caps `ONLINE_CAP_LDN`, `ONLINE_CAP_ACCOUNT`; ldn scan/create/destroy/connect/disconnect/get_network/set_advertise/reject; `account_info`. No Nintendo service code, no endpoints.
- **ldn** (`ldn.c`, `ldn_core.c`): GetNetworkInfo, GetIpv4Address, GetSecurityParameter, GetNetworkConfig, Scan, CreateNetwork, Connect, Disconnect, Reject, SetAdvertiseData, GetDisconnectReason go through the provider; NetworkInfo (0x480) encode/decode in the core. NullProvider answers are unchanged.
- **account** (`account.c`): CheckAvailability / GetAccountId / NA resource cache from `account_info` when linked, else the old offline answers; `InitializeApplicationInfo{V0,,V2}` remembered per connection; registration/switch-lock rows are documented constants.
- **vi** (`vi.c`, new `vi_core.*`): layer table (scaling mode validation, position/size/z/visibility, stacks), display enable flag, binder refcounts, real `ISystemDisplayService`/`IManagerDisplayService` rows. `TransactParcel`/`TransactParcelAuto` stay stubs (the graphics back end never uses the producer).
- **mii_img** (new `mii_img_core.*`): in-memory image database (100 slots), supplied JPEGs stored as given, otherwise a deterministic 96x96 face JPEG generated from the Mii id with the portable JPEG encoder.
- **ns**: `GetApplicationControlData`, `GetApplicationContentPath`, `ResolveApplicationContentPath` read `<data_dir>/control.nacp` (mod-glue-core writes it at build time); not found without it.
- **settings**: `GetKeyCodeMap`/2/3 return a US layout (16 bytes per HID usage).
- Tests: new `test_svc_account.c`, `test_svc_vi.c`, `test_svc_mii_img.c`; extended `test_svc_online.c` (dup, mmsg), `test_svc_ldn.c` (provider network over the loopback provider), `test_svc_ns.c`, `test_svc_settings.c`; `tests/online_loopback.h` gained ldn/account. `tests/linux/run.sh` adds `-I../gfx/include` for vi. 86 rows appended to `known_calls_ipc_depth.txt`.

## Decisions
- NullProvider paths must keep today's answers (success, fixed ids), because an error where a title saw success has broken boot before (bsd RegisterClient).
- Command ids and argument layouts for the new rows are from memory of the public documentation (unverified); unknown layouts are read leniently and never fatal.
- nifm: no new rows added. The existing nifm ids are the author's numbering and adding rows at guessed ids risks collisions; nifm already routes through `net_info/net_connect`.

## Not verified
Nothing here was compiled or run on macOS. Mac checks remaining: `make -C runtime test-services`, LAS/SMO/ACNH/MK8D headless boot 45 s, and a Mii applet run using miiimg.
