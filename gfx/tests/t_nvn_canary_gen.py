#!/usr/bin/env python3
# t_nvn_canary_gen.py <gfx dir>: the per-family call lists of tests/t_obj_canary.c, from nvn_protos.h + nvn_protos_ext.h (every entry point whose first parameter
# is a non-const pointer to the family's object). Prints C to stdout.
# Regenerate after changing nvn_protos*.h: python3 gfx/tests/t_nvn_canary_gen.py gfx, then paste the CALLS_* block over the one in t_obj_canary.c.
import re, sys
G = sys.argv[1]
src = open(G + '/src/nvnapi/nvn_protos.h').read() + open(G + '/src/nvnapi/nvn_protos_ext.h').read()
FAMS = ['DeviceBuilder', 'Device', 'QueueBuilder', 'Queue', 'Sync', 'EventBuilder', 'Event', 'MemoryPoolBuilder', 'MemoryPool', 'BufferBuilder', 'Buffer',
        'TextureBuilder', 'TextureView', 'Texture', 'SamplerBuilder', 'Sampler', 'TexturePool', 'SamplerPool', 'Program', 'WindowBuilder', 'Window',
        'CommandBuffer', 'BlendState', 'ColorState', 'ChannelMaskState', 'MultisampleState', 'PolygonState', 'DepthStencilState', 'VertexAttribState',
        'VertexStreamState']
SKIP = {
    # need a presentable window image / a second process; the queue object they take is 0x20 bytes of 0x2000
    'nvnQueuePresentTexture': 'needs an acquired window image', 'nvnQueueAcquireTexture': 'needs an acquired window image',
    'nvnSyncInitializeFromFencedGLSync': 'GL interop, refused',
}
def T(t):
    t = ' '.join(t.replace('*', ' * ').split()).replace(' *', '*')
    return t
SCAL = {'int': '1', 'uint32_t': '1', 'float': '1.0f', 'size_t': '0x1000', 'ptrdiff_t': '0', 'int64_t': '0', 'uint64_t': '0', 'NVNboolean': '1'}
PTR = {
    'NVNdevice*': 'h.dev', 'NVNmemoryPool*': 'h.pool', 'const NVNmemoryPool*': 'h.pool', 'NVNsync*': 'g_sync', 'const NVNsync*': 'g_sync',
    'NVNwindow*': 'g_win', 'NVNprogram*': 'g_prog', 'const NVNprogram*': 'g_prog', 'NVNtexture* const*': 'g_texarr', 'const NVNtexture* const*': '(const NVNtexture *const *)g_texarr',
    'const NVNtextureView* const*': '(const NVNtextureView *const *)g_viewarr', 'const NVNtexture*': 'g_tex', 'const NVNtextureView*': 'g_view', 'const NVNcommandHandle*': '&g_handle',
    'const NVNevent*': 'g_event', 'const NVNtexturePool*': 'g_tpool', 'const NVNsamplerPool*': 'g_spool',
    'const NVNbufferRange*': 'g_ranges', 'const NVNcopyRegion*': '&g_region', 'const NVNdrawTextureRegion*': '&g_dtr',
    'const char*': '"canary"', 'const float*': '(const float *)g_scr', 'const int*': '(const int *)g_scr', 'const uint32_t*': '(const uint32_t *)g_scr',
    'void*': 'g_scr', 'const void*': 'g_scr', 'int*': '(int *)g_out', 'NVNqueueErrorInfo*': '(NVNqueueErrorInfo *)g_out',
    'const NVNmappingRequest*': '(const NVNmappingRequest *)g_scr', 'const NVNpackagedTextureLayout*': '(const NVNpackagedTextureLayout *)g_scr',
    'const NVNshaderData*': 'g_sd', 'const NVNsubroutineLinkageMapPtr*': '(const NVNsubroutineLinkageMapPtr *)g_scr',
    'const NVNviewportSwizzle*': '(const NVNviewportSwizzle *)g_scr', 'NVNnativeWindow': 'NULL', 'const PFNNVNDEBUGCALLBACKPROC': 'NULL',
    'PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC': 'NULL', 'PFNNVNDEBUGCALLBACKPROC': 'NULL',
    'NVNbufferAddress': 'g_addr', 'NVNcommandHandle': '0', 'NVNtextureHandle': '0', 'NVNimageHandle': '0',
    'NVNseparateTextureHandle': '(NVNseparateTextureHandle){0}', 'NVNseparateSamplerHandle': '(NVNseparateSamplerHandle){0}',
    'const NVNtextureHandle*': '(const NVNtextureHandle *)g_scr', 'const NVNimageHandle*': '(const NVNimageHandle *)g_scr',
    'const NVNseparateTextureHandle*': '(const NVNseparateTextureHandle *)g_scr', 'const NVNseparateSamplerHandle*': '(const NVNseparateSamplerHandle *)g_scr',
    'const NVNblendState*': 'g_blend', 'const NVNchannelMaskState*': 'g_chmask', 'const NVNcolorState*': 'g_color', 'const NVNdepthStencilState*': 'g_ds',
    'const NVNmultisampleState*': 'g_ms', 'const NVNpolygonState*': 'g_poly', 'const NVNvertexAttribState*': 'g_va', 'const NVNvertexStreamState*': 'g_vs',
    'const NVNdeviceBuilder*': 'g_db', 'const NVNqueueBuilder*': 'g_qb', 'const NVNeventBuilder*': 'g_eb', 'const NVNmemoryPoolBuilder*': 'g_pb',
    'const NVNbufferBuilder*': 'g_bb', 'const NVNtextureBuilder*': 'g_tb', 'const NVNsamplerBuilder*': 'g_sb', 'const NVNwindowBuilder*': 'g_wb',
}
def arg(t):
    t = T(re.sub(r'\b(?!const\b)[a-z_][a-z0-9_]*\s*$', '', t.strip()) if re.search(r'[\*\s][a-z_][a-z0-9_]*\s*$', t) and t.split()[-1] not in SCAL else t)
    if '*' not in t and t.startswith('const '): t = t[6:]
    if t in SCAL: return SCAL[t]
    if t in PTR: return PTR[t]
    if re.match(r'^NVN[a-z]\w*$', t): return '(%s)1' % t      # enums
    raise SystemExit('unmapped type %r' % t)
fam_fns = {f: [] for f in FAMS}
for m in re.finditer(r'typedef\s+(.+?)\s*\(\*NVNT_(nvn\w+)\)\((.*?)\);', src, re.S):
    ret, name, params = m.groups()
    ps = [p.strip() for p in params.split(',')] if params.strip() not in ('', 'void') else []
    if not ps: continue
    for f in FAMS:
        if re.match(r'^NVN' + f[0].lower() + f[1:] + r'\s*\*', ps[0]) and name.startswith('nvn' + f) and not any(
                name.startswith('nvn' + g) for g in FAMS if g != f and g.startswith(f)):
            if name in SKIP: break
            fam_fns[f].append((name, [arg(p) for p in ps[1:]]))
            break
FIRST = ['SetDevice', 'SetDefaults', 'Initialize', 'AddCommandMemory', 'AddControlMemory', 'BeginRecording']
LAST = ['EndRecording', 'Finalize']
SPECIAL = {'nvnProgramSetShaders': ['2', 'g_sd'], 'nvnProgramSetShadersExt': ['2', 'g_sd'], 'nvnWindowBuilderSetTextures': ['2', 'g_texarr']}
def rank(item):
    name = item[0]
    for i, s in enumerate(FIRST):
        if name.endswith(s): return i - 100
    for i, s in enumerate(LAST):
        if name.endswith(s): return 100 + i
    return 0
for f in FAMS:
    fam_fns[f] = [(n, SPECIAL.get(n, a)) for n, a in sorted(fam_fns[f], key=rank)]
for f in FAMS:
    print('#define CALLS_%s(X) \\' % f)
    for name, args in fam_fns[f]:
        print('    X(%s, %s) \\' % (name, ', '.join(['o'] + args)))
    print('')
