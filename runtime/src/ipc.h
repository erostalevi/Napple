/* In-process Horizon IPC: HIPC framing with CMIF payloads (including domains), service objects and the service registry.
 * The SDK's client code runs unchanged: requests are read from the calling thread's message buffer (TLR) and answered in
 * place. Message framing follows the public HIPC/CMIF description. Unknown services and commands never succeed silently:
 * they are logged with service, command id and request size, recorded in the summary and stop the runtime (set
 * NAPPLE_IPC_LENIENT=1 to get an error result instead and keep enumerating). */
#pragma once
#include "napple.h"

#define IPC_MAX_BUF 8
#define IPC_MAX_HANDLES 8
typedef struct {
    uint64_t addr, size;
    unsigned index;
} IpcBuf;

struct IpcObject;
struct IpcCall;
typedef void (*IpcCmdFn)(struct IpcObject *self, struct IpcCall *c);
enum { IPC_IMPL = 0, IPC_STUB = 1 }; /* IPC_STUB: a documented safe default (docs/architecture/runtime-services.md) */
typedef struct {
    unsigned cmd;
    const char *name;
    IpcCmdFn fn;
    int status;
} IpcCmd;

typedef struct IpcObject {
    const char *iface; /* interface / service name for logs */
    const IpcCmd *cmds;
    size_t ncmds;
    void *state;
    void (*destroy)(struct IpcObject *);
    unsigned refs;
} IpcObject;

typedef struct IpcCall {
    IpcObject *object;
    struct IpcSession *session;
    unsigned type, cmd;
    int has_pid;
    uint64_t pid;
    uint32_t copy[IPC_MAX_HANDLES], move[IPC_MAX_HANDLES];
    unsigned ncopy, nmove;
    IpcBuf x[IPC_MAX_BUF], a[IPC_MAX_BUF], b[IPC_MAX_BUF], w[IPC_MAX_BUF], c[IPC_MAX_BUF];
    unsigned nx, na, nb, nw, nc;
    const uint8_t *in;
    uint32_t in_size; /* command arguments after the CMIF header */
    IpcObject *in_objects[IPC_MAX_HANDLES];
    unsigned nin_objects; /* service objects passed as arguments (domain object ids or session copy handles), in order */
    /* response */
    uint32_t result;
    uint8_t out[0x100];
    uint32_t out_size;
    uint32_t out_copy[IPC_MAX_HANDLES], out_move[IPC_MAX_HANDLES];
    unsigned nout_copy, nout_move;
    IpcObject *out_objects[IPC_MAX_HANDLES];
    unsigned nout_objects;
    IpcBuf xout[IPC_MAX_BUF];
    unsigned nxout;
} IpcCall;

/* Service registry. `open` creates the root object of a new connection for a named service ("fsp-srv", "set:sys", ...). */
typedef IpcObject *(*IpcOpen)(void);
void ipc_register_service(const char *name, IpcOpen open);
IpcObject *ipc_object_new(const char *iface, const IpcCmd *cmds, size_t ncmds, void *state, void (*destroy)(IpcObject *));
#define IPC_OBJECT(iface, table, state, destroy) ipc_object_new(iface, table, sizeof(table) / sizeof((table)[0]), state, destroy)

/* Request helpers */
const void *ipc_in(const IpcCall *c, uint32_t off, uint32_t n); /* argument bytes; fails the runtime when out of range */
#define IPC_IN(c, T, off) (*(const T *)ipc_in((c), (off), sizeof(T)))
/* nth input buffer: map-alias "A" buffers first, then pointer "X" buffers. Returns NULL when absent. */
const IpcBuf *ipc_in_buf(const IpcCall *c, unsigned n);
/* nth output buffer: map-alias "B" buffers first, then receive-list "C" buffers. */
const IpcBuf *ipc_out_buf(const IpcCall *c, unsigned n);
/* Writes `n` bytes to output buffer `b`: B buffers are written in place; C buffers are filled and a pointer descriptor with the
 * written size is added to the response. Returns the bytes written (min of n and the buffer size). */
uint64_t ipc_write_buf(IpcCall *c, const IpcBuf *b, const void *data, uint64_t n);
void *ipc_buf_ptr(const IpcBuf *b); /* validated host pointer to a guest buffer */

/* Response helpers */
void ipc_out(IpcCall *c, const void *p, uint32_t n); /* append raw out data, aligned to min(n, 8) */
#define IPC_OUT(c, v) ipc_out((c), &(v), sizeof(v))
void ipc_out_zero(IpcCall *c, uint32_t n);
void ipc_out_copy_handle(IpcCall *c, uint32_t handle); /* returned as a copy handle (events, shared memory) */
void ipc_out_move_handle(IpcCall *c, uint32_t handle);
void ipc_out_object(IpcCall *c, IpcObject *obj); /* a new service object: object id in a domain, else a session handle */
void ipc_fail(IpcCall *c, uint32_t result);

/* Kernel objects owned by the IPC layer */
typedef struct {
    uint64_t addr, size;
    uint32_t perm;
} IpcTransferMemory;
typedef struct {
    uint64_t addr, size;
    int remapped;
} IpcShmemMap;
typedef struct {
    void *host;
    uint64_t size;
    uint32_t refs;
    int owned;
    IpcShmemMap maps[4];
    unsigned nmaps;
} IpcSharedMemory;
void ipc_shared_memory_flush(IpcSharedMemory *m);      /* propagate host writes to guest mappings that are copies */
IpcSharedMemory *ipc_shared_memory_new(uint64_t size); /* zeroed host pages; handle via handle_new(OBJ_SHMEM, ...) */
uint32_t ipc_shared_memory_handle(IpcSharedMemory *m);

/* Common results */
#define IPC_R_UNKNOWN_COMMAND 0xf601u /* sf: unknown command */
#define IPC_R_NOT_FOUND(module, desc) ((module) | ((desc) << 9))

/* Per-thread dispatch entry points used by svc handlers (ipc.c) */
void ipc_service_names(char names[][12], unsigned max, unsigned *count);

/* Services register themselves from services_init() (services/services.c). */
void services_init(void);
