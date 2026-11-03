/* swap_probe.c - how does the kernel account anonymous vs file-backed (MAP_SHARED, unlinked temp file) dirty memory?
 *
 * Maps N MiB each way, dirties every page, then prints TASK_VM_INFO after each step: phys_footprint (the figure jetsam compares with the
 * limit), internal / external / compressed pages, resident size and, on iOS, os_proc_available_memory(). Also runs the "evict" step the runtime uses
 * (msync MS_SYNC then MS_DEACTIVATE) and prints again.
 *
 *   macOS      cc -O2 -o swap_probe tools/swap_probe.c && ./swap_probe 2048
 *   Simulator  xcrun --sdk iphonesimulator clang -target arm64-apple-ios26.0-simulator -O2 tools/swap_probe.c -o swap_probe_sim &&
 *              xcrun simctl spawn booted ./swap_probe_sim 2048
 *   Device     same compile for iphoneos, wrap in a throwaway app (the probe's main() as the app's startup) and read the log lines.
 *
 * The probe prints one line per step; the interesting columns are footprint and ext (file-backed pages). Output "PROBE ..." lines are grep-able. */
#include <mach/mach.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <TargetConditionals.h>
#if TARGET_OS_IOS
#include <os/proc.h>
#endif

#define MB(x) ((unsigned long long)(x) >> 20)

static void show(const char *step) {
    task_vm_info_data_t vi;
    mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &n) != KERN_SUCCESS) return;
    unsigned long long avail = 0;
#if TARGET_OS_IOS
    avail = (unsigned long long)os_proc_available_memory();
#endif
    printf("PROBE %-28s footprint %6llu MB  internal %6llu  external %6llu  compressed %6llu  resident %6llu  peak %6llu  avail %llu\n", step, MB(vi.phys_footprint),
           MB(vi.internal), MB(vi.external), MB(vi.compressed), MB(vi.resident_size), MB(vi.ledger_phys_footprint_peak), MB(avail));
    fflush(stdout);
}

static void dirty(uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i += 16384) p[i] = (uint8_t)(i >> 14) | 1;
}

static uint8_t *file_map(size_t n) {
    char path[1024];
    const char *t = getenv("TMPDIR");
    snprintf(path, sizeof path, "%s/swap_probe.XXXXXX", t && *t ? t : "/tmp");
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); exit(2); }
    unlink(path); /* the space vanishes with the last reference: exit, crash or jetsam */
    if (ftruncate(fd, (off_t)n)) { perror("ftruncate"); exit(2); }
    uint8_t *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) { perror("mmap"); exit(2); }
    return p;
}

int main(int argc, char **argv) {
    size_t mb = argc > 1 ? (size_t)atol(argv[1]) : 1024, n = mb << 20;
    int which = argc > 2 ? atoi(argv[2]) : 3; /* bit 0: anonymous, bit 1: file-backed */
    setvbuf(stdout, NULL, _IOLBF, 0);
    show("start");
    if (which & 1) {
        uint8_t *a = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (a == MAP_FAILED) { perror("mmap anon"); return 2; }
        show("anon mapped (untouched)");
        dirty(a, n);
        show("anon dirtied");
        munmap(a, n);
        show("anon unmapped");
    }
    if (which & 2) {
        uint8_t *f = file_map(n);
        show("file mapped (untouched)");
        dirty(f, n);
        show("file dirtied");
        msync(f, n, MS_ASYNC);
        sleep(2);
        show("file msync(ASYNC)+2s");
        msync(f, n, MS_SYNC);
        show("file msync(SYNC)");
        msync(f, n, MS_SYNC | MS_DEACTIVATE);
        show("file msync(SYNC|DEACTIVATE)");
        /* touch again: pages come back from the file, not from zero */
        size_t bad = 0;
        for (size_t i = 0; i < n; i += 16384) bad += f[i] != ((uint8_t)(i >> 14) | 1);
        printf("PROBE reread mismatches %zu (0 = data survived eviction)\n", bad);
        show("file re-read");
        munmap(f, n);
        show("file unmapped");
    }
    return 0;
}
