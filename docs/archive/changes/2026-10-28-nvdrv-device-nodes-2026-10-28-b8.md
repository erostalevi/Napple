# nvdrv device nodes (b8): state-tracked nvhost-gpu / as-gpu / ctrl-gpu

- New `runtime/src/services/nvdrv_nodes_core.{c,h}`: portable state machines for the channel (`'H'`), address space (`'A'`) and ctrl (`'G'`) ioctls. Layouts are proposed (public nvgpu uapi / switchbrew from memory), unverified.
- `nvdrv.c`: three new `DevKind`s; the old `ctrl_gpu_ioctl` (GET_CHARACTERISTICS) moved into the core with identical values; the channel's fence is a dynamic syncpoint owned by the fd; closing an address space unbinds its channels; BIND_CHANNEL resolves the fd. nvdec/vic/nvjpg/msenc, nvmap, nvhost-ctrl and nvdisp paths untouched. Other known nodes (dbg-gpu, prof-gpu, tsec, ...) stay open-only.
- `nvdisp-disp0` already had its 5-ioctl surface and tests (media-nvdrv); unchanged.
- Tests: `runtime/tests/test_svc_nvdrv_nodes.c`, 105 checks (every ioctl: request, reply, state after, error paths, ordering, close). `nvdrv_media` 128 checks still pass.
- Status: tested on Linux, unbuilt on macOS; partial by design (no channel executes while NVN is HLE).
