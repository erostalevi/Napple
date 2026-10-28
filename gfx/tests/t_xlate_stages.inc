/* -- geometry / tessellation stages, transform feedback / stage-out, and the OUT / VABSDIFF4 / TXA / IMADSP ops. Included by t_surface.inc inside main().
 * The field layouts of the four ops are the ones the translator decodes (see the comments at their branches in maxwell_translate.c); the tests pin that reading. */
{
#define ST_ALD(d, ra, addr, n, vtx, patch, outr)                                                                                                               \
    (0xefd8000000000000ull | (uint64_t)(d) | ((uint64_t)(ra) << 8) | ((uint64_t)(addr) << 20) | ((uint64_t)((n)-1) << 47) | ((uint64_t)(vtx) << 39) |        \
     ((uint64_t)(patch) << 31) | ((uint64_t)(outr) << 32) | PT_BITS)
#define ST_AST(s, addr, n, patch)                                                                                                                              \
    (0xeff0000000000000ull | (uint64_t)(s) | (255ull << 8) | ((uint64_t)(addr) << 20) | ((uint64_t)((n)-1) << 47) | ((uint64_t)(patch) << 31) | PT_BITS)
#define ST_S2R(d, sr) (0xf0c8000000000000ull | (uint64_t)(d) | ((uint64_t)(sr) << 20) | PT_BITS)
#define ST_OUT(d, mode) (0xfbe0000000000000ull | (uint64_t)(d) | (255ull << 8) | (255ull << 20) | ((uint64_t)(mode) << 39) | PT_BITS)
    ShaderKey gk = z;
    gk.gs_max_vertices = 6;
    gk.gs_in_vertices = 3;
    gk.gs_out_topology = GS_OUT_TRIANGLE_STRIP;
    /* ---- geometry: one thread per input primitive, vertex-stage records in, strips with restart indices out */
    {
        XL_OK(g1, STAGE_GEOMETRY, gk, 0, "GS", ST_ALD(4, 255, 0x70, 4, 8, 0, 0), ST_AST(4, 0x70, 4, 0), ST_ALD(8, 255, 0x80, 4, 8, 0, 0), ST_AST(8, 0x80, 4, 0),
              ST_OUT(10, 1), ST_OUT(255, 2), ins_exit());
        CHECK(HAS(g1, "kernel void main0(uint gid [[thread_position_in_grid]], constant uint *sp [[buffer(20)]], device const float4 *stage_in [[buffer(19)]], "
                      "device float4 *gs_vtx [[buffer(21)]], device uint *gs_idx [[buffer(22)]]"),
              "GS entry point and buffer contract");
        CHECK(HAS(g1, "constant uint GS_MAXV = 6u;"), "GS_MAXV from the key");
        CHECK(HAS(g1, "stage_in[(sp[1] + gs_pid * 3u + vx) * 34u + 1u][0u]"), "GS ALD of the position reads vertex vx (latched Rb) of the primitive (slot 1)");
        CHECK(HAS(g1, "stage_in[(sp[1] + gs_pid * 3u + vx) * 34u + 2u][0u]"), "GS ALD of generic 0 reads slot 2");
        CHECK(HAS(g1, "gso[1][0] = as_type<float>(r4);") && HAS(g1, "gso[2][3] = as_type<float>(r"), "GS AST fills the staging vertex");
        CHECK(HAS(g1, "gs_put(gs_vtx + (gid * GS_MAXV + gs_n) * 34u, gso); gs_idx[gid * 2u * GS_MAXV + gs_ic++] = gid * GS_MAXV + gs_n; gs_n++; gs_open = true;"),
              "OUT.EMIT appends a vertex and its index");
        CHECK(HAS(g1, "if (gs_open) { gs_ic++; gs_open = false; }"), "OUT.CUT ends the strip (the restart index is pre-filled)");
        CHECK(HAS(g1, "r10 = gs_n;"), "OUT writes the emitted count to Rd");
        CHECK(HAS(g1, "gs_idx[gid * 2u * GS_MAXV + i] = 0xFFFFFFFFu;"), "the index region starts as primitive restart");
        CHECK(HAS(g1, "static inline void gs_put(") && HAS(g1, "dst[1] = p;") && HAS(g1, "dst[2] = src[2];") && !HAS(g1, "dst[3] = src[3];"),
              "gs_put copies the position and only the stored generics");
        CHECK((g1->stage_slot_mask & 6ull) == 6ull && !(g1->stage_slot_mask & 8ull) && g1->gs_max_vertices == 6 && g1->gs_out_topology == GS_OUT_TRIANGLE_STRIP,
              "ShaderInfo: stage_slot_mask, gs_max_vertices, gs_out_topology");
        CHECK(g1->attr_mask & 1u, "ShaderInfo.attr_mask names the generic read");
        maxwell_free(g1);
        ShaderKey gf = gk;
        gf.flip_y = true;
        gf.depth_minus_one = true;
        XL_OK(g2, STAGE_GEOMETRY, gf, 0, "GS clip", ST_ALD(4, 255, 0x70, 4, 255, 0, 0), ST_AST(4, 0x70, 4, 0), ST_OUT(255, 3), ins_exit());
        CHECK(HAS(g2, "p.y = -p.y;") && HAS(g2, "p.z = (p.z + p.w) * 0.5f;"), "GS emit applies the clip-space conversions to the position");
        CHECK(HAS(g2, "if (gs_n < GS_MAXV)") && HAS(g2, "if (gs_open)"), "OUT.EMIT_THEN_CUT does both");
        CHECK(HAS(g2, "stage_in[(sp[1] + gs_pid * 3u + 0u) * 34u + 1u][0u]"), "no vertex register reads vertex 0");
        maxwell_free(g2);
        ShaderKey gl = gk;
        gl.gs_in_vertices = 6;
        XL_OK(g3, STAGE_GEOMETRY, gl, 0, "GS adjacency", ST_S2R(2, 0x11), ST_ALD(4, 255, 0x60, 1, 255, 0, 0), ins_exit());
        CHECK(HAS(g3, "gs_pid * 6u") || HAS(g3, "(sp[2] + gs_pid)"), "adjacency primitive: six vertices; attribute 0x60 is the primitive id");
        CHECK(HAS(g3, "r2 = inv;") && HAS(g3, "gs_pid = gid / gs_ninv"), "SR_INVOCATION_ID is the geometry instance (gid % sp[3])");
        maxwell_free(g3);
        XL_OK(g4, STAGE_GEOMETRY, gk, 0, "GS indexed read", ST_ALD(4, 20, 0x80, 1, 8, 0, 0), ins_exit());
        CHECK(HAS(g4, "stage_in[(sp[1] + gs_pid * 3u + vx) * 34u + min("), "an indexed ALD computes the slot at run time");
        maxwell_free(g4);
        XL_REFUSE(g5, STAGE_GEOMETRY, z, 0, "gs_max_vertices", "GS without a vertex limit", ST_OUT(255, 1), ins_exit());
        ShaderKey gb = gk;
        gb.gs_max_vertices = 1025;
        XL_REFUSE(g6, STAGE_GEOMETRY, gb, 0, "1024", "GS over the hardware limit", ST_OUT(255, 1), ins_exit());
        gb = gk;
        gb.gs_in_vertices = 5;
        XL_REFUSE(g7, STAGE_GEOMETRY, gb, 0, "input primitive of 5", "GS with a five-vertex input", ST_OUT(255, 1), ins_exit());
        gb = gk;
        gb.xfb_count = 1;
        XL_REFUSE(g8, STAGE_GEOMETRY, gb, 0, "geometry shader is captured by the replay", "GS xfb is the replay's", ST_OUT(255, 1), ins_exit());
        XL_REFUSE(g9, STAGE_VERTEX, z, 0, "OUT", "OUT in a vertex shader", ST_OUT(255, 1), ins_exit());
        XL_REFUSE(g10, STAGE_TESS_EVAL, (ShaderKey){.tess_out_vertices = 4}, 0, "OUT", "OUT in a tess eval shader", ST_OUT(255, 1), ins_exit());
        /* OUT forms: the immediate and constant buffer encodings */
        XL_OK(g11, STAGE_GEOMETRY, gk, 0, "OUT imm", 0xf6e0000000000000ull | (255ull << 8) | (1ull << 39) | (1ull << 20) | PT_BITS, ins_exit());
        CHECK(HAS(g11, "gs_n++"), "OUT immediate form");
        maxwell_free(g11);
        XL_OK(g12, STAGE_GEOMETRY, gk, 0, "OUT cbuf", 0xebe0000000000000ull | (255ull << 8) | (2ull << 39) | PT_BITS, ins_exit());
        CHECK(HAS(g12, "if (gs_open)"), "OUT constant buffer form");
        maxwell_free(g12);
    }
    /* ---- tessellation control: compute kernel, one threadgroup per patch, one thread per output control point */
    ShaderKey tk = z;
    tk.tess_in_vertices = 3;
    tk.tess_out_vertices = 4;
    tk.tess_domain = TESS_DOMAIN_QUAD;
    {
        XL_OK(t1, STAGE_TESS_CTRL, tk, 0, "TCS", ST_S2R(0, 0x11), ST_ALD(4, 255, 0x80, 4, 8, 0, 0), ST_AST(4, 0x80, 4, 0), i_bar(),
              ST_ALD(12, 255, 0x80, 4, 9, 0, 1), ST_AST(12, 0x00, 4, 1), ST_AST(12, 0x10, 2, 1), ST_AST(12, 0x20, 4, 1), ins_exit());
        CHECK(HAS(t1, "kernel void main0(uint3 tid [[thread_position_in_threadgroup]], uint3 ctaid [[threadgroup_position_in_grid]], constant uint *sp [[buffer(20)]], "
                      "device const float4 *stage_in [[buffer(19)]], device float4 *tess_vtx [[buffer(21)]], device float4 *tess_patch [[buffer(22)]], "
                      "device half *tess_fac [[buffer(23)]]"),
              "TCS entry point and buffer contract");
        CHECK(HAS(t1, "uint inv = tid.x, pid = ctaid.x;") && HAS(t1, "if (pid >= sp[0]) return;"), "TCS: threadgroup = patch, thread = output control point");
        CHECK(HAS(t1, "r0 = inv;"), "SR_INVOCATION_ID is the output control point");
        CHECK(HAS(t1, "stage_in[(sp[1] + pid * 3u + vx) * 34u + 2u][0u]"), "TCS ALD reads an input control point (3 per patch)");
        CHECK(HAS(t1, "tess_vtx[(pid * 4u + inv) * 34u + 2][0] = as_type<float>(r4);"), "TCS AST stores into the thread's own control point");
        CHECK(HAS(t1, "tess_vtx[(pid * 4u + vx) * 34u + 2u][0u]"), "TCS ALD.O reads another invocation's output");
        CHECK(HAS(t1, "threadgroup_barrier(mem_flags::mem_device | mem_flags::mem_threadgroup);"), "BAR also orders device memory in a tess control shader");
        CHECK(HAS(t1, "tess_patch[pid * 34u + 0][0] = as_type<float>(r12);") && HAS(t1, "tess_patch[pid * 34u + 1][1] = as_type<float>(r13);") &&
                  HAS(t1, "tess_patch[pid * 34u + 2][0] = as_type<float>(r12);"),
              "TCS AST.P writes the patch record: levels in slots 0 / 1, generic patch attribute 0 in slot 2");
        CHECK(HAS(t1, "tess_fac[pid * 6u + i] = half(tess_patch[pid * 34u + 0u][i]);") && HAS(t1, "tess_fac[pid * 6u + 4u + i]"),
              "quad factors: edge[4], inside[2] halves per patch");
        CHECK((t1->patch_slot_mask & 7ull) == 7ull && (t1->stage_slot_mask & (1ull << 2)) && t1->tess_out_vertices == 4 && t1->tess_domain == TESS_DOMAIN_QUAD,
              "ShaderInfo: patch_slot_mask, stage_slot_mask, tess_out_vertices, tess_domain");
        maxwell_free(t1);
        ShaderKey tt = tk;
        tt.tess_domain = TESS_DOMAIN_TRIANGLE;
        XL_OK(t2, STAGE_TESS_CTRL, tt, 0, "TCS triangle", ST_AST(4, 0x00, 4, 1), ST_AST(4, 0x10, 1, 1), ins_exit());
        CHECK(HAS(t2, "tess_fac[pid * 4u + i] = half(tess_patch[pid * 34u + 0u][i]);") && HAS(t2, "tess_fac[pid * 4u + 3u] = half(tess_patch[pid * 34u + 1u][0]);"),
              "triangle factors: edge[3], inside[1] halves per patch");
        maxwell_free(t2);
        XL_OK(t3, STAGE_TESS_CTRL, tk, 0, "TCS without levels", ST_ALD(4, 255, 0x80, 4, 8, 0, 0), ST_AST(4, 0x80, 4, 0), ins_exit());
        CHECK(!HAS(t3, "tess_fac[pid") && t3->patch_slot_mask == 0, "a program that stores no levels leaves the factor buffer to the replay (state default levels)");
        maxwell_free(t3);
        ShaderKey ti = tk;
        ti.tess_domain = TESS_DOMAIN_ISOLINE;
        XL_REFUSE(t4, STAGE_TESS_CTRL, ti, 0, "isoline", "isoline tessellation", ins_exit());
        ti = tk;
        ti.tess_out_vertices = 33;
        XL_REFUSE(t5, STAGE_TESS_CTRL, ti, 0, "tess_out_vertices", "33 output control points", ins_exit());
        ti = tk;
        ti.tess_in_vertices = 0;
        XL_REFUSE(t6, STAGE_TESS_CTRL, ti, 0, "tess_in_vertices", "no input control points", ins_exit());
    }
    /* ---- tessellation evaluation: post-tessellation vertex function */
    {
        XL_OK(e1, STAGE_TESS_EVAL, tk, 0, "TES", ST_S2R(0, 0x11), ST_ALD(4, 255, 0x2f0, 2, 255, 0, 0), ST_ALD(8, 255, 0x80, 4, 8, 0, 0), ST_ALD(12, 255, 0x00, 4, 255, 1, 0),
              ST_ALD(16, 255, 0x60, 1, 255, 0, 0), ST_AST(8, 0x70, 4, 0), ST_AST(8, 0x80, 4, 0), ins_exit());
        CHECK(HAS(e1, "[[patch(quad, 4)]] vertex VOut main0(uint patch_id [[patch_id]], float2 tess_coord [[position_in_patch]], constant uint *sp [[buffer(20)]], "
                      "device const float4 *tess_vtx [[buffer(21)]], device const float4 *tess_patch [[buffer(22)]]"),
              "TES entry point and buffer contract");
        CHECK(HAS(e1, "r4 = as_type<uint>(tess_coord[0]);") && HAS(e1, "r5 = as_type<uint>(tess_coord[1]);"), "attributes 0x2F0 / 0x2F4 are the tessellation coordinate");
        CHECK(HAS(e1, "tess_vtx[(patch_id * 4u + vx) * 34u + 2u][0u]"), "TES ALD reads a control point of the patch from the TCS output");
        CHECK(HAS(e1, "tess_patch[patch_id * 34u + 0u][0u]"), "TES ALD.P reads the patch record (levels at slot 0)");
        CHECK(HAS(e1, "r16 = (sp[2] + patch_id);"), "attribute 0x60 is the patch id");
        CHECK(HAS(e1, "out.pos[0] = as_type<float>(r8);") && HAS(e1, "out.v0[0] = as_type<float>(r8);") && HAS(e1, "struct VOut"), "TES outputs leave through the vertex output struct");
        CHECK(!HAS(e1, "struct VIn"), "a post-tessellation vertex function has no stage_in vertex fetch");
        maxwell_free(e1);
        ShaderKey et = tk;
        et.tess_domain = TESS_DOMAIN_TRIANGLE;
        et.flip_y = true;
        XL_OK(e2, STAGE_TESS_EVAL, et, 0, "TES triangle", ST_ALD(4, 255, 0x2f0, 2, 255, 0, 0), ST_AST(4, 0x70, 4, 0), ins_exit());
        CHECK(HAS(e2, "[[patch(triangle, 4)]]") && HAS(e2, "float3 tess_coord") && HAS(e2, "out.pos.y = -out.pos.y;"), "triangle domain: float3 coordinate, clip conversions as a vertex shader");
        maxwell_free(e2);
        ShaderKey ex = tk;
        ex.xfb_count = 1;
        XL_REFUSE(e3, STAGE_TESS_EVAL, ex, 0, "tessellation evaluation shader", "TES transform feedback", ins_exit());
        ex = tk;
        ex.tess_domain = TESS_DOMAIN_ISOLINE;
        XL_REFUSE(e4, STAGE_TESS_EVAL, ex, 0, "isoline", "TES isoline", ins_exit());
    }
    /* ---- vertex stage capture: transform feedback and the stage-out record */
    {
        ShaderKey xk = z;
        xk.xfb_count = 3;
        xk.xfb_stride[0] = 32;
        xk.xfb_stride[2] = 12;
        xk.xfb[0] = (ShaderXfbEntry){.addr = 0x70, .buffer = 0, .comps = 4, .offset = 0};
        xk.xfb[1] = (ShaderXfbEntry){.addr = 0x80, .buffer = 0, .comps = 4, .offset = 16};
        xk.xfb[2] = (ShaderXfbEntry){.addr = 0x90, .buffer = 2, .comps = 3, .offset = 0};
        XL_OK(x1, STAGE_VERTEX, xk, 0, "XFB", ins_ald(0, 0x80, 4), ins_ast(0, 0x70, 4), ins_ast(0, 0x80, 4), ins_exit());
        CHECK(HAS(x1, ", constant uint *xfb_ctl [[buffer(28)]]") && HAS(x1, ", device uint *xfb0 [[buffer(24)]]") && HAS(x1, ", device uint *xfb2 [[buffer(26)]]") &&
                  !HAS(x1, "xfb1 ") && !HAS(x1, "stage_out"),
              "XFB arguments: control buffer and the buffers used, at XFB_BUFFER_BASE + k");
        CHECK(HAS(x1, "uint xo = xfb_ctl[0] * iid + (vid - xfb_ctl[1]);"), "vertex ordinal of the capture");
        CHECK(HAS(x1, "xfb0[xfb_ctl[2] + xo * 8u + 0u] = as_type<uint>(out.pos[0]);") && HAS(x1, "xfb0[xfb_ctl[2] + xo * 8u + 3u] = as_type<uint>(out.pos[3]);"),
              "position captured at offset 0 of xfb buffer 0 (stride 32)");
        CHECK(HAS(x1, "xfb0[xfb_ctl[2] + xo * 8u + 4u] = as_type<uint>(out.v0[0]);"), "generic 0 captured at byte offset 16");
        CHECK(HAS(x1, "xfb2[xfb_ctl[4] + xo * 3u + 2u] = as_type<uint>(0.0f);"), "an output the program never wrote captures as 0");
        CHECK(strstr(x1->msl, "xfb0[xfb_ctl[2]") < strstr(x1->msl, "out.pos.y") || !HAS(x1, "out.pos.y = -out.pos.y"), "capture happens before the clip-space epilogue");
        CHECK(x1->uses_xfb && !x1->uses_stage_out && x1->xfb_buffer_mask == 5u, "ShaderInfo: uses_xfb, xfb_buffer_mask");
        maxwell_free(x1);
        ShaderKey xf = xk;
        xf.flip_y = true;
        XL_OK(x2, STAGE_VERTEX, xf, 0, "XFB flip", ins_ald(0, 0x80, 4), ins_ast(0, 0x70, 4), ins_exit());
        CHECK(strstr(x2->msl, "xfb0[xfb_ctl[2]") < strstr(x2->msl, "out.pos.y = -out.pos.y"), "XFB stores the shader's position, not the flipped one");
        maxwell_free(x2);
        ShaderKey so = z;
        so.stage_out = true;
        XL_OK(s1, STAGE_VERTEX, so, 0, "stage out", ins_ald(0, 0x80, 4), ins_ast(0, 0x70, 4), ins_ast(0, 0x84 + 0x10, 4), ins_ast(0, 0x68, 1), ins_exit());
        CHECK(HAS(s1, ", device float4 *stage_out [[buffer(29)]]") && HAS(s1, "stage_out[xo * 34u + 1] = out.pos;") && HAS(s1, "stage_out[xo * 34u + 3] = out.v1;") &&
                  HAS(s1, "stage_out[xo * 34u + 0] = vsys;") && HAS(s1, "vsys[2] = as_type<float>(r0);") && HAS(s1, "float4 vsys = float4(0.0f);"),
              "stage-out record: system slot 0, position slot 1, generic n at slot 2 + n");
        CHECK(s1->uses_stage_out && (s1->stage_slot_mask & 0xbull) == 0xbull, "ShaderInfo: uses_stage_out, stage_slot_mask");
        maxwell_free(s1);
        /* the point size store also lands in the system slot */
        XL_OK(s2, STAGE_VERTEX, so, 0, "stage out psize", ins_ald(0, 0x80, 4), ins_ast(0, 0x6c, 1), ins_ast(0, 0x70, 4), ins_exit());
        CHECK(HAS(s2, "vsys[3] = as_type<float>(r0);"), "the point size is vsys.w");
        maxwell_free(s2);
        ShaderKey xb = xk;
        xb.xfb[2].offset = 8; /* 8 + 12 > stride 12 */
        XL_REFUSE(b1, STAGE_VERTEX, xb, 0, "bad transform feedback entry 2", "xfb entry past the stride", ins_ast(0, 0x70, 4), ins_exit());
        xb = xk;
        xb.xfb[0].buffer = 4;
        XL_REFUSE(b2, STAGE_VERTEX, xb, 0, "bad transform feedback entry 0", "xfb buffer out of range", ins_ast(0, 0x70, 4), ins_exit());
        xb = xk;
        xb.xfb_stride[0] = 0;
        XL_REFUSE(b3, STAGE_VERTEX, xb, 0, "bad transform feedback entry 0", "xfb stride 0", ins_ast(0, 0x70, 4), ins_exit());
        xb = xk;
        xb.xfb_count = 65;
        XL_REFUSE(b4, STAGE_VERTEX, xb, 0, "65 transform feedback entries", "too many xfb entries", ins_ast(0, 0x70, 4), ins_exit());
        xb = xk;
        XL_REFUSE(b5, STAGE_FRAGMENT, xb, 0xf, "transform feedback / stage-out requested for a fragment", "xfb on a fragment shader", ins_exit());
        /* no capture requested: the vertex shader is byte-identical to the old translation (no new arguments, no vsys) */
        XL_OK(n1, STAGE_VERTEX, z, 0, "no capture", ins_ald(0, 0x80, 4), ins_ast(0, 0x70, 4), ins_exit());
        CHECK(!HAS(n1, "xfb_ctl") && !HAS(n1, "stage_out") && !HAS(n1, "vsys"), "no capture, no plumbing");
        maxwell_free(n1);
    }
    /* ---- VABSDIFF4 (0x5000 class): layout A [8,16) B reg [20,28) / imm16 C [39,47) A signed 48 B signed 49 B-is-register 50 op [51,54) SAT 54 */
    {
#define VAD(d, a, b, c_, fl) (0x5000000000000000ull | (uint64_t)(d) | ((uint64_t)(a) << 8) | ((uint64_t)(b) << 20) | ((uint64_t)(c_) << 39) | (fl) | PT_BITS)
        XL_OK(v1, STAGE_FRAGMENT, z, 0xf, "VABSDIFF4", VAD(1, 2, 3, 4, 1ull << 50), ins_exit());
        CHECK(HAS(v1, "static inline uint vabsdiff4(") && HAS(v1, "r1 = vabsdiff4(r2, r3, r4, false, false, 0u, false);"), "VABSDIFF4: four packed byte differences");
        maxwell_free(v1);
        XL_OK(v2, STAGE_FRAGMENT, z, 0xf, "VABSDIFF4.ADD", VAD(1, 2, 3, 4, (1ull << 50) | (1ull << 48) | (1ull << 49) | (1ull << 51) | (1ull << 54)), ins_exit());
        CHECK(HAS(v2, "r1 = vabsdiff4(r2, r3, r4, true, true, 1u, true);"), "VABSDIFF4 signed lanes, ADD, SAT");
        maxwell_free(v2);
        XL_OK(v3, STAGE_FRAGMENT, z, 0xf, "VABSDIFF4.MAX imm", VAD(1, 2, 0x1234, 4, 3ull << 51), ins_exit());
        CHECK(HAS(v3, "r1 = vabsdiff4(r2, 0x1234u, r4, false, false, 3u, false);"), "VABSDIFF4 immediate B, MAX");
        maxwell_free(v3);
        XL_REFUSE(v4, STAGE_FRAGMENT, z, 0xf, "lane selector", "VABSDIFF4 with an A lane selector", VAD(1, 2, 3, 4, (1ull << 50) | (2ull << 36)), ins_exit());
        XL_REFUSE(v5, STAGE_FRAGMENT, z, 0xf, "combine op 5", "VABSDIFF4 reserved op", VAD(1, 2, 3, 4, (1ull << 50) | (5ull << 51)), ins_exit());
#undef VAD
    }
    /* ---- IMADSP: d = sel_a(Ra) * sel_b(B) + C; A selector [48,51), B selector [51,54) (0 U32, 1 S32, 2 U8, 3 S8, 4 U16, 5 S16) */
    {
#define IMS(cls, d, a, b, c_, as, bs, fl)                                                                                                                      \
    ((uint64_t)(cls) << 56 | (1ull << 55) | (uint64_t)(d) | ((uint64_t)(a) << 8) | ((uint64_t)(b) << 20) | ((uint64_t)(c_) << 39) | ((uint64_t)(as) << 48) |        \
     ((uint64_t)(bs) << 51) | (fl) | PT_BITS)
        XL_OK(m1, STAGE_FRAGMENT, z, 0xf, "IMADSP reg", IMS(0x5a, 1, 2, 3, 4, 2, 5, 0), ins_exit());
        CHECK(HAS(m1, "static inline uint isel(") && HAS(m1, "r1 = (isel(r2, 2u) * isel(r3, 5u) + r4);"), "IMADSP: U8 x S16 + C");
        maxwell_free(m1);
        XL_OK(m2, STAGE_FRAGMENT, z, 0xf, "IMADSP imm", IMS(0x34, 1, 2, 5, 4, 1, 0, 0), ins_exit());
        CHECK(HAS(m2, "r1 = (isel(r2, 1u) * isel(0x00000005u, 0u) + r4);"), "IMADSP immediate B");
        maxwell_free(m2);
        XL_OK(m3, STAGE_FRAGMENT, z, 0xf, "IMADSP cbuf", IMS(0x4a, 1, 2, 3, 4, 4, 3, 0), ins_exit());
        CHECK(HAS(m3, "isel(r2, 4u) * isel(cb0[0][3], 3u) + r4"), "IMADSP constant buffer B");
        maxwell_free(m3);
        XL_OK(m4, STAGE_FRAGMENT, z, 0xf, "IMADSP c-form", IMS(0x52, 1, 2, 5, 4, 0, 0, 0), ins_exit());
        CHECK(HAS(m4, "isel(r2, 0u) * isel(r4, 0u) + cb0["), "IMADSP 0x52: B = REG_39, C = constant buffer");
        maxwell_free(m4);
        XL_REFUSE(m5, STAGE_FRAGMENT, z, 0xf, "operand selector 6", "IMADSP reserved selector", IMS(0x5a, 1, 2, 3, 4, 6, 0, 0), ins_exit());
        XL_REFUSE(m6, STAGE_FRAGMENT, z, 0xf, "IMADSP carry/high", "IMADSP .HI", IMS(0x5a, 1, 2, 3, 4, 0, 0, 1ull << 54), ins_exit());
#undef IMS
    }
    /* ---- TXA: read like TXQ (index [36,49), selector [22,28), mask [31,35)) */
    {
        XL_OK(a1, STAGE_FRAGMENT, z, 0xf, "TXA", ins_tex2d_lz(0, 8, 9, 0), 0xdf40000000000000ull | (uint64_t)4 | (2ull << 8) | (1ull << 22) | (3ull << 31) | PT_BITS, ins_exit());
        CHECK(HAS(a1, "get_width(lv)") && HAS(a1, "r4 = q[0];") && HAS(a1, "r5 = q[1];"), "TXA dimension query");
        maxwell_free(a1);
        XL_REFUSE(a2, STAGE_FRAGMENT, z, 0xf, "txa type 3", "TXA other selector", ins_tex2d_lz(0, 8, 9, 0),
                  0xdf40000000000000ull | (uint64_t)4 | (2ull << 8) | (3ull << 22) | (3ull << 31) | PT_BITS, ins_exit());
        XL_REFUSE(a3, STAGE_FRAGMENT, z, 0xf, "txa type 2", "TXQ and TXA share the query", ins_tex2d_lz(0, 8, 9, 0),
                  0xdf48000000000000ull | (uint64_t)4 | (2ull << 8) | (2ull << 22) | (3ull << 31) | PT_BITS, ins_exit());
    }
    /* ---- TLD.MS (multisampled texel fetch) and sample shading */
    {
        uint64_t tldms = 0xdc38000000000000ull | (uint64_t)0 | (4ull << 8) | (6ull << 20) | (1ull << 29) | (0xfull << 31) | (8ull << 36) | (1ull << 50) | PT_BITS;
        XL_OK(ms1, STAGE_FRAGMENT, z, 0xf, "TLD.MS", ins_mov32i(4, 3), ins_mov32i(5, 3), ins_mov32i(6, 2), tldms, ins_exit());
        CHECK(HAS(ms1, "texture2d_ms<float> t0 [[texture(0)]]") && HAS(ms1, "get_num_samples() - 1u)"), "TLD.MS declares texture2d_ms and reads by sample index");
        CHECK(!HAS(ms1, "[[sample_id]]"), "no sample shading unless the key asks");
        maxwell_free(ms1);
        ShaderKey ssk = z;
        ssk.sample_shading = true;
        XL_OK(ms2, STAGE_FRAGMENT, ssk, 0xf, "sample shading", ins_mov32i(0, 0x3f800000), ins_exit());
        CHECK(HAS(ms2, "uint sample_id [[sample_id]]"), "ShaderKey.sample_shading declares [[sample_id]] (per-sample execution)");
        maxwell_free(ms2);
        XL_REFUSE(ms3, STAGE_FRAGMENT, z, 0xf, "tld.ms of a texture that is not 2D", "TLD.MS 3D", (tldms & ~(3ull << 29)) | (2ull << 29), ins_exit());
    }
    /* ---- capture-only vertex function: void, no return, pairs with rasterizationEnabled = NO */
    {
        ShaderKey cv = z;
        cv.stage_out = true;
        cv.capture_void = true;
        XL_OK(cv1, STAGE_VERTEX, cv, 0, "capture void", ins_ald(0, 0x80, 4), ins_ast(0, 0x70, 4), ins_exit());
        CHECK(HAS(cv1, "vertex void main0(") && !HAS(cv1, "return out;") && HAS(cv1, "stage_out[xo * 34u + 1] = out.pos;"), "capture_void: void vertex function that still writes its record");
        maxwell_free(cv1);
        ShaderKey cv2 = z;
        cv2.capture_void = true;
        XL_REFUSE(cv3, STAGE_VERTEX, cv2, 0, "capture_void is for", "capture_void without capture", ins_exit());
    }
#undef ST_ALD
#undef ST_AST
#undef ST_S2R
#undef ST_OUT
}
