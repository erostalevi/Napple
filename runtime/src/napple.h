/* Napple runtime: shared declarations.
 *
 * The runtime hosts the guest's own AArch64 code (already rewritten ahead of time: x18, TLS registers, svc and counter
 * reads become jumps into a per-module patch area that ends in `runtime_gate`) and supplies what Horizon OS would:
 * the kernel (svc), the IPC services and, behind gfx.h, the graphics driver. Nothing in here is specific to one game:
 * everything per game comes from the data directory (docs/runtime-modules.md). */
#pragma once
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#define PAGE_4K 0x1000ULL
#define PAGE_16K 0x4000ULL

/* ---- logging and the structured trace (log.c) ---------------------------------------------------------------- */
void log_init(void);
void filelog_start(void);                 /* unattended sessions: stderr -> ~/Library/Logs/Napple/<titleid>.log (filelog.c) */
void filelog_set_title(const char *title_id);
void filelog_drain(void);                 /* call before an abrupt _exit so the last lines reach the file */
void log_line(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#define LOG(tag, ...) log_line(tag, __VA_ARGS__)
#define LOGV(tag, ...) do { if (log_verbose) log_line(tag, __VA_ARGS__); } while (0)
extern int log_verbose;
/* Fatal runtime error: logs, writes the summary, exits with status 70. */
_Noreturn void runtime_fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* One JSON object per line in the trace file (NAPPLE_TRACE / --trace): {"t":sec,"k":kind,...}. `fields` is the
 * already formatted tail, e.g. "\"svc\":33,\"x0\":5". */
void trace_event(const char *kind, const char *fields_fmt, ...) __attribute__((format(printf, 2, 3)));
int trace_enabled(void);
/* Compatibility bookkeeping, written to the summary file at exit (summary.c). */
void summary_svc_unsupported(unsigned num, const char *name);
void summary_ipc(const char *service, unsigned cmd, int status /* 0 implemented, 1 stub, 2 missing */, unsigned request_bytes);
void summary_nvn_missing(const char *name);
void summary_note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void summary_set_title(const char *title_id, const char *build_id);
void summary_write(const char *status, const char *reason);
void summary_crash(int signal, uint64_t guest_pc, const char *note);
void summary_stage(const char *stage);   /* last boot milestone reached: "loaded", "start", "nvn", ... */
extern const char *summary_path;

/* ---- modules (module.c) --------------------------------------------------------------------------------------- */
typedef struct { uint32_t name; uint8_t binding, type, defined, pad; uint64_t value, size; } Sym;
typedef struct { uint64_t where; int64_t addend; uint32_t kind, symbol; } Reloc;
typedef struct { uint64_t ret, original; uint32_t timer, arg; } Gate;
typedef struct Module {
    char name[16];
    uint8_t build_id[32];
    uint64_t file_size, text_end, ro_off, data_off, patch_off, key_off;
    uint64_t dt_init, dt_fini, init_array, init_array_size, fini_array, fini_array_size;
    unsigned nsym, nrel, ngate;
    const Sym *syms; const Reloc *rels; const Gate *gates; const char *strtab;
    uint8_t *base; size_t size;
    unsigned *hash, hash_mask;
    unsigned relative, symbolic, cross, weak;
    void *tables_blob;
} Module;
extern Module *g_modules; extern unsigned g_nmodules;
/* Loads every module of the data directory's manifest, relocates, seals and checks them. */
void modules_load(const char *dir);
const char *module_sym_name(const Module *m, unsigned i);
void *module_find_symbol(const char *name);              /* first exporting module in load order, or NULL */
const Module *module_by_name(const char *name);
const Module *module_for_address(uint64_t a);
void module_describe(uint64_t a, char *out, size_t n);   /* "main+0x1234" or the nearest exported symbol */
int module_pc_is_guest_code(uint64_t pc);                /* original text, not the patch area (signal handlers) */
uint64_t module_image_bytes(void);
/* Guest permission calls over module data pages (svc 0x02 / 0x03 / 0x06). */
uint64_t module_set_permission(uint64_t address, uint64_t size, unsigned permission);
uint64_t module_set_attribute(uint64_t address, uint64_t size, unsigned mask, unsigned attribute);
int module_query(uint64_t address, uint64_t *base, uint64_t *size, unsigned *state, unsigned *permission, unsigned *attribute);
/* Host replacements: a name lookup that overrides a guest import (NVN, nn::vi, ...). Registered tables are searched
 * in order of registration; the first match wins. */
typedef struct { const char *name; void *fn; } HostSymbol;
void host_symbols_register(const HostSymbol *table, size_t count);
void *host_symbol_lookup(const char *name);

/* ---- runtime entry (gate.S, thread.c) ------------------------------------------------------------------------- */
extern pthread_key_t runtime_keys[8];     /* Darwin TSD keys: [0] guest x18 slot, [1] TLS, [2] TLR, [3] gate, [4] host stack */
void runtime_gate(void);
void runtime_dispatch(uint64_t ret, uint64_t *regs);
void runtime_thread_init(void);           /* per guest thread: TSD slots, signal stack, TLR */
uint64_t runtime_now_ns(void);
uint64_t runtime_guest_ticks(void);       /* 19.2 MHz Horizon system tick */
void *runtime_hle_wrap_named(void *fn, const char *name);
void runtime_hle_stack_init(uint64_t tsd_offset);
void runtime_hle_stack_set(uintptr_t top);
void runtime_hle_trace_set(int mode);
void runtime_hle_describe(unsigned index, char *out, size_t cap);
void runtime_trace_guest_n(uint64_t fp, uint64_t lr, unsigned max);
void runtime_trace_guest(uint64_t fp, uint64_t lr);
void runtime_peek(const char *what);   /* NAPPLE_PEEK="main+0x25687f8": logs the 8 bytes there (debug aid) */   /* guest frame-pointer chain to the log */
void signals_install(void);

/* ---- kernel (kernel.c, threads.c, memory.c, ipc.c) ------------------------------------------------------------ */
typedef uint32_t KResult;
#define R_SUCCESS 0u
#define R_NOT_IMPLEMENTED 0x4201u
#define R_TIMED_OUT 0xEA01u
#define R_CANCELLED 0xEC01u
#define R_INVALID_HANDLE 0xE401u
#define R_INVALID_STATE 0xFA01u
#define R_INVALID_ADDRESS 0xCC01u
#define R_INVALID_SIZE 0xCA01u
#define R_INVALID_CURRENT_MEMORY 0xD401u
#define R_INVALID_MEMORY_PERMISSIONS 0xD801u
#define R_INVALID_COMBINATION 0xE801u
#define R_INVALID_ENUM 0xF001u
#define R_INVALID_POINTER 0xE601u
#define R_LIMIT_REACHED 0x10801u
#define R_OUT_OF_RESOURCE 0xCE01u
#define R_SESSION_CLOSED 0xF601u
#define R_INVALID_THREAD_PRIORITY 0xE001u
#define R_INVALID_PROCESSOR_ID 0xE201u

typedef enum { OBJ_FREE, OBJ_THREAD, OBJ_EVENT_R, OBJ_EVENT_W, OBJ_SESSION, OBJ_TMEM, OBJ_SHMEM, OBJ_PROCESS, OBJ_CODEMEM, OBJ_PORT, OBJ_MISC } ObjType;
uint32_t handle_new(ObjType type, void *obj);
void *handle_get(uint32_t h, ObjType type);          /* NULL when not that type */
ObjType handle_type(uint32_t h);
void *handle_close(uint32_t h, ObjType *type);       /* removes the handle; returns the object */
#define HANDLE_CURRENT_THREAD 0xFFFF8000u
#define HANDLE_CURRENT_PROCESS 0xFFFF8001u

/* svc handlers return 1 when they handled the call. regs[0..8] are x0..x8 in and out. */
typedef void (*SvcHandler)(uint64_t *regs);
const char *svc_name(unsigned num);
void svc_dispatch(unsigned num, uint64_t *regs, const char *module, uint64_t offset);
void svc_register(unsigned num, const char *name, SvcHandler fn);
void svc_threads_register(void);   /* threads.c */
void svc_memory_register(void);    /* memory.c */
void svc_ipc_register(void);       /* ipc.c */
extern int svc_trace_all;

/* Threads and synchronization (threads.c) */
uint32_t threads_register_main(void);
uint32_t threads_current_handle(void);
uint64_t threads_current_id(void);
int threads_current_core(void);
/* Events: write end signals, read end is what the guest waits on. Used by services for system events. */
uint32_t event_create(uint32_t *readable_out);        /* returns the writable handle */
void event_signal_handle(uint32_t writable);
void event_clear_handle(uint32_t any);
int event_is_signaled(uint32_t readable);
void thread_core_away(int *token);                    /* a blocking host service gives its virtual core up */
void thread_core_back(int *token);
void thread_dump_all(void);

/* Memory (memory.c) */
void memory_init(void);                  /* reserve guest regions; idempotent */
uint64_t memory_heap_base(void);
uint64_t memory_alias_base(void);
uint64_t memory_alias_size(void);
uint64_t memory_stack_region_base(void);
uint64_t memory_stack_region_size(void);
uint64_t memory_aslr_base(void);
uint64_t memory_aslr_size(void);
int memory_contains(uint64_t addr, uint64_t size);   /* inside any region the guest may address */
void *guest_ptr(uint64_t addr, uint64_t size);       /* validated pointer or NULL */
void guest_write_prepare(uint64_t addr, size_t size);/* before a host write into guest memory (gfx dirty tracking) */
int guest_fault(void *addr, int is_write, void *ucontext);  /* gfx fault hook: 1 = handled */
extern uint64_t g_program_id;
extern uint64_t g_application_pool_bytes;            /* GetInfo TotalMemorySize */

/* NPDM facts the kernel/services need (npdm.c) */
typedef struct {
    char name[16]; uint8_t is_64bit, address_space, main_priority, main_core; uint32_t main_stack_size, version, system_resource_size;
    uint64_t title_id; uint64_t fs_perms;
    char services[64][12]; uint8_t service_host[64]; unsigned nservices;
    uint32_t svc_mask[6]; unsigned nsvc_allowed; uint16_t handle_table_size; int valid;
} Npdm;
extern Npdm g_npdm;
int npdm_load(const char *path);
int npdm_service_allowed(const char *name);

/* Launch configuration (main.c) */
typedef struct {
    const char *data_dir;        /* work/games/<titleid>/<buildid> */
    const char *save_root;       /* ~/Library/Application Support/Napple */
    const char *log_dir;
    char title_id[17], build_id[17];
    int headless, seconds;       /* seconds > 0: stop after that long (boot tests) */
    int op_mode_handheld;
    const char *language;
    char title_name[96];         /* window title (game.json "title_name"), else empty */
    unsigned nvn_major, nvn_minor;   /* game.json: NVN API version the game was built against (0,0 = gfx default) */
    int gfx_log;                 /* NAPPLE_GFX_LOG */
} RuntimeConfig;
extern RuntimeConfig g_cfg;
