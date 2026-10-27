/* AArch32 (ARM + Thumb-2, VFPv4, NEON) CPU core.
 *
 * Apple Silicon cannot execute AArch32, so 32-bit Horizon titles (Mario Kart 8 Deluxe, ...) are run by this core: it decodes the guest's
 * instructions and executes them on 32-bit guest registers against the guest's 4 GiB window, with `svc`, the CP15 thread registers and
 * calls into host-implemented functions handed to the runtime through hooks. Portable C (no OS calls): builds and is tested on Linux
 * (runtime/tests/a32), the integration with the Horizon kernel lives in a32_proc.c.
 *
 * Design notes (docs/aarch32.md): instructions are decoded into A32Insn (one shared form for ARM and Thumb, so a later ahead-of-time
 * A64 translator can consume the same decoder) and kept in a direct-mapped cache validated by the raw instruction bits, so
 * self-modifying code and re-mapped pages stay correct. */
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef enum {
    A32_EXIT_BUDGET = 0, /* instruction budget used up (call a32_run again) */
    A32_EXIT_STOP,       /* a hook asked to stop (thread exit, process exit) */
    A32_EXIT_UNDEF,      /* undefined instruction (cpu->fault_pc / fault_word) */
    A32_EXIT_UNIMPL,     /* valid instruction this core does not implement yet (same fields): the list of these is the to-do list */
    A32_EXIT_BKPT,       /* bkpt / udf #imm */
    A32_EXIT_ALIGN,      /* alignment fault (ldrd/ldm/ldrex on an unaligned address) */
    A32_EXIT_NOBLOCK,    /* the pc has no lifted code (cpu->fault_pc): the lifted image is incomplete; there is no interpreter to fall back to */
} A32Exit;

struct A32Cpu;
typedef struct {
    /* svc #imm: arguments in r0..r7 of the cpu. Return non-zero to stop a32_run with A32_EXIT_STOP. */
    int (*svc)(struct A32Cpu *c, uint32_t imm);
    /* Called when the fetch address is inside [hle_lo, hle_hi): the host function implementing the guest import. It reads its arguments from
     * the cpu and sets the return value; the core then branches to lr (the stub is entered by bl/blx/b from guest code). Non-zero stops. */
    int (*hle)(struct A32Cpu *c, uint32_t pc);
    /* mrrc p15,1,rt,rt2,c14 (CNTVCT) and mrc p15,0,rt,c14,c0,0 (CNTFRQ): which = 0 vct, 1 frq */
    uint64_t (*counter)(struct A32Cpu *c, int which);
} A32Hooks;

typedef struct A32Cpu {
    uint32_t r[16];         /* r[15] is the address of the next instruction between steps */
    uint32_t n, z, c, v, q; /* flags, each 0 or 1 */
    uint32_t ge;            /* APSR.GE, 4 bits */
    uint32_t thumb;         /* CPSR.T */
    uint32_t itstate;       /* CPSR.IT (cond<<4 | mask), 0 outside an IT block */
    uint32_t fpscr;
    uint32_t fpexc_en; /* FPEXC.EN: the guest never clears it; kept so vmrs/vmsr FPEXC read back */
    union {
        uint64_t d[32];
        uint32_t s[64];
        uint8_t b[256];
    } v_; /* VFP/NEON register file (s0..s63 / d0..d31 / q0..q15), little endian host */
    uint32_t tpidruro, tpidrurw;
    /* exclusive monitor: ldrex records, strex compares-and-swaps against the recorded value */
    uint32_t ex_addr, ex_size, ex_valid;
    uint64_t ex_val;
    uint8_t *mem;            /* host pointer of guest address 0: guest address a is mem[a] (a is 32 bit) */
    uint32_t hle_lo, hle_hi; /* guest address range whose fetch calls hooks->hle (empty when equal) */
    const A32Hooks *hooks;
    void *user; /* the owning thread (a32_proc.c) */
    uint32_t fault_pc, fault_word, fault_addr;
    uint64_t insns; /* retired instructions (statistics) */
    void *cache;    /* decoded-instruction cache (a32_exec.c), allocated by the first a32_run */
    uint32_t *call_ring;
    uint32_t call_n; /* debug (a32_proc.c, NAPPLE_A32_CALLS=1): ring of the last 1024 call targets of bl / blx */
} A32Cpu;

#define VS(c, i) ((c)->v_.s[i])
#define VD(c, i) ((c)->v_.d[i])

void a32_reset(A32Cpu *c);
void a32_free(A32Cpu *c); /* releases the decode cache */
/* Runs up to `budget` instructions. */
/* Only with NAPPLE_A32_INTERP (the test oracle / debug build): the shipped runtime has no interpreter loop. */
A32Exit a32_run(A32Cpu *c, uint64_t budget);
/* CPSR as the guest reads it (mrs / the flags part of ldm^ etc.) and the inverse for msr. */
uint32_t a32_get_cpsr(const A32Cpu *c);
void a32_set_flags(A32Cpu *c, uint32_t cpsr_bits); /* N Z C V Q GE; the mode and T bits are not changed */
/* Name for an exit reason, "undefined" etc. */
const char *a32_exit_name(A32Exit e);
