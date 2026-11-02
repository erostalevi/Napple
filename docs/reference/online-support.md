# Online support

Per-game status of online and local multiplayer on Napple. **Generated** by `tools/support/gen_support.py` (module `tools/support/gen_online.py`) from `compat/online.json`; do not edit by hand (edit the JSON, run `python3 -I tools/support/gen_support.py`; `--check` fails when this page, the README block or `site/online.json` is stale).

**Online play is between Napple players only**, through the Napple-run servers at `pretend.napple.lat` (napple-online for accounts and the game servers, napple-room for LAN Play and local wireless rooms). Napple never connects to Nintendo's servers, and Napple players cannot meet players on a console. See [network providers](../guides/network-providers.md) and [deploying the servers](../guides/deploy-servers.md).

Last checked: 2026-11-02. Statuses: `works`, `partial`, `broken`, `untested`; `-` (unsupported) means the game has no such mode.

## By title

| Title | Id | Backend | LAN Play | Local wireless | Online matchmaking | Online race / match | Friends | Rankings | DataStore |
|---|---|---|:-:|:-:|:-:|:-:|:-:|:-:|:-:|
| Animal Crossing: New Horizons | `01006F8002326000` | nex | untested | untested | untested | untested | untested | - | untested |
| Link's Awakening | `01006BB00C6F0000` | none | - | - | - | - | - | - | - |
| Mario Kart 8 Deluxe | `0100152000022000` | nex | works | broken | works | partial | partial | untested | untested |
| Super Mario Odyssey | `0100000000010000` | nex | - | - | - | - | - | untested | untested |

## Notes per title

### Animal Crossing: New Horizons (`01006F8002326000`)

Backend `nex`: NEX, the game-server protocol of most first-party games; served by napple-online.

| Mode | Status | Note | Checked |
|---|:-:|---|---|
| LAN Play | untested | Local play is wireless; a separate LAN mode is not confirmed. | 2026-11-02 |
| Local wireless | untested | Local play (island visits over LDN) through a room; not tried yet. | 2026-11-02 |
| Online matchmaking | untested | Online island visits (Dodo Code and friends' islands); not tried yet. | 2026-11-02 |
| Online race / match | untested | The online island visit session itself; not tried yet. | 2026-11-02 |
| Friends | untested | Best friends and friend island visits; not tried yet. | 2026-11-02 |
| Rankings | unsupported | The game has no rankings. | 2026-11-02 |
| DataStore | untested | Dream Addresses and shared designs; not tried yet. | 2026-11-02 |

### Link's Awakening (`01006BB00C6F0000`)

Backend `none`: no online features.

| Mode | Status | Note | Checked |
|---|:-:|---|---|
| LAN Play | unsupported | The game has no LAN mode. | 2026-11-02 |
| Local wireless | unsupported | The game has no local wireless mode. | 2026-11-02 |
| Online matchmaking | unsupported | The game has no online play. | 2026-11-02 |
| Online race / match | unsupported | The game has no online play. | 2026-11-02 |
| Friends | unsupported | The game has no online features. | 2026-11-02 |
| Rankings | unsupported | The game has no online features. | 2026-11-02 |
| DataStore | unsupported | The game has no online features. | 2026-11-02 |

### Mario Kart 8 Deluxe (`0100152000022000`)

Backend `nex`: NEX, the game-server protocol of most first-party games; served by napple-online.

| Mode | Status | Note | Checked |
|---|:-:|---|---|
| LAN Play | works | LAN Play through a local napple-room: two instances verified in the same room (one creates it, the other lists and joins it; both reach its character select). | 2026-11-02 |
| Local wireless | broken | Local wireless (LDN) through a room: the station crashes right after joining. | 2026-11-02 |
| Online matchmaking | works | Through a local napple-online: login, the online menu, and two instances matched into one gathering (verified). | 2026-11-02 |
| Online race / match | partial | In a two-instance run through a local napple-online the race starts; it has not been played through to the end. | 2026-11-02 |
| Friends | partial | A friend shows as joinable; the friend list on the Friends menu is still empty. | 2026-11-02 |
| Rankings | untested | Time trial rankings; not tried yet. | 2026-11-02 |
| DataStore | untested | Ghost upload and download; not tried yet. | 2026-11-02 |

### Super Mario Odyssey (`0100000000010000`)

Backend `nex`: NEX, the game-server protocol of most first-party games; served by napple-online.

| Mode | Status | Note | Checked |
|---|:-:|---|---|
| LAN Play | unsupported | The game has no LAN mode. | 2026-11-02 |
| Local wireless | unsupported | The game has no local wireless mode (two-player is on one console). | 2026-11-02 |
| Online matchmaking | unsupported | The game has no online multiplayer. | 2026-11-02 |
| Online race / match | unsupported | The game has no online multiplayer. | 2026-11-02 |
| Friends | unsupported | The game has no friend features. | 2026-11-02 |
| Rankings | untested | Luigi's Balloon World scores; not tried yet. | 2026-11-02 |
| DataStore | untested | Luigi's Balloon World balloons (upload and find); not tried yet. | 2026-11-02 |

## What the modes mean

| Mode | Key | Meaning |
|---|---|---|
| LAN Play | `lan_play` | The game's LAN mode, its broadcast traffic carried between players by a Napple room (napple-room). |
| Local wireless | `local_wireless` | The game's local wireless play (LDN), emulated over the network through a Napple room. |
| Online matchmaking | `online_matchmaking` | Logging in to napple-online, reaching the game's online menu and being matched with other players into one session (gathering). |
| Online race / match | `online_race_or_match` | Actually playing the matched session together: the race, battle or visit, with players talking peer to peer. |
| Friends | `friends` | Friend list, presence and joining friends, between Napple accounts. |
| Rankings | `rankings` | Leaderboards uploaded to and read from the Napple server. |
| DataStore | `datastore` | Shared user content stored on the Napple server: ghosts, designs, levels and similar. |

## Statuses and backends

| Status | Meaning |
|---|---|
| `works` | verified working on Napple |
| `partial` | parts work; the note says what is missing |
| `broken` | tried and fails; the note says how |
| `untested` | the game has this mode, nobody has tried it on Napple yet |
| `unsupported` | the game has no such mode |

| Backend | Meaning |
|---|---|
| `nex` | NEX, the game-server protocol of most first-party games; served by napple-online |
| `npln` | NPLN, the newer game-server protocol |
| `own-servers` | the publisher's own servers, not run by Napple |
| `none` | no online features |

To update a status, edit `compat/online.json` (the status, a one-line note, the date checked) and run the generator. See also the [support matrix](support-matrix.md) for the offline state of each title.
