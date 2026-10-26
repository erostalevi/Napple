/* irs (nn::irsensor): the Joy-Con IR camera. There is no camera: activation and the shared-memory handle work (zeroed 0x8000-byte
 * block, so readers see "no data"), camera handles are valid, and every processor start or state query reports the IR sensor as not
 * connected. Stop calls succeed. Command ids and result descriptions are from the author's memory of the public documentation
 * (unverified). */
#include "svc_common.h"
#include "irs_core.h"
#include "hz_result.h"

static IpcSharedMemory *shm;
static void irs_get_shmem(IpcObject *s, IpcCall *c) {
    (void)s;
    if (!shm) shm = ipc_shared_memory_new(0x8000);
    ipc_out_copy_handle(c, ipc_shared_memory_handle(shm));
}
static void irs_get_camera_handle(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t h;
    if (irs_camera_handle(IPC_IN(c, uint32_t, 0), &h)) {
        c->result = HZ_RESULT(HZ_MOD_IRSENSOR, 1);
        return;
    } /* [unverified] invalid npad */
    IPC_OUT(c, h);
}
static void irs_not_connected(IpcObject *s, IpcCall *c) {
    (void)s;
    c->result = HZ_R_IRSENSOR_NOT_CONNECTED;
}
static const IpcCmd irs_cmds[] = {
    {302, "ActivateIrsensor", svc_noop, IPC_STUB},
    {303, "DeactivateIrsensor", svc_noop, IPC_STUB},
    {304, "GetIrsensorSharedMemoryHandle", irs_get_shmem, IPC_STUB},
    {305, "StopImageProcessor", svc_noop, IPC_STUB},
    {306, "RunMomentProcessor", irs_not_connected, IPC_STUB},
    {307, "RunClusteringProcessor", irs_not_connected, IPC_STUB},
    {308, "RunImageTransferProcessor", irs_not_connected, IPC_STUB},
    {309, "GetImageTransferProcessorState", irs_not_connected, IPC_STUB},
    {310, "RunTeraPluginProcessor", irs_not_connected, IPC_STUB},
    {311, "GetNpadIrCameraHandle", irs_get_camera_handle, IPC_IMPL},
    {312, "RunDpdProcessor", irs_not_connected, IPC_STUB},
    {313, "SuspendImageProcessor", svc_noop, IPC_STUB},
    {314, "CheckFirmwareVersion", irs_not_connected, IPC_STUB},
    {315, "SetFunctionLevel", svc_noop, IPC_STUB},
    {316, "RunImageTransferExProcessor", irs_not_connected, IPC_STUB},
    {317, "RunIrLedProcessor", irs_not_connected, IPC_STUB},
    {318, "StopImageProcessorAsync", svc_noop, IPC_STUB},
    {319, "ActivateIrsensorWithFunctionLevel", svc_noop, IPC_STUB},
};
static IpcObject *irs_open(void) { return IPC_OBJECT("irs", irs_cmds, NULL, NULL); }
void service_register_irs(void) { ipc_register_service("irs", irs_open); }
