/* iOS module images: the Mach-O dylibs `napple build --target ios` writes (crates/napple-build/src/macho.rs). Portable C (no Apple headers) so the
 * naming and the header checks are unit-tested on any host (tests/host/test_ios_dylib.c). */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define IOS_DYLIB_HEADER_PAGE 0x4000ULL /* guest image offset 0 sits this far past the Mach-O header */

/* Path of the dylib for module `file` (a module name, or an NRO's module id) whose images dir is `<data>/modules` or `<data>/nro`:
 * `<data>/../../Frameworks/<basename(data)>.<file>.dylib`, `<basename(data)>.nro.<file>.dylib` for NROs. 0, or -1 when it does not fit. */
int ios_dylib_path(char *out, size_t cap, const char *images_dir, const char *file);

/* Validates a mapped Mach-O (header at `hdr`) against the module layout from its tables. Returns NULL when it is the expected image
 * (segment protections equal what module.c seal() would have set on macOS), else a static message. */
const char *ios_dylib_check(const void *hdr, uint64_t text_end, uint64_t data_off, uint64_t key_off, uint64_t patch_off, uint64_t file_size);
