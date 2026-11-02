# IPC tail: fs 84, miiimg 18-20, ldn 503, ncm ContentInfo type (round 11)

## Rows converted (IPC_STUB -> IPC_IMPL)
- **fsp-srv 84 ListAccessibleSaveDataOwnerId** (`fs.c`): input `{s32 start, s32 reserved, u64 program id}`, output `s32 count` + `u64` owner ids in buffer 0. Answered from the
  save index (`meta_walk`): the distinct non-zero owner ids of the saves of that program (program id 0 = every save), from `start`, cut to the buffer. The console reads the
  FsAccessControl owner list of the npdm, which `npdm.c` does not parse; the save index is the host's equivalent.
- **miiimg 18 AddImageForMigration / 19 ClearHeaderForMigration / 20 SaveHeaderForMigration** (`mii_img.c`, `mii_img_core.c`): a staging table (`MiiImgDb.m`) keyed by the same unique id as
  the database. 18 stages (arguments like 16), 19 empties the staging table, 20 merges it into the database (all-or-nothing: refused with the full result when the new ids do not fit) and
  empties it. Memory only, like the database; ForceInitialize / DeleteFile drop it.
- **ldn 503 RecvActionFrame** (`ldn.c`, `ldn_core.[ch]`): a bounded FIFO (16 frames x 0x400 bytes) per local-communication object, `ldn_push_action_frame()` as the entry point for the
  provider/room side (queues on every object that enabled action frames for that LocalCommunicationId, 0 = all; wakes a blocked receive). 503 returns `{src, bssid, band, channel, size}`
  (0x14 bytes, host layout) and the frame in buffer 0. Empty queue: `HZ_R_LDN_NETWORK_NOT_AVAILABLE` (as before); a blocking call waits at most 200 ms first.

## Kept as documented default
- **btm 2 GetBleScanFilterParameter2**: the GattAttributeUuid values per id are not public; unchanged.

## Bug fixed
- `ncm_core.c`: the ContentInfo content type is at byte 0x16 (libnx NcmContentInfo: ContentId 0x10, size[6], type 0x16, id offset 0x17); it was read from 0x17.

## Not done / notes
- Nothing in `runtime/src/net/` calls `ldn_push_action_frame` yet (out of this session's ownership); a send hook for 502 is also still missing.
