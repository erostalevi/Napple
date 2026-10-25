#!/usr/bin/env python3
"""Differential fuzzer: one AArch32 instruction at a time, runtime/src/a32 core vs Unicorn (QEMU).

  python3 tests/a32/fuzz.py [--mode arm|thumb] [--count N] [--seed S] [--family NAME] [--cpu max|a15] [--show N] [--vfp]

Builds the core into a shared library, then for each random instruction word runs both implementations from the same random state and
compares r0-r15, the flags (N Z C V Q GE), the D registers (--vfp), FPSCR and the 96 KiB of backed memory. Unicorn faulting or rejecting
the instruction skips the sample (counted). Exit status 1 when any mismatch is found."""
import argparse, collections, ctypes, os, random, struct, subprocess, sys
from unicorn import *
from unicorn.arm_const import *

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', '..', 'src')
LIB = '/tmp/a32fuzz_%d.so' % os.getpid()
WIN_LO, WIN_HI = 0x8000, 0x20000


def build():
    d = os.path.join(SRC, 'a32')
    srcs = [os.path.join(d, f) for f in sorted(os.listdir(d)) if f.endswith('.c') and f not in ('a32_module.c', 'a32_proc.c', 'a32_hle.c')] + [os.path.join(HERE, 'fuzz_lib.c')]  # loader/process glue needs the runtime, so it is left out
    subprocess.check_call(['gcc', '-std=gnu11', '-O1', '-g', '-shared', '-fPIC', '-I' + SRC, '-o', LIB] + srcs)


class FState(ctypes.Structure):
    _fields_ = [('r', ctypes.c_uint32 * 16), ('cpsr', ctypes.c_uint32), ('d', ctypes.c_uint64 * 32), ('fpscr', ctypes.c_uint32),
                ('fault_word', ctypes.c_uint32), ('tpidruro', ctypes.c_uint32), ('itstate', ctypes.c_uint32)]


EXIT_NAMES = {0: 'ok', 1: 'stop', 2: 'undef', 3: 'unimpl', 4: 'bkpt', 5: 'align', 100: 'hostfault'}

# ARM encoding families: (value, mask) - the bits outside the mask are random
ARM_FAMILIES = {
    'dp_reg':   (0x00000000, 0x0e000010),
    'dp_regsh': (0x00000010, 0x0e000090),
    'dp_imm':   (0x02000000, 0x0e000000),
    'mul':      (0x00000090, 0x0f0000f0),
    'mull':     (0x00800090, 0x0f8000f0),
    'ldst_imm': (0x04000000, 0x0e000000),
    'ldst_reg': (0x06000000, 0x0e000010),
    'extra':    (0x000000b0, 0x0e0000b0),
    'extra2':   (0x000000d0, 0x0e0000d0),
    'ldm':      (0x08000000, 0x0e000000),
    'media':    (0x06000010, 0x0e000010),
    'misc':     (0x01000000, 0x0f900000),
    'branch':   (0x0a000000, 0x0e000000),
    'sync':     (0x01800090, 0x0f8000f0),
    'halfmul':  (0x01000080, 0x0f900090),
    'movw':     (0x03000000, 0x0fb00000),
    'uncond':   (0xf0000000, 0xf0000000),
    'cp':       (0x0e000010, 0x0f000010),
    'vfp_dp':   (0x0e000a00, 0x0f000e00),
    'vfp_ldst': (0x0c000a00, 0x0e000e00),
    'vfp_dp64': (0x0e000b00, 0x0f000f00),
    'vfp_xfer': (0x0e000a10, 0x0f000e10),
    'vfp_x64':  (0x0c400a00, 0x0fe00e00),
    'vfp_v8':   (0xfe000a00, 0xff000e00),
    'neon_dp':  (0xf2000000, 0xfe000000),
    'neon_ls':  (0xf4000000, 0xff100000),
}


def rnd_word(rng, fam):
    v, m = ARM_FAMILIES[fam]
    w = (rng.getrandbits(32) & ~m) | v
    if (v >> 28) != 0xf and rng.random() < 0.9:
        w = (w & 0x0fffffff) | 0xe0000000
    return w


def rnd_state(rng):
    r = []
    for _ in range(15):
        k = rng.random()
        if k < 0.45:
            r.append(0x10000 + (rng.getrandbits(14) & ~rng.choice([0, 3, 3, 7])))
        elif k < 0.7:
            r.append(rng.choice([0, 1, 2, 3, 0xff, 0x80, 0x7fffffff, 0x80000000, 0xffffffff, 0xfffffffe, 0x8000, 0xffff, 0x10000]))
        else:
            r.append(rng.getrandbits(32))
    r.append(0x8000)
    return r, (rng.getrandbits(5) << 27) | (rng.getrandbits(4) << 16)


def unpredictable(word, thumb):
    """Encodings the architecture leaves UNPREDICTABLE (pc as a base with writeback, base equal to a loaded register, ...): Unicorn and this core may
    legitimately differ, so those samples are not compared."""
    if thumb:
        if word >> 16 == 0 or (word & 0xffff) >> 11 not in (0x1d, 0x1e, 0x1f):
            return False
        hw1, hw2 = word & 0xffff, word >> 16
        top = hw1 >> 8
        if top in (0xea, 0xeb) or ((hw1 & 0xfa00) == 0xf000 and not hw2 >> 15):
            op4, s, rd = (hw1 >> 5) & 15, (hw1 >> 4) & 1, (hw2 >> 8) & 15
            if rd == 15 and not (s and op4 in (0, 4, 8, 13)):
                return True
        if top in (0xfa, 0xfb):
            rn, rd = hw1 & 15, (hw2 >> 8) & 15
            if rn == 15 or (top == 0xfb and (rd == 15 or (hw2 >> 12) == 15 and hw1 & 0x80)) or (top == 0xfa and rd == 15):
                return True
        if (hw1 & 0xfa00) == 0xf200 and not hw2 >> 15 and (hw2 >> 8) & 15 == 15:
            return True
        if top in (0xf8, 0xf9) and (hw2 >> 12) == 15:
            return True
        if top in (0xf8, 0xf9) and (hw1 & 15) == 15 and not hw1 & 0x10:
            return True
        if top in (0xe8, 0xe9):
            rn = hw1 & 15
            if rn == 15:
                return True
            if hw1 & 0x40:
                p, w = (hw1 >> 8) & 1, (hw1 >> 5) & 1
                rt, rt2 = hw2 >> 12, (hw2 >> 8) & 15
                if (p or w) and (rn == rt or rn == rt2):
                    return True
        return False
    cls = (word >> 25) & 7
    if (word >> 20) & 0xff == 0x75 and ((word >> 5) & 7) in (6, 7) and ((word >> 12) & 15) == 15:   # smmls with Ra = pc
        return True
    rn, rt = (word >> 16) & 15, (word >> 12) & 15
    if cls in (2, 3):                      # ldr / str
        p, w = (word >> 24) & 1, (word >> 21) & 1
        wb = (not p) or w
        if wb and (rn == 15 or rn == rt):
            return True
    if cls == 0 and (word & 0x90) == 0x90 and (word & 0x60):   # halfword / dual transfers
        p, w = (word >> 24) & 1, (word >> 21) & 1
        wb = (not p) or w
        if wb and (rn == 15 or rn == rt or rn == rt + 1):
            return True
        if rt == 15 or (word & 15) == 15:
            return True
    return False


def disasm(codes, thumb):
    """Mnemonic of each instruction (llvm-mc), for grouping the report; '?' when llvm-mc cannot decode it."""
    out = {}
    triple = 'thumbv8a' if thumb else 'armv8a'
    for c in codes:
        text = ' '.join('0x%02x' % b for b in bytes.fromhex(c))
        try:
            r = subprocess.run(['llvm-mc', '--disassemble', '-triple=' + triple, '-mattr=+neon,+fp-armv8,+crc'], input=text, capture_output=True, text=True, timeout=20)
            lines = [l.strip() for l in r.stdout.splitlines() if l.strip() and not l.strip().startswith('.')]
            out[c] = lines[0] if lines else '?'
        except Exception:
            out[c] = '?'
    return out


def rnd_double(rng):
    k = rng.random()
    if k < 0.3:
        return rng.choice([0, 0x8000000000000000, 0x3ff0000000000000, 0xbff0000000000000, 0x7ff0000000000000, 0xfff0000000000000,
                           0x7ff8000000000000, 0x4000000000000000, 0x3fe0000000000000, 0x7fefffffffffffff, 1, 0x000fffffffffffff])
    if k < 0.45:   # two packed floats
        f = rng.choice([0, 0x80000000, 0x3f800000, 0xbf800000, 0x7f800000, 0xff800000, 0x7fc00000, 0x40000000, 0x3f000000, 1, 0x007fffff, 0x4b800000])
        g = rng.choice([0, 0x80000000, 0x3f800000, 0xbf800000, 0x7f800000, 0x40490fdb, 0x42f60000, 0xc2c80000, 0x0000ffff, 0x7fffffff])
        return f | (g << 32)
    return rng.getrandbits(64)


reg_ids = [UC_ARM_REG_R0 + i for i in range(13)] + [UC_ARM_REG_SP, UC_ARM_REG_LR]
dreg_ids = [UC_ARM_REG_D0 + i for i in range(32)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--mode', default='arm')
    ap.add_argument('--count', type=int, default=20000)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--family', default='')
    ap.add_argument('--cpu', default='max')
    ap.add_argument('--show', type=int, default=25)
    ap.add_argument('--vfp', action='store_true', help='randomise the VFP/NEON state and compare it')
    args = ap.parse_args()
    build()
    lib = ctypes.CDLL(LIB)
    lib.ft_run.argtypes = [ctypes.POINTER(FState), ctypes.c_char_p, ctypes.c_int, ctypes.c_uint]
    rng = random.Random(args.seed)
    cpu = {'max': UC_CPU_ARM_MAX, 'a15': UC_CPU_ARM_CORTEX_A15}[args.cpu]
    def make_uc():
        u = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        u.ctl_set_cpu_model(cpu)
        u.mem_map(WIN_LO, WIN_HI - WIN_LO)
        # enable VFP/NEON: CPACR cp10/cp11 full access, FPEXC.EN
        u.reg_write(UC_ARM_REG_C1_C0_2, 0xf00000)
        u.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
        return u
    uc = make_uc()
    thumb = args.mode == 'thumb'
    fams = [args.family] if args.family else [f for f in ARM_FAMILIES if args.vfp or not f.startswith(('vfp', 'neon'))]
    stats = collections.Counter()
    bad = []
    unimpl = collections.Counter()
    accepted_by_us = collections.Counter()
    for _ in range(args.count):
        fam = rng.choice(fams)
        if thumb:
            if rng.random() < 0.4:
                code = struct.pack('<H', rng.getrandbits(16))
                if (struct.unpack('<H', code)[0] & 0xff00) == 0xbf00 and (struct.unpack('<H', code)[0] & 15):
                    code = struct.pack('<H', 0xbf00)   # IT blocks are covered by the program tests: Unicorn runs the whole block as one step
                if (struct.unpack('<H', code)[0] >> 11) in (0x1d, 0x1e, 0x1f):
                    code += struct.pack('<H', rng.getrandbits(16))
                else:
                    code += b'\x00\xbf'
            else:
                hw1 = rng.choice([0xe800, 0xe900, 0xea00, 0xeb00, 0xf000, 0xf100, 0xf200, 0xf300, 0xf400, 0xf500, 0xf600, 0xf700,
                                  0xf800, 0xf900, 0xfa00, 0xfb00] + ([0xec00, 0xed00, 0xee00, 0xef00, 0xfc00, 0xfd00, 0xfe00, 0xff00] if args.vfp else [])) | rng.getrandbits(8)
                code = struct.pack('<HH', hw1, rng.getrandbits(16))
        else:
            code = struct.pack('<I', rnd_word(rng, fam))
        word = struct.unpack('<I', code)[0]
        if unpredictable(word, thumb):
            continue
        regs, cpsr = rnd_state(rng)
        mem = bytearray(rng.getrandbits(8) for _ in range(64)) * ((WIN_HI - WIN_LO) // 64)
        mem[0:len(code)] = code
        dregs = [rnd_double(rng) for _ in range(32)] if args.vfp else [0] * 32
        fpscr = rng.choice([0, 0, 0x03000000, 0x00c00000, 0x01000000, 0x02000000, 0x00400000, 0x00800000]) if args.vfp else 0
        uc.mem_write(WIN_LO, bytes(mem))
        uc.ctl_remove_cache(WIN_LO, WIN_LO + 16)   # drop the translated block of the previous sample
        uc.reg_write(UC_ARM_REG_CPSR, cpsr | (0x20 if thumb else 0) | 0x10)   # user mode first: SP and LR are banked
        for i, rid in enumerate(reg_ids):
            uc.reg_write(rid, regs[i])
        if args.vfp:
            for i in range(32):
                uc.reg_write(dreg_ids[i], dregs[i])
            uc.reg_write(UC_ARM_REG_FPSCR, fpscr)
        res_uc = 'ok'
        try:
            uc.emu_start(WIN_LO | (1 if thumb else 0), WIN_LO + 0x100000, count=1)
        except UcError as e:
            res_uc = e.errno
            uc = make_uc()   # an error leaves the emulator unusable
            if os.environ.get('FUZZ_DEBUG') and stats['dbg'] < 5:
                stats['dbg'] += 1
                print('uc error', e, 'word=%s' % code.hex(), 'cpsr=%08x' % cpsr)
        st = FState()
        st.r[:] = regs
        st.cpsr = cpsr
        for i in range(32):
            st.d[i] = dregs[i]
        st.fpscr = fpscr
        buf = ctypes.create_string_buffer(bytes(mem), len(mem))
        if res_uc != 'ok':
            stats['uc_err_%s' % res_uc] += 1
            if res_uc == UC_ERR_INSN_INVALID:
                rc = lib.ft_run(ctypes.byref(st), buf, int(thumb), 1)
                if rc == 0:
                    accepted_by_us['%s' % code.hex()] += 1
            continue
        uregs = [uc.reg_read(rid) for rid in reg_ids] + [uc.reg_read(UC_ARM_REG_PC)]
        ucpsr = uc.reg_read(UC_ARM_REG_CPSR)
        umem = bytes(uc.mem_read(WIN_LO, WIN_HI - WIN_LO))
        rc = lib.ft_run(ctypes.byref(st), buf, int(thumb), 1)
        if rc in (2, 3):
            unimpl['%s %s' % (EXIT_NAMES[rc], code.hex())] += 1
            stats['we_' + EXIT_NAMES[rc]] += 1
            continue
        mism = []
        if rc != 0:
            mism.append('exit=%s' % EXIT_NAMES.get(rc, rc))
        else:
            for i in range(15):
                if uregs[i] != st.r[i]:
                    mism.append('r%d uc=%08x we=%08x' % (i, uregs[i], st.r[i]))
            pc_strict = fam in ('branch', 'dp_reg', 'dp_regsh', 'dp_imm', 'ldst_imm', 'ldst_reg', 'ldm') or thumb
            if pc_strict and uregs[15] != st.r[15] and (uregs[15] & ~3) != st.r[15]:   # bit 1 of a written pc is unpredictable; other classes with pc operands are unpredictable
                mism.append('pc uc=%08x we=%08x' % (uregs[15], st.r[15]))
            fm = 0xf80f0000
            if (ucpsr & fm) != (st.cpsr & fm):
                mism.append('cpsr uc=%08x we=%08x' % (ucpsr & fm, st.cpsr & fm))
            if umem != buf.raw:
                mism.append('memory differs')
            if args.vfp:
                for i in range(32):
                    u = uc.reg_read(dreg_ids[i])
                    if u != st.d[i]:
                        mism.append('d%d uc=%016x we=%016x' % (i, u, st.d[i]))
                uf = uc.reg_read(UC_ARM_REG_FPSCR)
                if (uf & 0xff000007) != (st.fpscr & 0xff000007):   # the sticky inexact / underflow / input-denormal bits are not compared
                    mism.append('fpscr uc=%08x we=%08x' % (uf, st.fpscr))
        if mism:
            bad.append((word, code.hex(), ' '.join(mism), regs, hex(cpsr)))
        stats['compared'] += 1
    print('compared', stats['compared'], dict((k, v) for k, v in stats.items() if k != 'compared'))
    if unimpl:
        print('encodings Unicorn executes that this core rejects (top %d):' % args.show)
        names = disasm([k.split()[1] for k, _ in unimpl.most_common(args.show)], thumb)
        for k, v in unimpl.most_common(args.show):
            print('   ', k, v, names.get(k.split()[1], '?'))
    if accepted_by_us:
        print('Unicorn rejects, this core executes (%d distinct); first few:' % len(accepted_by_us), list(accepted_by_us)[:args.show])
    print('mismatches: %d' % len(bad))
    names = disasm([b[1] for b in bad[:args.show]], thumb)
    for b in bad[:args.show]:
        print('   word=%08x code=%s %-34s %s' % (b[0], b[1], names.get(b[1], '?'), b[2]))
    os.unlink(LIB)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
