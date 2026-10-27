#!/usr/bin/env python3
"""Targeted lifter-vs-interpreter check of every VFP / NEON / system instruction form the lifter inlines: assembles one representative instruction per form (a
list of mnemonics below, ARM and Thumb-2), and runs tests/a32jit/difftest on that exact word with 20000 random register / memory / FPSCR states each.

  python3 tests/a32jit/inline_words.py [--count N] [--difftest PATH]

Prints per form: samples compared, how many ran as inline code, mismatches. Exit status 1 on any mismatch, or when a form did not run inline (the lifter must handle it)."""
import argparse, os, re, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
FORMS = '''
vld1.8 {d0}, [r1]
vld1.16 {d2, d3}, [r2]!
vld1.32 {d4, d5, d6}, [r3], r4
vld1.64 {d8, d9, d10, d11}, [r5:128]
vst1.8 {d1}, [r6]
vst1.32 {d2, d3}, [r7]!
vst1.64 {d4, d5, d6, d7}, [r8], r9
vld1.32 {d4[0]}, [r1:32]
vld1.16 {d5[3]}, [r2]!
vld1.8 {d6[5]}, [r3], r4
vst1.32 {d7[1]}, [r5]
vst1.8 {d8[7]}, [r6]!
vld1.32 {d0[]}, [r1]
vld1.8 {d2[], d3[]}, [r2]!
vld1.16 {d4[]}, [r3], r4
vand q0, q1, q2
vbic d3, d4, d5
vorr q3, q4, q5
vorn d6, d7, d8
veor q6, q7, q8
vbsl d9, d10, d11
vbit q9, q10, q11
vbif d12, d13, d14
vadd.i8 d0, d1, d2
vadd.i16 q0, q1, q2
vadd.i32 d3, d4, d5
vadd.i64 q3, q4, q5
vsub.i8 q0, q1, q2
vsub.i64 d6, d7, d8
vmul.i8 d0, d1, d2
vmul.i16 q0, q1, q2
vmul.i32 d3, d4, d5
vceq.i8 d0, d1, d2
vceq.i32 q0, q1, q2
vtst.8 d0, d1, d2
vtst.16 q0, q1, q2
vcgt.s8 d0, d1, d2
vcgt.u16 q0, q1, q2
vcge.s32 d3, d4, d5
vcge.u8 q3, q4, q5
vmax.s8 d0, d1, d2
vmax.u16 q0, q1, q2
vmin.s32 d3, d4, d5
vmin.u8 q3, q4, q5
vadd.f32 d0, d1, d2
vadd.f32 q0, q1, q2
vsub.f32 d3, d4, d5
vsub.f32 q3, q4, q5
vmul.f32 d0, d1, d2
vmul.f32 q0, q1, q2
vmla.f32 d3, d4, d5
vmla.f32 q3, q4, q5
vmls.f32 d6, d7, d8
vmls.f32 q6, q7, q8
vfma.f32 d0, d1, d2
vfms.f32 q0, q1, q2
vabd.f32 d3, d4, d5
vceq.f32 d0, d1, d2
vcge.f32 q0, q1, q2
vcgt.f32 d3, d4, d5
vacge.f32 d6, d7, d8
vacgt.f32 q6, q7, q8
vmax.f32 d0, d1, d2
vmin.f32 q0, q1, q2
vcvt.f32.s32 d0, d1
vcvt.f32.u32 q0, q1
vcvt.s32.f32 d2, d3
vcvt.u32.f32 q2, q3
vldr s0, [r1]
vldr d1, [r2, #8]
vldr s3, [r3, #-12]
vldr d4, [pc, #20]
vstr s5, [r4]
vstr d6, [r5, #16]
vldmia r1, {d0-d3}
vldmia r2!, {s4-s9}
vldmdb r3!, {d4-d6}
vstmia r4, {d8-d11}
vstmia r5!, {s10-s13}
vstmdb sp!, {d8-d15}
vpop {d8-d15}
vpush {s16-s19}
vmov r0, s1
vmov s2, r3
vmov r1, r2, d3
vmov d4, r5, r6
vmov.f32 s0, s1
vmov.f64 d0, d1
vabs.f32 s0, s1
vabs.f64 d2, d3
vneg.f32 s4, s5
vneg.f64 d6, d7
vmov.f32 s0, #1.0
vmov.f64 d1, #-2.5
vadd.f32 s0, s1, s2
vadd.f64 d0, d1, d2
vsub.f32 s3, s4, s5
vsub.f64 d3, d4, d5
vmul.f32 s6, s7, s8
vmul.f64 d6, d7, d8
vnmul.f32 s9, s10, s11
vnmul.f64 d9, d10, d11
vdiv.f32 s12, s13, s14
vdiv.f64 d12, d13, d14
vsqrt.f32 s15, s16
vsqrt.f64 d15, d16
vmla.f32 s17, s18, s19
vmla.f64 d17, d18, d19
vmls.f32 s20, s21, s22
vmls.f64 d20, d21, d22
vcvt.s32.f32 s0, s1
vcvt.u32.f32 s2, s3
vcvt.s32.f64 s4, d5
vcvt.u32.f64 s6, d7
vcvt.f32.s32 s8, s9
vcvt.f32.u32 s10, s11
vcvt.f64.s32 d6, s13
vcvt.f64.u32 d7, s15
vcvt.f64.f32 d8, s17
vcvt.f32.f64 s18, d9
vcmp.f32 s0, s1
vcmpe.f32 s2, s3
vcmp.f64 d4, d5
vcmpe.f64 d6, d7
vcmp.f32 s8, #0
vcmpe.f64 d9, #0
vmrs APSR_nzcv, fpscr
vldr s0, [r1, #1020]
vext.8 d0, d1, d2, #3
vext.8 q0, q1, q2, #11
vtbl.8 d0, {d1}, d2
vtbl.8 d0, {d1, d2}, d3
vtbl.8 d0, {d2, d3, d4}, d5
vtbl.8 d0, {d4, d5, d6, d7}, d8
vtbx.8 d0, {d1}, d2
vtbx.8 d0, {d1, d2}, d3
vtbx.8 d0, {d3, d4, d5}, d6
vtbx.8 d0, {d4, d5, d6, d7}, d8
vdup.8 d0, d1[3]
vdup.16 q0, d1[2]
vdup.32 d0, d1[1]
vdup.8 d0, r1
vdup.16 q0, r2
vdup.32 d0, r3
vmov.8 d0[5], r1
vmov.16 d0[2], r1
vmov.32 d0[1], r1
vmov.s8 r0, d1[6]
vmov.u8 r0, d1[6]
vmov.s16 r0, d1[1]
vmov.u16 r0, d1[3]
vmov.32 r0, d1[1]
vmov.i8 d0, #0x55
vmov.i16 q0, #0x5500
vmov.i32 d0, #0x00ff0000
vmov.i32 q0, #0xff
vmov.i64 d0, #0xff00ff00ff00ff00
vmov.f32 q0, #1.5
vmvn.i32 d0, #0xff00
vorr.i32 q0, #0x12
vbic.i16 d0, #0x3400
vzip.8 d0, d1
vzip.16 q0, q1
vuzp.8 d2, d3
vuzp.32 q2, q3
vtrn.16 d4, d5
vtrn.32 d6, d7
vswp d0, d1
vswp q0, q1
vrev64.8 d0, d1
vrev32.16 q0, q1
vrev16.8 d0, d1
vmvn d0, d1
vcnt.8 q0, q1
vclz.i16 d0, d1
vcls.s32 q0, q1
vneg.s8 d0, d1
vabs.s32 q0, q1
vneg.f32 d0, d1
vabs.f32 q0, q1
vcgt.s16 d0, d1, #0
vcge.s32 q0, q1, #0
vceq.i8 d0, d1, #0
vcle.s16 q0, q1, #0
vclt.s32 d0, d1, #0
vcgt.f32 d0, d1, #0
vcge.f32 q0, q1, #0
vceq.f32 d0, d1, #0
vcle.f32 q0, q1, #0
vclt.f32 d0, d1, #0
vmovn.i16 d0, q1
vmovn.i32 d2, q2
vshll.i16 q0, d1, #16
vshll.s8 q0, d1, #3
vshll.u32 q0, d1, #7
vmovl.s16 q0, d1
vshrn.i32 d0, q1, #5
vrshrn.i16 d0, q1, #3
vshr.s8 d0, d1, #3
vshr.u16 q0, q1, #9
vshr.s32 d0, d1, #31
vshr.u64 q0, q1, #40
vshr.s64 d0, d1, #64
vshl.i8 d0, d1, #7
vshl.i32 q0, q1, #17
vshl.i64 d0, d1, #63
vsra.s16 d0, d1, #4
vsra.u32 q0, q1, #12
vrshr.s8 d0, d1, #2
vrshr.u64 d0, d1, #33
vrsra.s32 q0, q1, #7
vsri.16 d0, d1, #5
vsli.32 q0, q1, #9
vmul.i16 d0, d1, d2[1]
vmul.i32 q0, q1, d2[0]
vmul.f32 d0, d1, d2[1]
vmul.f32 q0, q1, d2[0]
vmla.i16 d0, d1, d2[3]
vmla.i32 q0, q1, d2[1]
vmla.f32 d0, d1, d2[0]
vmla.f32 q0, q1, d2[1]
vmls.i16 q0, q1, d2[2]
vmls.f32 d0, d1, d2[1]
vhadd.s8 d0, d1, d2
vhadd.u32 q0, q1, q2
vrhadd.s16 d0, d1, d2
vhsub.u8 q0, q1, q2
vabd.s8 d0, d1, d2
vabd.u16 q0, q1, q2
vaba.s32 d0, d1, d2
vaba.u8 q0, q1, q2
vmla.i8 d0, d1, d2
vmls.i32 q0, q1, q2
vpmax.s16 d0, d1, d2
vpmin.u8 d0, d1, d2
vpadd.i32 d0, d1, d2
vpadd.f32 d0, d1, d2
vpmax.f32 d0, d1, d2
vpmin.f32 d0, d1, d2
vmaxnm.f32 d0, d1, d2
vminnm.f32 q0, q1, q2
vcvta.s32.f32 d0, d1
vcvtn.u32.f32 q0, q1
vcvtp.s32.f32 d0, d1
vcvtm.u32.f32 q0, q1
vseleq.f32 s0, s1, s2
vselvs.f64 d0, d1, d2
vselge.f32 s3, s4, s5
vselgt.f64 d3, d4, d5
vmaxnm.f32 s0, s1, s2
vminnm.f64 d0, d1, d2
vrinta.f32 s0, s1
vrintn.f64 d0, d1
vrintp.f32 s2, s3
vrintm.f64 d2, d3
vrintz.f32 s0, s1
vrintr.f64 d0, d1
vrintx.f32 s2, s3
vcvta.s32.f32 s0, s1
vcvtn.u32.f64 s0, d1
vcvtp.s32.f64 s2, d3
vcvtm.u32.f32 s4, s5
vcvtr.s32.f32 s0, s1
vcvtr.u32.f64 s2, d3
vmrs r0, fpscr
vmsr fpscr, r1
ldaex r0, [r1]
lda r0, [r1]
ldab r0, [r1]
ldah r0, [r1]
stl r0, [r1]
stlb r0, [r1]
stlh r0, [r1]
tst r0, r1, lsl r2
tst r0, r1, lsr r2
tst r0, r1, asr r2
tst r0, r1, ror r2
teq r3, r4, lsl r5
movs r0, r1, lsl r2
movs r0, r1, lsr r2
movs r0, r1, asr r2
movs r0, r1, ror r2
ands r0, r1, r2, lsl r3
orrs r0, r1, r2, lsr r3
eors r0, r1, r2, asr r3
bics r0, r1, r2, ror r3
mvns r0, r1, lsl r2
'''.strip().splitlines()
THUMB_FORMS = [f for f in FORMS if not f.startswith(('vldr d4, [pc', 'lda', 'stl', 'tst', 'teq', 'movs', 'ands', 'orrs', 'eors', 'bics', 'mvns', 'ldaex')) and 'r5, r6' not in f and not re.search(r'\bs(p|r)\b', f)]  # register-shifted register operands do not exist in Thumb-2


def assemble(lines, thumb):
    src = '.syntax unified\n.text\n.%s\n' % ('thumb' if thumb else 'arm') + ''.join(l + '\n' for l in lines)
    d = '/tmp/a32iw_%d' % os.getpid()
    open(d + '.s', 'w').write(src)
    r = subprocess.run(['clang', '-c', '-target', 'thumbv8a-none-eabi' if thumb else 'armv8a-none-eabi', '-o', d + '.o', d + '.s'], capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(r.stderr)
    objdump = subprocess.run(['xcrun', '--find', 'llvm-objdump'], capture_output=True, text=True).stdout.strip()
    txt = subprocess.run([objdump, '-d', '--mattr=+neon,+fp-armv8', d + '.o'], capture_output=True, text=True).stdout
    words = []
    for l in txt.splitlines():
        m = re.match(r'^\s*[0-9a-f]+:\s+((?:[0-9a-f]{2} )+|[0-9a-f]{8}(?: [0-9a-f]{4})?|[0-9a-f]{4}(?: [0-9a-f]{4})?)\s', l)
        if m:
            hexs = m.group(1).split()
            if thumb:
                # objdump prints Thumb as 16-bit halfwords (hw1 hw2)
                hw = [int(h, 16) for h in hexs]
                words.append(hw[0] | (hw[1] << 16) if len(hw) > 1 else hw[0] | 0xbf000000)
            else:
                words.append(int(hexs[0], 16))
    os.unlink(d + '.s'), os.unlink(d + '.o')
    return words


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--count', type=int, default=20000)
    ap.add_argument('--difftest', default=os.path.join(HERE, '..', '..', 'build', 'tests', 'a32', 'difftest'))
    a = ap.parse_args()
    bad = 0
    tot_native = tot_cmp = 0
    for mode, forms in (('arm', FORMS), ('thumb', THUMB_FORMS)):
        words = assemble(forms, mode == 'thumb')
        if len(words) != len(forms):
            print('assembler/objdump produced %d words for %d forms (%s)' % (len(words), len(forms), mode))
            continue
        for f, w in zip(forms, words):
            r = subprocess.run([a.difftest, '--mode', mode, '--word', '0x%08x' % w, '--count', str(a.count), '--vfp'], capture_output=True, text=True)
            m = re.search(r'^compared (\d+) samples \((\d+) native, (\d+) through the interpreter\).*mismatches (\d+)', r.stdout, re.M)
            if not m:
                print('%-6s %-34s %08x  no result' % (mode, f, w))
                bad += 1
                continue
            cmp_, nat, _, mis = map(int, m.groups())
            tot_native += nat
            tot_cmp += cmp_
            flag = 'ok' if mis == 0 and nat > 0 else ('MISMATCH' if mis else 'NOT INLINE')
            if flag != 'ok':
                bad += 1
            print('%-6s %-34s %08x  compared %6d inline %6d mismatches %d  %s' % (mode, f, w, cmp_, nat, mis, flag))
    print('total: %d samples compared, %d ran as inline code, %d forms failed' % (tot_cmp, tot_native, bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
