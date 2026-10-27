#!/usr/bin/env python3
"""Runs tests/a32/fuzz.py (core vs Unicorn) over every family in short child processes, so that a crash inside Unicorn (it dies with SIGILL on a few
NEON encodings on arm64 macOS hosts) only loses one chunk, and prints one table: compared samples and mismatches per family.

  python3 tests/a32/fuzz_all.py [--mode arm|thumb] [--per-family 20000] [--chunk 400] [--families a,b,...] [--seed S]

The child command is fuzz.py --count CHUNK --vfp --seed S+n --family F; samples that Unicorn rejects or crashes on, or that the architecture leaves
unpredictable, are not compared (fuzz.py counts them separately). Exit status 1 when any mismatch was seen."""
import argparse, collections, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)


def build_lib():
    src = os.path.join(HERE, '..', '..', 'src')
    d = os.path.join(src, 'a32')
    srcs = [os.path.join(d, f) for f in sorted(os.listdir(d)) if f.endswith('.c') and f not in ('a32_module.c', 'a32_proc.c', 'a32_hle.c')] + [os.path.join(HERE, 'fuzz_lib.c')]
    lib = '/tmp/a32fuzz_all_%d.so' % os.getpid()
    subprocess.check_call(['cc', '-std=gnu11', '-O1', '-g', '-shared', '-fPIC', '-DNAPPLE_A32_INTERP', '-I' + src, '-o', lib] + srcs)
    return lib


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--mode', default='arm')
    ap.add_argument('--per-family', type=int, default=20000, help='samples that must be compared per family')
    ap.add_argument('--chunk', type=int, default=400)
    ap.add_argument('--families', default='')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--max-chunks', type=int, default=400)
    ap.add_argument('--show', type=int, default=4)
    args = ap.parse_args()
    import fuzz  # noqa: for the family list
    if args.mode == 'thumb':
        fams = ['thumb']
    else:
        fams = args.families.split(',') if args.families else list(fuzz.ARM_FAMILIES)
    lib = build_lib()
    skipfile = '/tmp/a32fuzz_skip_%d.txt' % os.getpid()
    env = dict(os.environ, FUZZ_LIB=lib)
    total = collections.OrderedDict()
    mism_lines = []
    crashes = collections.Counter()
    for fam in fams:
        compared = mism = n = 0
        while compared < args.per_family and n < args.max_chunks:
            cmd = [sys.executable, os.path.join(HERE, 'fuzz.py'), '--mode', args.mode, '--count', str(args.chunk), '--vfp', '--seed', str(args.seed * 1000003 + n), '--show', '12']
            if args.mode == 'arm':
                cmd += ['--family', fam]
            n += 1
            env2 = dict(env, FUZZ_TRACE='1', FUZZ_SKIP=skipfile)
            for _ in range(6):   # a crash inside Unicorn: remember the word, rerun the same chunk (same seed, same samples) without it
                r = subprocess.run(cmd, capture_output=True, text=True, env=env2)
                last = [l for l in r.stdout.splitlines() if l.startswith('sample ')]
                if r.returncode >= 0 or not last:
                    break
                crashes[(fam, last[-1].split()[1])] += 1
                with open(skipfile, 'a') as f:
                    f.write(last[-1].split()[1] + '\n')
            if r.returncode < 0:
                continue
            m = re.search(r'^compared (\d+)', r.stdout, re.M)
            k = re.search(r'^mismatches: (\d+)', r.stdout, re.M)
            if not m or not k:
                crashes[(fam, 'no-report')] += 1
                continue
            compared += int(m.group(1))
            mism += int(k.group(1))
            if int(k.group(1)):
                mism_lines += [(fam, l) for l in r.stdout.splitlines() if l.startswith('   word=')][:args.show]
        total[fam] = (compared, mism, n)
        print('%-10s compared %7d  mismatches %4d  (%d chunks)' % (fam, compared, mism, n), flush=True)
    os.unlink(lib)
    if os.path.exists(skipfile):
        os.unlink(skipfile)
    print('total compared %d, mismatches %d, unicorn crashes (chunks lost) %d' % (sum(v[0] for v in total.values()), sum(v[1] for v in total.values()), sum(crashes.values())))
    for (fam, word), n in crashes.most_common(10):
        print('  unicorn crash: family %s word %s x%d' % (fam, word, n))
    groups = collections.defaultdict(list)
    for fam, l in mism_lines:
        m = re.match(r'\s*word=(\w+) code=(\w+) (.*?)\s{2,}(.*)$', l)
        mn = m.group(3).split()[0].split('.')[0] if m and m.group(3).strip() else '?'
        groups[(fam, mn)].append(l.strip())
    print('mismatches by family / mnemonic (first examples):')
    for (fam, mn), ls in sorted(groups.items()):
        print('  [%s] %-10s x%-3d %s' % (fam, mn, len(ls), ls[0][:200]))
    return 1 if mism_lines or any(v[1] for v in total.values()) else 0


if __name__ == '__main__':
    sys.exit(main())
