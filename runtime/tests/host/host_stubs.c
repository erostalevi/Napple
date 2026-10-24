/* Host (Linux / any POSIX) stand-ins for the macOS-only parts of the runtime, so the portable kernel and service code builds and runs its
 * unit tests off a Mac: `make -C runtime test-host`. Guest memory is host memory here (guest_ptr only bounds-checks), there are no modules,
 * and the TLR of a test thread is whatever host_thread_attach gave it. */
#include "napple.h"
#include "host_stubs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int log_verbose;
const char *summary_path;
RuntimeConfig g_cfg;
Npdm g_npdm;
uint64_t g_program_id;
uint64_t g_application_pool_bytes = 0xcd500000ULL;
pthread_key_t runtime_keys[8];
int host_fail_count;
char host_last_fail[512];

static void keys_init(void) __attribute__((constructor));
static void keys_init(void) { for (int i = 0; i < 8; i++) pthread_key_create(&runtime_keys[i], NULL); }

void log_line(const char *tag, const char *fmt, ...) {
    if (!getenv("HOST_TEST_LOG")) return;
    va_list ap; va_start(ap, fmt); fprintf(stderr, "[%s] ", tag); vfprintf(stderr, fmt, ap); fputc('\n', stderr); va_end(ap);
}
_Noreturn void runtime_fail(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(host_last_fail, sizeof host_last_fail, fmt, ap); va_end(ap);
    fprintf(stderr, "runtime_fail: %s\n", host_last_fail);
    abort();
}
void trace_event(const char *kind, const char *fmt, ...) { (void)kind; (void)fmt; }
int trace_enabled(void) { return 0; }
void summary_svc_unsupported(unsigned num, const char *name) { (void)num; (void)name; }
void summary_ipc(const char *service, unsigned cmd, int status, unsigned bytes) { (void)service; (void)cmd; (void)status; (void)bytes; }
void summary_nvn_missing(const char *name) { (void)name; }
void summary_note(const char *fmt, ...) { (void)fmt; }
void summary_stage(const char *stage) { (void)stage; }
void summary_write(const char *status, const char *reason) { (void)status; (void)reason; }
/* ipc.c is not part of the host kernel build: host sessions and memory objects never appear in these tests */
__attribute__((weak)) int ipc_close_object(ObjType type, void *obj) { (void)type; (void)obj; return 0; }

uint64_t runtime_now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
uint64_t runtime_guest_ticks(void) { return runtime_now_ns() * 12 / 625; }
void runtime_trace_guest(uint64_t fp, uint64_t lr) { (void)fp; (void)lr; }
void runtime_trace_guest_n(uint64_t fp, uint64_t lr, unsigned max) { (void)fp; (void)lr; (void)max; }
void runtime_hle_stack_set(uintptr_t top) { (void)top; }
void runtime_thread_init(void) {}
void runtime_thread_cleanup(void) {}
void runtime_thread_enter(uint64_t entry, uint64_t arg, uint64_t sp, uint64_t *host_context) { (void)entry; (void)arg; (void)sp; (void)host_context; runtime_fail("no guest code on the host"); }
_Noreturn void runtime_thread_leave(const uint64_t *host_context, void *arg, void (*fn)(void *)) { (void)host_context; fn(arg); abort(); }

void *guest_ptr(uint64_t addr, uint64_t size) {
    if (addr < 0x1000 || addr + size < addr) return NULL;
    return (void *)(uintptr_t)addr;
}
int memory_contains(uint64_t addr, uint64_t size) { return guest_ptr(addr, size) != NULL; }
void guest_write_prepare(uint64_t addr, size_t size) { (void)addr; (void)size; }
void memory_init(void) {}
uint64_t memory_heap_base(void) { return 0; }
uint64_t memory_heap_cap(void) { return 0; }
uint64_t memory_alias_base(void) { return 0; }
uint64_t memory_alias_size(void) { return 0; }
uint64_t memory_stack_region_base(void) { return 0; }
uint64_t memory_stack_region_size(void) { return 0; }
uint64_t memory_aslr_base(void) { return 0; }
uint64_t memory_aslr_size(void) { return 0; }
uint64_t memory_used_bytes(void) { return 0; }
uint64_t memory_exception_context(void) { return 0; }

/* module.c binds host overrides through the HLE stack switch and unbound imports to named traps (hle_stack.c, arm64 only) */
void *runtime_hle_wrap_named(void *fn, const char *name) { (void)name; return fn; }
static void unbound_import(void) { runtime_fail("unbound import called"); }
void *runtime_hle_wrap_trap(const char *name) { (void)name; return (void *)unbound_import; }
int npdm_service_allowed(const char *name) { (void)name; return 1; }

/* A fake guest heap for code memory, and aliases that just record what was asked. */
HostCodeMemoryFake host_code;
int memory_is_heap(uint64_t addr, uint64_t size) { return addr >= host_code.heap && addr + size <= host_code.heap + host_code.heap_size; }
int memory_range_unused(uint64_t addr, uint64_t size) { return addr >= host_code.free_lo && addr + size <= host_code.free_hi; }
uint32_t memory_code_alias(uint64_t dst, uint64_t src, uint64_t size, unsigned perm) { (void)src; (void)size; host_code.aliases++; host_code.last_dst = dst; host_code.last_perm = perm; return 0; }
void memory_code_unalias(uint64_t dst, uint64_t size) { (void)dst; (void)size; host_code.aliases--; }

/* Adopts the calling host thread as a guest thread with its own TLR. */
uint32_t host_thread_attach(void *tlr) {
    pthread_setspecific(runtime_keys[2], tlr);
    return threads_register_main();
}
