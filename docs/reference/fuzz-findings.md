# Fuzz findings

Every crash, panic, hang, overflow or unbounded allocation found by the robustness work (2026-10-27, branch `claude/fuzz-hardening-2026-10-27`), with the fix and its regression test. How to run the fuzzers: [fuzzing guide](../guides/fuzzing.md). "Found by": **fuzz** = cargo-fuzz or a C fuzz driver, **mutation** = the seeded-mutation test with its allocation guard, **audit** = read while writing the targets, then reproduced with a hostile input.

## Rust

| Target | Input class | Found by | Fix | Regression test |
|---|---|---|---|---|
| `Nso::parse` (`napple-core/.../nso.rs`) | Compressed segment whose header claims 512 MiB of output for a few bytes: `lz4_flex` reserved `dsz` up front (x3 segments) | mutation | Refuse `dsz > csz * 255 + 64` for LZ4 segments (LZ4 cannot expand more) | `mutation.rs::nso_huge_decompressed_size_is_not_preallocated` |
| `NczSource` block mode (`ncz.rs`) | Block header with exponent 32 and section sizes of 4 GiB: `zstd::bulk::decompress(raw, dlen)` reserved 4 GiB for a tiny block | mutation | Stream-decode with `Decoder::take(dlen + 1)` and a capacity bounded by the raw size | `mutation.rs::ncz_block_with_huge_declared_size_is_not_preallocated` |
| `ZipArchive::open_entry` (`napple-build/.../zipread.rs`, firmware zip) | Deflate entry declaring 1 GiB uncompressed size: `Vec::with_capacity(usize)` | mutation | Capacity `min(usize, csize * 1032)` (deflate's maximum ratio) | `napple-build/tests/mutation.rs::zip_deflate_with_huge_declared_size_is_not_preallocated` |
| BKTR `IndirectSource::read_at/map`, `PatchedRomfs::delta_entries` (`bktr.rs`) | Relocation `physical_off` near `u64::MAX`: `physical_off + (pos - virtual_off)` overflows (panic in debug, wrong read in release) | audit | `checked_add`, error / `false` on overflow | `bktr.rs::tests::hostile_physical_offsets_do_not_overflow` |
| `relocs::decode_relr` / `collect`, `ModuleImage::function_starts` (`napple-rewrite`) | RELR address `0xFFFF_FFFF_FFFF_FFF8`, bitmap cursor and DT_HASH/RELA/JMPREL/SYMTAB values near the top of the address space; `eh_frame_hdr` encoded value near `i64::MAX`: unchecked `+` | audit, reproduced | `checked_add`/`saturating_add` everywhere an untrusted value is added; `rd32/rd64` saturate their range end | `napple-rewrite/tests/mutation.rs::hostile_dynamic_values_do_not_overflow` |
| `Nca::open`, NCA0 (`nca.rs::nca0_xts`) | NCA0 section starting inside the header (`start < 0x400`): `abs - NCA0_SECTOR_BASE` underflows | fuzz (`nca`) | Refuse an NCA0 section that starts below `0x400` | `napple-core/tests/mutation.rs::regress_nca0_section_below_header` (`tests/regress/nca0_section_below_header.bin`) |
| `KeySet::parse` (`domain/keys.rs`, derivation of missing keys) | `master_key_00` (or another source key) of 32 bytes: `GenericArray::from_slice(key)` asserts on a non-16-byte AES key | fuzz (`keyfile`) | `dec` derives nothing for a key that is not 16 bytes (the existing 16/32 length filter then drops it) | `keys.rs::tests::odd_length_master_keys_derive_nothing_instead_of_panicking` |

No finding (clean in the mutation tests and the fuzz budget): PFS0/HFS0/XCI, NCA body and IVFC/romfs tables, NCZ stream mode, CNMT, NACP, tickets, NPDM, Mod IPS/pchtxt/cheats/manifest/config/`.napmod`, NRO/NTB1/BFTTF, `Container::load`, `ingest`. Accepted by design: `ips::apply` on a 5-byte IPS32 record may grow the target up to the documented 1 GiB cap.

## C runtime

| # | Target | Input class | Fix (file:function) | Regression test |
|---|---|---|---|---|
| 1 | jpeg | 4-component frame header: stack write out of bounds | `services/jpeg_core.c:napjpeg_info` | `regress_jpeg.c` |
| 2 | jpeg | over-subscribed Huffman table: heap write out of bounds | `jpeg_core.c:huff_build` | `regress_jpeg.c` |
| 3 | movie_audio | Vorbis comment header with a `0xfffffff0` vendor length: 4 GiB allocation and loop | `services/movie_audio_core.c:add_header_` | `regress_movie_audio.c` |
| 4 | movie_audio | codebook where `pow()` gives inf, cast to int | `third_party/stb_vorbis.c:lookup1_values` | `regress_movie_audio.c` |
| 5 | movie_audio | stream that decodes to NaN, cast to int16 | `movie_audio_core.c:vorbispcm_packet` | `regress_movie_audio.c` |
| 6 | movie_audio | codebook entries x dimensions asks for hundreds of MB (capped at 4M values per codebook) | `stb_vorbis.c:start_decoder` | `regress_movie_audio.c` |
| 7 | amiibo | stray `}` in an AmiiboAPI array: infinite loop | `services/amiibo_core.c:ami_jskip` | `regress_amiibo.c` |
| 8 | amiibo | `key_retail.bin` with magic size above 16: stack overflow | `amiibo_core.c:amiibo_keys_load`, `amiibo_crypto_core.c:prepare_seed` | `regress_amiibo.c` |
| 9 | json | literals and numbers read past the end of text that is not NUL-terminated | `src/json.c:parse_value` | `regress_json.c` |
| 10 | json | every string kept a buffer sized to the rest of the text (80 KB of short strings held 80 MB) | `json.c:parse_string` | `regress_json.c` |
| 11 | romfs_map | more than 64 files: NULL descriptor table walked on close | `services/romfs_map.c:romfs_map_close` | `regress_romfs.c` |
| 12 | romfs_map | extent count the file cannot hold: multi-GB allocation | `romfs_map.c:romfs_map_open` | `regress_romfs.c` |
| 13 | codec_headers | Exp-Golomb and QP signed overflow | `services/codec_headers_core.c:br_uei,br_sei` | `regress_codec_headers.c` |
| 14 | codec_headers | PPS builder overflow on guest QP values | `codec_headers_core.c:bw_se,h264_build_pps` | `regress_codec_headers.c` |
| 15 | ipc | descriptor, handle and data counts read past the 0x100-byte message buffer | `src/ipc.c:parse` | `regress_ipc.c` |
| 16 | ipc | no room for the domain/CMIF header; object-id offset past the message | `ipc.c:dispatch` | `regress_ipc.c` |
| 17 | ipc | misaligned `IPC_IN` load on TIPC arguments, wrapping `off + n` | `ipc.h:IPC_IN`, `ipc.c:ipc_in` | `regress_ipc.c` |
| 18 | ipc (bsd) | Select timeout overflow on guest int64 values | `services/offline.c` | `regress_ipc.c` |
| 19 | ipc (time) | guest time snapshot arithmetic overflow | `services/time_core.c:time_snapshot_span`, `services/time.c` | `regress_ipc.c` (span only; `st_user_clock_difference`, `st_monotonic_base` have no embedded test) |

Not present in the C runtime, so nothing to fuzz: WebArgs TLV and IPS patching (IPS lives in Rust: `napple-mods`). `libapplet` (library-applet storages) is the closest TLV-like parser. The `ipc` fixes came from the first fuzz rounds and are a start, not a complete sweep of the services (the target reached about 2000 edges); `hid_core` and `audio` decoders are not covered, `fs.c` command handlers only through `ipc`.

### Budgets run (mutation driver, ASan + UBSan, 300 s per target after the last fix to that target)

| Target | Executions | Result |
|---|---|---|
| codec_headers | 6.3M | clean |
| amiibo | 5.9M | clean |
| jpeg | 526K | clean |
| json | 14.3M | clean |
| romfs_map | 5.5M | clean |
| movie_audio | 144K | clean |
| libapplet | 38M | clean |
| romfs_overlay | 5.8M | clean |
| nro | 6.2M | clean |
| ipc | 389K | clean (plus 4 seeds x 60 s) |

The Rust budget (24 cargo-fuzz targets, 301 s each, about 183M executions) ended with zero crashes after the fixes above. The libFuzzer entries of the C targets compile in the Makefile but were not run (all C runs used the mutation driver).
