---
branch: claude/net-host-provider-2026-11-02-r6
orchestration: net-host-provider
base: 87938c7
head: e0468f3 (code and docs; the integrator file is the next commit)
env: mac
date: 2026-11-02
areas: [runtime/net, runtime/services]
status: ready-with-caveats
---

# claude/net-host-provider-2026-11-02-r6: a built-in, opt-in macOS host network provider (sockets, DNS, TLS, nifm, LDN over LAN)

## Summary
`NAPPLE_NET=host` selects a new built-in `OnlineProvider` (`runtime/src/net/`) that puts the guest on the Mac's real network: BSD sockets (bsd:u),
the host resolver (sfdnsres), TLS clients verified against the host trust store plus guest-imported CAs (ssl, SecureTransport), nifm connectivity from the
Mac's primary IPv4 interface, and LDN emulated over the LAN between Napple instances (UDP discovery + TCP session, real LAN addresses as node addresses).
Default stays offline; with the variable unset every service answers exactly as before (all existing tests unchanged and green). The provider contains no
online-service endpoint, token, certificate or authentication, and refuses to resolve the console vendor's domains (requirement added at hand-off): friends, presence, prepo, nsd, accounts keep their offline answers.
Service side: ldn.c shows every node, fires the state change event on provider notifications and moves a station whose host left back to StationOpened;
after nifm-netconnect landed in main (merged here), ssl ImportServerPki keeps the certificate and DoHandshake hands imported CAs and SetVerifyOption to the provider.
Everything in the assignment is built and verified except one part of acceptance 3 ("ends cleanly"), which fails in the applet area. Titles: the four 60 s boots are clean with and without the variable (no crash, no missing IPC); SMO and ACNH get their nifm requests accepted with the Mac's IP. **Caveat:** MK8D's scripted Online Play with `NAPPLE_NET=host` now gets past nifm, is refused an account ID token (no Nintendo account: expected), shows the error applet, and then the guest calls svcBreak because the runtime's error applet gives no output data (`applet.c`, not this item's area; see Needs). Offline, the same path is unchanged (netConnect applet, rc 0).

## Files
Branch-only changes (`git diff --stat c33eb7f..HEAD`, c33eb7f = the main merged in):
- New: `runtime/src/net/net_host.{h,c}`, `net_host_sock.c`, `net_host_dns.c`, `net_host_tls.m`, `net_host_nifm.m`, `net_host_ldn.c`;
  `runtime/tests/test_net_host_{sock,tls,ldn}.c`; `docs/archive/changes/2026-11-02-claude-net-host-provider-2026-11-02-r6.md`; this file.
- Changed (owned): `runtime/src/services/online_core.c` (NAPPLE_NET selection, +20), `ldn.c`, `ldn_core.[ch]`, `offline.c` (ssl only: ImportServerPki,
  SetVerifyOption, DoHandshake; done after merging main with nifm-netconnect, as the orchestration allows).
- Shared, append-only: `runtime/src/services/online_provider.h` (fields appended at the end of `OnlineProvider`, `OnlineLdnNode`, `ONLINE_TLS_VERIFY_*`,
  `ONLINE_ENV_NET`, `online_builtin_host`), `runtime/Makefile` (a 6-line block after the `OBJ :=` line adding `src/net/*.c|*.m`, `FRAMEWORKS += -framework
  Security -framework SystemConfiguration`, test rules at the end; `test-services` now also runs `test_net_host_*`), `docs/guides/network-providers.md`
  (new section), `docs/reference/environment-variables.md` (new table at the end).
- One merge commit of local `main` (2b6727f) brings nifm-netconnect's files; nothing of theirs was edited.

## Built and tested here
| Item | Status | Evidence (command + result) |
|---|---|---|
| Runtime builds with `src/net` (no new warnings) | verified | `make -C runtime` |
| Acceptance 1: sockets (TCP echo, UDP, non-blocking connect + poll, EAGAIN, SO_RCVTIMEO, Close ends a blocked recv, ECONNREFUSED), DNS (localhost, `example.com`, .invalid, reverse), nifm connected with the Mac's IP, provider selection (unset / offline / bogus / plugin wins) and the bsd + nifm handlers | verified | `runtime/build/tests/test_net_host_sock` → 102 checks, 0 failed (Mac online at 192.168.5.229) |
| Vendor-domain block: 9 names under the 7 domains refused with EAI_NONAME, look-alikes not blocked, the redirect hook resolves a redirected name, refuses a redirect to a blocked name | tested | same binary (part of the 102) |
| Acceptance 1: TLS handshake to a local `openssl s_server` with a throw-away CA added as guest CA (DER and PEM), refused without it, wrong host name refused, verify options; through the ssl handlers (ImportServerPki → DoHandshake → Write/Read); `example.com:443` against the host trust store | verified | `runtime/build/tests/test_net_host_tls` → 41 checks, 0 failed |
| Acceptance 1: variable unset → every service as before | verified | `test_svc_online` 263/0, `test_svc_offline` 70/0, `test_svc_ldn` 140/0, `test_svc_sfdnsres` 25/0, `test_svc_nifm_audit`, all of `test-services`; `test_net_host_sock` last check (Connect ENETDOWN again) |
| Acceptance 2: LDN — two provider instances (plus a third) discover, create, join, exchange a UDP packet both ways at the reported node addresses, advertise update, node limit, version mismatch, Reject (reason 5), Disconnect (1), DestroyNetwork (3); then through `ldn:u` (Scan → Connect → NetworkInfo with 2 nodes, GetIpv4Address, host state-change event, station back to StationOpened, Finalize while hosting) | verified (in one process; not with two runtime processes) | `runtime/build/tests/test_net_host_ldn` → 65 checks, 0 failed |
| Acceptance 3: four titles 60 s with `NAPPLE_NET=host` | verified (plain boots); **fails on MK8D Online Play** (applet area) | `NAPPLE_NET=host NAPPLE_TRACE_IPC=all runtime/build/napple-runtime <games>/<tid>/<build> --headless --seconds 60 --save-root <private copy> --summary x.json` for SMO 3ca12dfaaf9c82da, LAS ae16f71e002af8cb, MK8D fe941ed5ba14be5d, ACNH ff1d1c05670db602: rc 0 each, `missing_ipc []`, `unsupported_svcs []`, last stage notify-running. MK8D + `NAPPLE_INPUT_SCRIPT=15:A,19:DOWN,20.5:DOWN,22:A,32:A`: rc 70, guest Break after the error applet (trace in the shared notes folder `net-host-provider-evidence/`) |
| Acceptance 4: four titles 60 s without the variable | verified | same four commands without the variable: rc 0, `missing_ipc []`, `unsupported_svcs []`; no `online:` line; MK8D Online Play script: netConnect applet twice, rc 0 (as nifm-netconnect left it) |
| Acceptance 5: `make -C runtime test test-services test-ipc-reference` | verified | exit 0 (79 test binaries incl. test_net_host_* 65/102/41 checks, 0 failed; re-run on the final tree after the main merge) |

## Unbuilt / untested list (the integrator checks these first)
- Nothing unbuilt. Untested: LDN between two separate runtime processes and between two Macs (only provider instances in one process); a real title's local
  wireless mode (none of the four was driven into it); IPv6 sockets beyond address conversion; `NAPPLE_LDN_PEERS`.

## Findings
- Decision: SecureTransport (deprecated, warnings silenced in that file) is the TLS stack because the guest owns an already-connected socket and
  Network.framework cannot adopt a descriptor. TLS 1.2 minimum; no client certificates.
- Decision: host descriptors are always non-blocking; blocking semantics are emulated in 50 ms poll slices so a Close / ShutdownAllSockets from another
  guest thread ends a wait (macOS does not wake a thread blocked in recv when another closes the fd), and the waiting thread gives its virtual core away
  (`thread_core_away`, weak so tests link without the scheduler).
- Decision: options the Mac lacks are accepted and ignored on set (logged once), ENOPROTOOPT on get; FreeBSD SO_NOSIGPIPE is implied (SO_NOSIGPIPE always on).
- Decision: nsd is left NULL (names unchanged), so templated online-service host names never resolve; no online-service behaviour is emulated.
- Requirement (orchestrator, at hand-off): the provider's DNS path refuses every name under nintendo.net, nintendo.com, nintendo.co.jp, nintendo.eu,
  nintendowifi.net, nintendo-europe.com, nintendoswitch.com (and subdomains) with EAI_NONAME, like an offline resolver (`nh_dns_blocked`,
  `net_host_dns.c`). Hook for napple-online (r7): `extern int (*net_host_redirect_lookup)(const char *name, char *out, size_t n)` in `net_host.h`, NULL
  by default, consulted for every resolved name before the block check (1 = resolve `out` instead); a redirect to a blocked name is refused. Only DNS is
  covered: a literal IP bypasses it (no title seen doing so). `nh_register` now registers only when `online_builtin_host` is still empty, so napple-online's
  wrapper (`net_online.c`) wins in either constructor order (agreed with that session).
- Decision: nifm reports a wired-style connection (kind 2, strength 3, no SSID) with the Mac's real IP/mask/router/DNS; 1 s cache.
- Decision (LDN): node addresses are real LAN addresses (ldn_mitm-style), so titles' own sockets work unmodified; two nodes on one Mac share an address,
  so titles that bind a fixed port on every node need two Macs. The station's user name and the passphrase are not passed to the provider (ABI has no field);
  the accept policy / filters stay in the service. GetIpv4Address still reports mask 255.255.0.0 (ldn.c, pre-existing) — a title computing a subnet broadcast
  from it may miss; a provider mask field would fix it.
- ldn_core: NetworkInfo did not carry `localCommunicationVersion`, so a station always joined with version 0 — now written to and read from NodeInfo +0x2e.
- ldn.c: Finalize while hosting did not end the provider's network (pre-existing; now does).
- ldn (reported by net-rooms r7): MK8D Wireless Play calls SetAdvertiseData after OpenAccessPoint and before CreateNetwork; ldn_core refused it outside
  AP_CREATED (0x40cb). Now SetAdvertiseData, SetStationAcceptPolicy and the filter calls are accepted from AP_OPENED too, the data is only stored until
  CreateNetwork, which hands it to the provider. Tested (test_svc_ldn 142/0, test_net_host_ldn 68/0). MK8D Wireless Play (host) now gets past that and
  hosts the network, opens a UDP broadcast socket on 12345 (all socket calls succeed, `NAPPLE_NET_LOG=1`), then still shows the error applet about
  60 ms later and Breaks (same applet PopOutData abort as Online Play). Not diagnosed here: likely the game rejects the network description (node
  addresses are real LAN addresses, not the console's 169.254/16, and GetIpv4Address reports mask 255.255.0.0). Handed to net-rooms (r7), whose item this is.
- New debug aid: `NAPPLE_NET_LOG=1` logs each socket call and its result (no payloads).
- `tests/test_svc_online.c:204` has a pre-existing `-Wstring-concatenation` warning (not mine, not touched).
- What the titles do with the network (host mode, 60 s boots): SMO submits one nifm request at boot (accepted) and registers bsd; ACNH submits two, asks
  IsAnyInternetRequestAccepted, then cancels them (no further traffic in 60 s); MK8D and LAS make no network request while booting. No title opened a socket,
  resolved a name or started TLS in these runs, so no online-service server was contacted.
- MK8D Online Play (host): nifm accepted -> bsd RegisterClient -> `IManagerForApplication::EnsureIdTokenCacheAsync` (account.c: `sysd_err_generic` stub,
  answers 0xf601 synchronously) -> prepo / friend presence calls (offline answers) -> error applet 0xe with jump-to-settings -> `PopOutData` answers 0x680
  (am, no storage) -> `SetTerminateResult(0x680)` -> svcBreak in sdk (+0xa9f324). The account refusal is the expected "no Nintendo account" outcome; the
  abort comes from the error applet emulation returning no output data where this SDK path pops one (hypothesis: the error viewer's result storage).

## How to integrate
- No ordering constraint. Already contains local main up to c33eb7f (nifm-netconnect landed); merges cleanly with main as of hand-off.
- Conflict hotspots: `online_provider.h` (anyone appending fields: keep both, order of appended fields = ABI order, put later additions after mine),
  `runtime/Makefile` (additive lines only), `offline.c` ssl section (ImportServerPki / handshake), docs tables (append both).
- Build: `make -C input && make -C audio && make -C frontend && make -C gfx && make -C gfx adapter && make -C runtime` → `runtime/build/napple-runtime`.
- No Linux-only calls. `net_host_tls.m` / `net_host_nifm.m` are macOS-only by nature; `src/net` is not compiled by `tools/linux-check` and `online_core.c`
  needs nothing from it (registration through a constructor-set pointer).

## How to test
1. `make -C runtime test-net-host` → three binaries, `102 / 41 / 65 checks, 0 failed`. Needs `/usr/bin/openssl` (stock macOS LibreSSL) for the TLS server; the
   public-name checks print `note: ... skipped` when the Mac is offline (still pass).
2. `make -C runtime test test-services test-ipc-reference` → exit 0.
3. Headless: `NAPPLE_NET=host NAPPLE_TRACE_IPC=all runtime/build/napple-runtime <games>/0100000000010000/3ca12dfaaf9c82da --headless --seconds 60 --save-root <copy>`
   → log has `online: provider 'host (macOS network stack)' (built in) selected by NAPPLE_NET=host` and `net: host provider: nifm connected, ip <Mac IP>`,
   and `IRequest::Submit` is followed by an accepted request. Without the variable: no `online:` line, request rejected as before.
4. MK8D Online Play: `NAPPLE_NET=host NAPPLE_INPUT_SCRIPT=15:A,19:DOWN,20.5:DOWN,22:A,32:A NAPPLE_TRACE_IPC=all runtime/build/napple-runtime <games>/0100152000022000/fe941ed5ba14be5d --headless --seconds 45 --save-root <copy>` → today: `IRequest::Submit` accepted, `EnsureIdTokenCacheAsync` error, `error applet: type 0, jump-to-settings requested`, then `FAIL: guest Break` (rc 70). Passing (after the applet fix): the run ends at the time limit, rc 0.
5. Human (optional): two Macs on one LAN, a title's local wireless mode, `NAPPLE_NET=host` on both.

## Fix policy hints
- Safe: include / signature fixes, log wording, test timing margins (LDN waits up to 2 s; TLS server start up to 5 s).
- Design decisions: switching TLS to Network.framework would need the provider to own the TCP connection (the ssl service passes a guest socket);
  LDN address translation (169.254.x.y per node) instead of real LAN addresses would need bsd-layer rewriting.

## Needs from other areas
- **applet (applet.c, error applet 0xe):** MK8D's Online Play with `NAPPLE_NET=host` aborts after the error applet because `PopOutData` finds no storage
  (0x680) where this SDK path expects one. Needs the error applet's output on the console (likely a result storage, at least when jump-to-settings is
  requested). Repro in "How to test" 4. Offline this path is never reached (nifm refuses first), so it only matters with a network.
- **account (account.c):** `EnsureIdTokenCacheAsync` / `LoadIdTokenCache` are generic error stubs. A console without a linked Nintendo account answers through an
  IAsyncContext whose result is the "account required" error; worth checking against the SDK's expectations when the applet fix lands.
- Nice to have (ABI, later): a netmask in `OnlineLdnNetwork`, the station's user name / passphrase in `ldn_connect`.
