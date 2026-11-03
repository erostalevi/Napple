# 2026-10-26 sim-online: online provider hooks, NFC / amiibo simulator, IR camera simulator

Branch `claude/sim-online-2026-10-26-macos1` (Linux cloud session, assignment `orchestrations/sim-online.md`, batch macos1). Everything here is **tested on Linux, never compiled or run on macOS**
(status word: *unbuilt* for the Mac); the Mac checks that remain are listed at the end. The user's rules were followed: no questions asked, decisions recorded below, no keys, dumps, amiibo
data or online endpoints in the repository (proof below).

## What changed
| Area | Result |
|---|---|
| Online provider (`online_provider.h`, `online_core.c`, `bsd_core.[ch]`, `offline_core.c`) | One C ABI table (`OnlineProvider`, version 1, `struct_size` forward compatible, capability flags) for sockets, DNS, nifm connection, TLS, friends/presence, nsd, play reports. NullProvider = the previous offline answers moved behind the table. Plugin boundary: `NAPPLE_ONLINE_PROVIDER=<dylib>` exporting `napple_online_provider_v1`; absent or invalid = NullProvider; iOS never loads modules. |
| `bsd:*` | Guest descriptor table (lowest-first, 128) over provider sockets; Socket, Select, Poll, Recv/RecvFrom, Send/SendTo, Accept, Bind, Connect, Get/SetSockOpt, GetSock/PeerName, Listen, Ioctl(FIONBIO), Fcntl, Shutdown, ShutdownAllSockets, Read/Write, Close. sockaddr codec (v4/v6). |
| `nifm:*` | Submit asks the provider; request states, GetCurrentIpAddress, connection status, accepted-request counters, release on Cancel/destroy. |
| `sfdnsres` | GetHostByName/Addr, GetAddrInfo, GetNameInfo (+WithOptions) serialised in the resolver wire format (`sfdnsres_core.c`). |
| `ssl`, `friend:*`, `nsd`, `prepo` | Routed through the provider (handshake/read/write/pending; friend count/list/ids/presence; nsd resolve; play reports). |
| NFC / amiibo (`amiibo_core.c`, `amiibo_crypto_core.c`, `amiibo_lib_core.c`, `nfp_core.c`, `nfc_source.h`, glue in `offline.c`) | Virtual amiibo (id + generated UID + nickname/owner + persisted application area) as one JSON per amiibo in `<save-root>/system/amiibo/`; full nfp state machine and structures over an `NfcTagSource`; `nfc:user` incl. pass-through READ/FAST_READ/GET_VERSION; `nfc:mf:u` registered (the NPDM of LAS and SMO list it; Read/Write report failure); AmiiboAPI-format metadata library with cache fallback; picker contract; `NAPPLE_AMIIBO` headless script; optional real-dump import with the user's key file (test-only key in the tests). |
| IR camera (`irs.c`, `irs_core.[ch]`) | Moment, clustering, image-transfer processors over an `IrFrameSource` (320x240), seqlock states in the irsensor shared memory, transfer memory, pump thread, access request on first processor start, webcam adapter (`irs_adapter_rgb_to_ir`), scripted sources (`NAPPLE_IR_SOURCE`). |
| Docs | `docs/guides/network-providers.md`, `docs/guides/hardware-simulators.md` (incl. the Mac wiring list), `runtime-services.md` section, environment-variables rows, guides index. |

## Tests (Linux: `sh tools/linux-check/run-tests.sh`, gcc and clang, also clean under ASAN/UBSAN apart from the mock's own leaks)
| Test | Checks | Covers |
|---|---|---|
| `test_svc_online` (new) | 222 | NullProvider answers pinned for the known calls (Socket/Connect/Poll/Select/Fcntl/Close, nifm Submit/IP/status, GetHostByName/GetAddrInfo, ssl handshake, friends, nsd, prepo); provider validation; plugin loading (three modules built with the host compiler: good, wrong ABI, missing symbol; missing file); address codec; the loopback provider serving a bsd echo connection (bind/listen/connect/accept/send/recv/poll/select/ioctl/fcntl/close, EMFILE), DNS serialisation, nifm accept/release, TLS over the socket, friends, nsd, prepo |
| `test_svc_irs_sim` (new) | 93 | moment/clustering/image-transfer maths on synthetic frames, adapter, scripted sources, access flow (denied / pending / granted), the irs service end to end (pump thread, shared memory seqlock, transfer memory) |
| `test_svc_amiibo` (new) | 122 | SHA-256 / HMAC / AES-128-CTR against published vectors, base64, virtual amiibo, JSON, store, dump pack/unpack with a generated key (wrong key, flipped bits), AmiiboAPI and database documents, refresh/cache policy, picker, scripts |
| `test_svc_nfp` (new) | 175 | every nfp state transition and result, structures, application area, flush/restore/format, tag removal and swap, pass-through, nfp/nfc/mifare services with events through the picker listener, `NAPPLE_AMIIBO` immediate and timer-thread scripts |
| `test_svc_offline` (changed: the table reset line only) | 70 | unchanged expectations, all still pass |
| `test_svc_irs`, `test_svc_sfdnsres` (unchanged) | 19, 15 | still pass |
Whole suite: 33 binaries run, all `"failed":0`; `test_svc_fs`, `test_svc_fs_dataid`, `test_svc_time` still do not link on Linux (as before this change).

## IPC command depth (IPC_IMPL / IPC_STUB rows)
`offline.c` 1 / 146 -> 106 / 80; `irs.c` 1 / 17 -> 11 / 7; `sfdnsres.c` 2 / 12 -> 10 / 4. (Counted with `grep -cE 'IPC_IMPL\}'` and `'IPC_STUB\}'` on the base and on HEAD; `/percent-analysis` is not run by parallel sessions.)

## Decisions (nobody could be asked)
- **No reader by default.** nfp/nfc keep the old answers (no device, Unavailable, detection calls accepted) until a tag source is installed (frontend or `NAPPLE_AMIIBO`). The four titles all use nfp; changing their device list before the Mac wiring exists would be an untested behaviour change.
- **One amiibo application area** per amiibo (the NTAG215 has one); the spec's "areas as base64" is the single `app_area.data`.
- **Reuse vs new**: selecting a model reuses the stored virtual amiibo (so progress persists); `AMIIBO_PICK_NEW` makes another.
- **Created areas are open**; unused area bytes are filled with zero (a console fills them with random data).
- **Owner Mii**: virtual amiibo keep a `CharInfo` (what RegisterInfo returns); a dump's 3DS-format Mii is kept verbatim (`owner_raw`) but not converted.
- **TeraPlugin/Dpd/IrLed/ImageTransferEx** are accepted while a source exists and produce nothing (failing them would break flows that only need the call to succeed).
- **Provider `dns_resolve` returns EAI_* only**; the hostent calls map it to netdb codes in the service.
- `friend` record layout (0x300 bytes: id at 0, name at 0x40) is invented to carry the data; mark unverified.
- AmiiboAPI's URL is the default value of a configuration setting, not data; the data is never fetched at build time or stored.

## Proof: nothing that must not be in the repository
- `git diff <base>..HEAD --name-status`: only `.c`/`.h` sources, tests and docs; no `.bin`, `.json` data, keys or dumps.
- URLs added: `amiiboapi.com/api/amiibo/` (the default metadata source setting), `github.com/N3evin/AmiiboAPI` (docs), and `.invalid` hosts in tests. `git diff | grep -ciE 'nintendo|baas|dauth|aauth|npns|nas\.'` on added lines: 0.
- Crypto in tests uses keys derived from a counter inside `test_svc_amiibo.c`; no retail key material exists in the repository.

## Not done / remains
- Nothing built or run on macOS. Mac checks: `make -C runtime test test-services test-host`; boot LAS/SMO/ACNH/MK8D headless 45 s (exit 0, no CRASH: the nfp/irs/bsd paths change only when a source/provider is installed, so no change is expected, but the services were rewritten); then `NAPPLE_AMIIBO=<id>` on a title that reads amiibo.
- All command layouts, struct layouts, result descriptions and the real-dump compatibility are from memory and unverified (listed in the integrator file).
- Frontend wiring (picker panel, webcam source, permission prompt, Info.plist/entitlement) is only specified (`docs/guides/hardware-simulators.md`).
- `bcat` network parts, `ldn`, `account` network service calls and `olsc` were not routed (not mine / not network-facing here). `friend` notification queue stays empty.
