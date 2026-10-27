# Real-writer fixtures

Everything here is SYNTHETIC: random throwaway keys (`nsz/synthetic_keys.txt`, a few forged to the
CRC32 the `nsz` tool insists on), NCAs generated from a seed, a throwaway 2048-bit RSA test key
shared with `src/infrastructure/ticket/ticket_testkey.rs`. No console keys, firmware or game data.
Files end in `.bin` / `.txt` so the repo's `*.nsz`, `*.nca`, `*.keys` ignore rules do not apply.

What makes them useful: `*.nsz.bin` / `c00.xcz.bin` were **written by the public `nsz` tool**
(5.0.0, `pip install nsz`), not by this repo's own builders, so they check the readers against a
second implementation.

| file | what |
| --- | --- |
| `c00_solid.nsz.bin`, `c00_block.nsz.bin` | NCA whose first section starts at 0xC00 (inside nsz's raw 0x4000 block): solid zstd stream / block form (2^14) |
| `g4000_solid.nsz.bin`, `g4000_block.nsz.bin` | same, first section padded out to 0x4000 (block 2^15) |
| `c00.xcz.bin` | an XCI (header at 0x100, root HFS0 at 0xF000) compressed by nsz |
| `rights_solid.nsz.bin` | rights-managed NCA; nsz got the title key from a common-form ticket, then the ticket member was swapped for a personalized (RSA-OAEP) one |
| `c00.nca.bin`, `g4000.nca.bin` | the uncompressed NCAs: the byte-exact reference |
| `nca2.nca.bin` | NCA2 built by `gen_nsz.py`'s own (Python) encoder; nsz leaves NCA2 uncompressed ("not packed": it reads NCA2 FS headers as NCA3 ones) |
| `rights_enc_title_key.hex` | the titlekek-encrypted title key the personalized ticket carries |

Regenerate (only needed if the generator changes; output is deterministic except the RSA-OAEP
padding of the personalized ticket):

    python3 -m venv /tmp/nszvenv && /tmp/nszvenv/bin/pip install nsz==5.0.0
    /tmp/nszvenv/bin/python tests/fixtures/gen_nsz.py tests/fixtures/nsz

What the real writer taught us (all fixed in this branch): nsz writes the XCI root partition
header size in the XCI header (not the partition size), puts the root HFS0 offset at 0x130 (we read
0x138), and rounds the declared partition size past the end of the file. The synthetic XCI builders
in `tests/ingest.rs` had mirrored the wrong offsets, so they could not have found this.
