/* <mach/mach_vm.h> on macOS; on iOS the vm_* calls (same signatures, 64-bit address types on arm64) under the mach_vm_* names. */
#pragma once
#include "platform.h"
#include <mach/mach.h>
#if NAPPLE_IOS
#include <mach/vm_map.h>
/* The iOS headers already typedef mach_vm_address_t / mach_vm_size_t as uint64_t, but the vm_* functions take pointers to the
   (distinct, same-size) unsigned long vm_address_t / vm_size_t: make the mach_vm_* names mean those so &a, &got etc. type-check. */
#define mach_vm_address_t vm_address_t
#define mach_vm_size_t vm_size_t
#define mach_vm_map vm_map
#define mach_vm_remap vm_remap
#define mach_vm_read_overwrite vm_read_overwrite
#define mach_vm_region vm_region_64
#define mach_vm_deallocate vm_deallocate
#else
#include <mach/mach_vm.h>
#endif
