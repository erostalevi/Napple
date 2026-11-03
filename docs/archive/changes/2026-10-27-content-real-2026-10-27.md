# Content formats against real writers (2026-10-27, branch claude/content-real-2026-10-27)

Cloud (Linux) session. Moves NSZ/XCZ, NCA2/NCA0 and personalized tickets from "only our own synthetic builders agree with our readers" to "checked against another implementation's bytes". Status words: **tested** = unit/integration test green on Linux; nothing here is **verified** on a Mac or on retail files.

## What was done
| Item | Result |
| --- | --- |
| NSZ / XCZ from the public `nsz` tool | `tests/fixtures/gen_nsz.py` drives nsz 5.0.0 (isolated venv) on synthetic NCAs and commits the output: solid and block (2^14, 2^15) NSZ, first section at 0xC00 (the straddling case) and at 0x4000, an XCZ. `tests/real_writers.rs`: byte-exact round trips, assertions that the layouts really are the ones the reader special-cases, ingest from NSZ/XCZ equals ingest of the plain NCA. **Tested.** |
| XCI header was wrong (found by the real writer) | The root HFS0 offset is at 0x130 (we read 0x138); 0x138 is the HFS0 *header* size, not the partition size; "full" dumps carry a 0x1000 key area before `HEAD`; nsz's XCZ rounds the declared `secure` partition past the end of the file. `xci.rs` fixed (zero-padded root view for the rounding, bounded at 64 KiB). The synthetic XCI builders in `tests/ingest.rs` had mirrored the wrong offsets and were corrected. |
| NCA0 | The earlier model (RSA key area of 64 bytes, AES-CTR sections, FS headers inside the main header) was a guess. Rewritten after hactool's `nca.c`: sections are AES-XTS with the 32-byte key, sector = (file offset - 0x400) / 0x200; the FS header is the first 0x200 bytes of each section (XTS too); key area: beta files RSA-OAEP-SHA256 (user `nca0_key_area_rsa`, plaintext >= 0x20 bytes, first 0x20 used), retail files in the clear (recognised by hactool's SHA-256 constant), otherwise AES-ECB with `key_area_key_<kind>_<gen>`. The pre-1.0 NCA0 RomFS layout is reported as unsupported (`romfs_note`), not misparsed. New tests: three key-area schemes, XTS sector origin, NCA0 ingest (PFS0 + RomFS note). |
| NCA2 | Per-FS-header XTS sector 0 confirmed against hactool; an independently written (Python) NCA2 reads. nsz itself leaves NCA2 uncompressed (it reads NCA2 FS headers as NCA3), recorded in the fixtures README. |
| Personalized tickets through `Container::load` | `Container::load` dropped every non-common ticket. New `Container::load_with_keys` and `Containers::with_keys` resolve RSA-OAEP tickets with the user's `eticket_rsa_keypair` (layout per public docs: D, N, E big-endian). `legacy --dump-romfs` too. Tests: NSZ whose ticket was swapped for a personalized one resolves end to end; no key -> skipped and the NCA names the missing ticket; wrong key -> integrity error, not silence. |

## Decisions
- **Decision**: the nsz tool cannot read an RSA ticket, so it was given the title key in a common-form ticket and the shipped `.tik` member was swapped for the personalized one afterwards (the NCZ bytes are untouched). This is also what a user does with a decrypted title key.
- **Decision**: fixtures are committed as `*.bin` / `*.txt` (the repo ignores `*.nsz`, `*.nca`, `*.keys`). The keys are random throwaway values (a few are forged to nsz's CRC32 check); nothing derives from a console.
- **Decision**: NCA0 tests were replaced, not weakened: the old ones asserted the old (unverified) layout. The behaviour now follows hactool's source (read, not executed: building the cloned hactool was refused by the sandbox in this session).
- Not done: NCA0 RomFS (needs a reader for the old u32 RomFS header in `romfs.rs`, not this area) and BKTR in NCZ.

## Files
`crates/napple-core/src/infrastructure/{nca,xci}.rs`, `crates/napple-core/tests/{ingest.rs,real_writers.rs,nca0_fixture/mod.rs,fixtures/**}`, `crates/napple-build/src/{container,legacy}.rs`, `crates/napple-build/tests/container_real.rs`.
