/* -- the rare control/system ops, IDP and stencil texture bindings. Included by t_surface.inc inside main(), after t_xlate_stages.inc.
 * Opcode bases and field layouts are the ones the translator decodes (see the comments at each branch in maxwell_translate.c); the tests pin that reading.
 * No application shader emits the control/system ops, so every case is a text assertion on the generated MSL (+ the Metal compile check on a Mac). */
{
#define RV(base, d, a, b) ((uint64_t)(base) | (uint64_t)(d) | ((uint64_t)(a) << 8) | ((uint64_t)(b) << 20) | PT_BITS)
    /* ---- documented no-ops: BPT IDE RAM SAM STP R2B translate and leave the surrounding code alone */
    {
        struct {
            const char *name;
            uint64_t w;
        } nops[] = {{"BPT", RV(0xe3a0000000000000ull, 0, 0, 0)},   {"IDE", RV(0xe390000000000000ull, 0, 0, 0)}, {"RAM", RV(0xe380000000000000ull, 0, 0, 0)},
                    {"SAM", RV(0xe370000000000000ull, 0, 0, 0)},   {"STP", RV(0xeea0000000000000ull, 0, 2, 0)}, {"R2B", RV(0xf0c0000000000000ull, 0, 2, 0)}};
        for (size_t i = 0; i < sizeof nops / sizeof *nops; i++) {
            XL_OK(n1, STAGE_FRAGMENT, z, 0xf, "no-op control op", ins_mov32i(1, 0x1234), nops[i].w, ins_mov32i(2, 0x5678), ins_exit());
            CHECK(HAS(n1, "r1 = 0x00001234u;") && HAS(n1, "r2 = 0x00005678u;") && !HAS(n1, "pc = ") && !HAS(n1, "exit_now"), "%s is a no-op in the generated code", nops[i].name);
            maxwell_free(n1);
        }
    }
    /* ---- B2R reads the (untracked) barrier state as 0 */
    {
        XL_OK(b1, STAGE_FRAGMENT, z, 0xf, "B2R", RV(0xf0b8000000000000ull, 5, 2, 0), ins_exit());
        CHECK(HAS(b1, "r5 = 0u;"), "B2R writes 0 to Rd");
        maxwell_free(b1);
    }
    /* ---- RTT ends the thread like EXIT; a predicated RTT only on its guard */
    {
        XL_OK(r1, STAGE_FRAGMENT, z, 0xf, "RTT", ins_mov32i(1, 7), RV(0xe360000000000000ull, 0, 0, 0), ins_mov32i(2, 8), ins_exit());
        CHECK(HAS(r1, "r1 = 0x00000007u;\n    break;\n"), "RTT: break out of the program");
        maxwell_free(r1);
    }
    /* ---- PEXIT: EXIT jumps to the pushed target once; the target is a label */
    {
        XL_OK(p1, STAGE_FRAGMENT, z, 0xf, "PEXIT", RV(0xe230000000000000ull, 0, 0, 0) | ((uint64_t)(2 * 8) << 20), ins_exit(), ins_mov32i(1, 9), ins_exit());
        CHECK(HAS(p1, "uint pex = 0xFFFFFFFFu;") && HAS(p1, "pex = 40u;"), "PEXIT stores the relative target (declared once)");
        CHECK(HAS(p1, "if (pex != 0xFFFFFFFFu) { pc = pex; pex = 0xFFFFFFFFu; continue; }"), "EXIT takes the PEXIT target and clears it");
        CHECK(HAS(p1, "case 40u:"), "the PEXIT target is a label");
        maxwell_free(p1);
        XL_OK(p2, STAGE_FRAGMENT, z, 0xf, "EXIT without PEXIT", ins_mov32i(1, 9), ins_exit());
        CHECK(!HAS(p2, "pex"), "a program without PEXIT carries no PEXIT state");
        maxwell_free(p2);
    }
    /* ---- PLONGJMP / LONGJMP: a second PBK/BRK-style stack */
    {
        XL_OK(j1, STAGE_FRAGMENT, z, 0xf, "PLONGJMP/LONGJMP", RV(0xe280000000000000ull, 0, 0, 0) | ((uint64_t)(2 * 8) << 20), RV(0xe310000000000000ull, 0, 0, 0),
              ins_mov32i(1, 9), ins_exit());
        CHECK(HAS(j1, "uint ljs[16]; uint ljs_n = 0;") && HAS(j1, "ljs[ljs_n++] = 40u;") && HAS(j1, "pc = ljs[--ljs_n]; continue;"), "PLONGJMP pushes, LONGJMP pops and jumps");
        CHECK(HAS(j1, "case 40u:"), "the PLONGJMP target is a label");
        maxwell_free(j1);
    }
    /* ---- GETCRSPTR / SETCRSPTR: the CAL depth, readable and settable */
    {
        XL_OK(c1, STAGE_FRAGMENT, z, 0xf, "GETCRSPTR/SETCRSPTR", RV(0xe2c0000000000000ull, 3, 0, 0), RV(0xe2e0000000000000ull, 0, 4, 0), ins_exit());
        CHECK(HAS(c1, "r3 = cal_n;") && HAS(c1, "cal_n = min(r4, 16u);") && HAS(c1, "uint cal[16]; uint cal_n = 0;"), "call-return stack pointer read and write, state machine declared");
        maxwell_free(c1);
    }
    /* ---- GETLMEMBASE / SETLMEMBASE */
    {
        XL_OK(l1, STAGE_FRAGMENT, z, 0xf, "GETLMEMBASE/SETLMEMBASE", RV(0xe2f0000000000000ull, 0, 4, 0), RV(0xe2d0000000000000ull, 3, 0, 0), ins_exit());
        CHECK(HAS(l1, "uint lmb = 0u;") && HAS(l1, "lmb = r4;") && HAS(l1, "r3 = lmb;"), "local memory base set and read back");
        maxwell_free(l1);
    }
    /* ---- ISBERD: register-addressed attribute read, shares the ALD path (vertex, fragment) */
    {
        ShaderKey vk = z;
        XL_OK(i1, STAGE_VERTEX, vk, 0, "ISBERD vertex", ins_ald(8, 0x80, 2), RV(0xefd0000000000000ull, 4, 2, 0x10), ins_exit());
        CHECK(HAS(i1, "{ uint ix = r2;") && HAS(i1, "vattr_arr[(((ix + 16u)) - 0x80u) >> 2]"), "ISBERD in a vertex shader reads the attribute table at Ra + imm");
        maxwell_free(i1);
        XL_OK(i2, STAGE_FRAGMENT, z, 0xf, "ISBERD fragment", RV(0xefd0000000000000ull, 4, 2, 0x10), ins_exit());
        CHECK(HAS(i2, "attr_arr[min(((ix + 16u) - 0x80u) >> 2, 127u)]"), "ISBERD in a fragment shader reads the varying table");
        maxwell_free(i2);
    }
    /* ---- IDP: 4-byte / 2-half dot product with accumulate; signedness per operand */
    {
#define IDPW(base, d, a, b, c_, fl) ((uint64_t)(base) | (uint64_t)(d) | ((uint64_t)(a) << 8) | ((uint64_t)(b) << 20) | ((uint64_t)(c_) << 39) | (fl) | PT_BITS)
        XL_OK(d1, STAGE_FRAGMENT, z, 0xf, "IDP.4A", IDPW(0x53f8000000000000ull, 1, 2, 3, 4, 0), IDPW(0x53f8000000000000ull, 5, 2, 3, 4, (1ull << 48) | (1ull << 49)),
              IDPW(0x53d8000000000000ull, 6, 2, 3, 4, 1ull << 47), ins_exit());
        CHECK(HAS(d1, "static inline uint idp(uint a, uint b, uint c, bool asg, bool bsg, bool two, bool hi)"), "IDP helper emitted once");
        CHECK(HAS(d1, "r1 = idp(r2, r3, r4, false, false, false, false);"), "IDP.4A unsigned");
        CHECK(HAS(d1, "r5 = idp(r2, r3, r4, true, true, false, false);"), "IDP.4A signed x signed");
        CHECK(HAS(d1, "r6 = idp(r2, r3, r4, false, false, true, true);"), "IDP.2A, high pair of B");
        maxwell_free(d1);
#undef IDPW
    }
    /* ---- stencil texture bindings: ShaderKey.stencil_slots -> a uint texture + the StencilTex wrapper, integer in .r */
    {
        ShaderKey sk = z;
        sk.stencil_slots = 1;
        XL_OK(s1, STAGE_FRAGMENT, sk, 0xf, "stencil sample", ins_mov32i(4, 0x3e800000), ins_mov32i(5, 0x3f400000), ins_tex2d_lz(0, 4, 4, 8), ins_exit());
        CHECK(s1->ntex == 1 && s1->tex[0].stencil && !s1->tex[0].depth && s1->tex[0].handle_offset == 0x20, "ShaderInfo.tex[0].stencil names the binding");
        CHECK(HAS(s1, ", texture2d<uint> t0_u [[texture(0)]]") && !HAS(s1, "depth2d") && !HAS(s1, "texture2d<float> t0"), "declared as a uint texture");
        CHECK(HAS(s1, "const StencilTex2D t0 = {t0_u};") && HAS(s1, "struct StencilTex2D {") && HAS(s1, "static inline float4 stx(uint v)"), "wrapper local named like the float texture");
        CHECK(HAS(s1, "t0.sample(s0,"), "the sample call is unchanged: the wrapper reads the integer");
        maxwell_free(s1);
        sk.depth_slots = 1;
        XL_OK(s2, STAGE_FRAGMENT, sk, 0xf, "stencil beats depth", ins_tex2d_lz(0, 4, 4, 8), ins_exit());
        CHECK(s2->tex[0].stencil && !s2->tex[0].depth && !HAS(s2, "depth2d"), "a slot in both masks is a stencil binding");
        maxwell_free(s2);
        sk.depth_slots = 0;
        XL_OK(s3, STAGE_FRAGMENT, sk, 0xf, "stencil texel fetch", i_tld(0, 8, 12, 8, 1, 0, 0xf, 0, 0, 1), ins_exit());
        CHECK(HAS(s3, "float4 t = t0.read(rsc(uint2(uint(int(r8)), uint(int(r9))), rscale[1]), uint(r12));") && HAS(s3, "texture2d<uint> t0_u"), "TLD of a stencil view uses the wrapper's integer read");
        maxwell_free(s3);
        XL_OK(s4, STAGE_FRAGMENT, sk, 0xf, "stencil 2D array", i_tld(0, 8, 12, 8, 1, 1, 0xf, 0, 0, 1), ins_exit());
        CHECK(HAS(s4, "texture2d_array<uint> t0_u") && HAS(s4, "const StencilTex2DArray t0 = {t0_u};") && HAS(s4, "struct StencilTex2DArray {"), "2D array stencil view");
        maxwell_free(s4);
        ShaderKey nk = z;
        XL_OK(s5, STAGE_FRAGMENT, nk, 0xf, "float binding unchanged", ins_tex2d_lz(0, 4, 4, 8), ins_exit());
        CHECK(!s5->tex[0].stencil && HAS(s5, "texture2d<float> t0") && !HAS(s5, "StencilTex"), "no stencil key bit, no wrapper");
        maxwell_free(s5);
        /* TEXS.F16 (bit 59 clear) of a stencil view: the integer would be packed as half precision, refused */
        uint64_t texs16 = 0xd000000000000000ull | 4ull | (4ull << 8) | (4ull << 20) | (255ull << 28) | (8ull << 36) | (1ull << 53) | PT_BITS;
        XL_REFUSE(s7, STAGE_FRAGMENT, sk, 0xf, "stencil texture binding", "stencil TEXS.F16", texs16, ins_exit());
        XL_REFUSE(s6, STAGE_FRAGMENT, sk, 0xf, "stencil sampling of a texture of type", "stencil 3D", i_tld(0, 8, 12, 8, 2, 0, 0xf, 0, 0, 1), ins_exit());
    }
#undef RV
}
