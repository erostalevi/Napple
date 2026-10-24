/* Stub for Linux syntax checks only. */
#pragma once
#include <stdint.h>
typedef int kern_return_t; typedef unsigned mach_port_t; typedef uint64_t mach_vm_address_t, mach_vm_size_t;
#define KERN_SUCCESS 0
mach_port_t mach_task_self(void);
kern_return_t mach_vm_read_overwrite(mach_port_t, mach_vm_address_t, mach_vm_size_t, mach_vm_address_t, mach_vm_size_t *);
