/* -- surface instructions (SULD / SUST / SUATOM / SURED): translation text, image binding contract, refusals. Included by t_xlate.m inside main(). */
{
/* Like XL_OK, but a shader that needs MSL 3.1 (texture atomics) is compiled as 3.1, the version the runtime must use for it (needs_msl31). */
#define SU_OK(var, stage, key, label, ...)                                                                                                                     \
    uint64_t var##_i[] = {__VA_ARGS__};                                                                                                                        \
    ShaderInfo *var = xl_ins((stage), var##_i, (int)(sizeof var##_i / 8), (key), 0);                                                                           \
    CHECK(var->msl != NULL, label " translation: %s", var->error);                                                                                             \
    if (var->msl) {                                                                                                                                            \
        if (var->needs_msl31) MSL_COMPILES31(var->msl, label);                                                                                                 \
        else MSL_COMPILES(var->msl, label);                                                                                                                    \
    }
    ShaderKey sk0 = z;
    /* ---- SULD.D 32-bit, every dimension: Metal call shape, declaration, binding index */
    {
        static const struct {
            int dim;
            const char *decl, *call;
        } dims[] = {
            {SD_1D, "texture1d<float, access::read> im0 [[texture(64)]]", "im0.read(cx)"},
            {SD_BUFFER, "texture_buffer<float, access::read> im0 [[texture(64)]]", "im0.read(cx)"},
            {SD_1D_ARRAY, "texture1d_array<float, access::read> im0 [[texture(64)]]", "im0.read(cx, cy)"},
            {SD_2D, "texture2d<float, access::read> im0 [[texture(64)]]", "im0.read(uint2(cx, cy))"},
            {SD_2D_ARRAY, "texture2d_array<float, access::read> im0 [[texture(64)]]", "im0.read(uint2(cx, cy), cz)"},
            {SD_3D, "texture3d<float, access::read> im0 [[texture(64)]]", "im0.read(uint3(cx, cy, cz))"},
        };
        for (size_t i = 0; i < sizeof dims / sizeof dims[0]; i++) {
            SU_OK(sd, STAGE_COMPUTE, sk0, "SULD.D.32", i_suld(4, 8, dims[i].dim, 3, SZ_32, CL_IGN), ins_exit());
            CHECK(HAS(sd, dims[i].decl), "SULD dim %d declares %s", dims[i].dim, dims[i].decl);
            CHECK(HAS(sd, dims[i].call), "SULD dim %d reads with %s", dims[i].dim, dims[i].call);
            CHECK(sd->nimg == 1 && sd->img[0].handle_offset == 12 && !sd->img[0].bindless && sd->img[0].access == IMGACC_READ &&
                      sd->img[0].kind == IMGKIND_FLOAT,
                  "SULD dim %d: ShaderInfo.img[0] = handle c2+12, read, float", dims[i].dim);
            CHECK(HAS(sd, "r4 = as_type<uint>(t.x);") && !HAS(sd, "r5 ="), "SULD.32 writes one register");
            CHECK(sd->ntex == 0 && !sd->needs_msl31, "an image is not a sampled texture and needs no MSL 3.1");
            maxwell_free(sd);
        }
    }
    /* ---- sizes: sub-word raw access promotes the image to unsigned integer; 64 / 128 write 2 / 4 registers */
    {
        SU_OK(s8, STAGE_COMPUTE, sk0, "SULD.U8", i_suld(4, 8, SD_2D, 0, SZ_U8, CL_IGN), ins_exit());
        CHECK(HAS(s8, "texture2d<uint, access::read> im0") && HAS(s8, "uint4 t = ok ? im0.read(") && HAS(s8, "r4 = (t.x & 0xFFu);"),
              "SULD.U8: uint image, masked");
        maxwell_free(s8);
        SU_OK(s8s, STAGE_COMPUTE, sk0, "SULD.S8", i_suld(4, 8, SD_2D, 0, SZ_S8, CL_IGN), ins_exit());
        CHECK(HAS(s8s, "r4 = uint(int(char(t.x)));"), "SULD.S8 sign-extends");
        maxwell_free(s8s);
        SU_OK(s16, STAGE_COMPUTE, sk0, "SULD.U16", i_suld(4, 8, SD_2D, 0, SZ_U16, CL_IGN), ins_exit());
        CHECK(HAS(s16, "r4 = (t.x & 0xFFFFu);"), "SULD.U16 masks");
        maxwell_free(s16);
        SU_OK(s16s, STAGE_COMPUTE, sk0, "SULD.S16", i_suld(4, 8, SD_2D, 0, SZ_S16, CL_IGN), ins_exit());
        CHECK(HAS(s16s, "r4 = uint(int(short(t.x)));"), "SULD.S16 sign-extends");
        maxwell_free(s16s);
        ShaderKey ik = z;
        ik.image_int_mask = 1u << 0;
        SU_OK(s8i, STAGE_COMPUTE, ik, "SULD.S8 on a signed image", i_suld(4, 8, SD_2D, 0, SZ_S8, CL_IGN), ins_exit());
        CHECK(HAS(s8i, "texture2d<int, access::read> im0") && HAS(s8i, "uint(int(char(as_type<uint>(t.x))))"),
              "key image_int_mask: int4 texture, bits via as_type");
        maxwell_free(s8i);
        SU_OK(s64, STAGE_COMPUTE, sk0, "SULD.64", i_suld(4, 8, SD_2D, 0, SZ_64, CL_IGN), ins_exit());
        CHECK(HAS(s64, "r4 = as_type<uint>(t.x);") && HAS(s64, "r5 = as_type<uint>(t.y);") && !HAS(s64, "r6 ="), "SULD.64 writes two registers");
        maxwell_free(s64);
        SU_OK(s128, STAGE_COMPUTE, sk0, "SULD.128", i_suld(4, 8, SD_2D, 0, SZ_128, CL_IGN), ins_exit());
        CHECK(HAS(s128, "r4 = as_type<uint>(t.x);") && HAS(s128, "r7 = as_type<uint>(t.w);"), "SULD.128 writes four registers");
        maxwell_free(s128);
        XL_REFUSE(sres, STAGE_COMPUTE, sk0, 0, "reserved size 7", "SULD reserved size", i_suld(4, 8, SD_2D, 0, 7, CL_IGN), ins_exit());
        /* a float image already read as float cannot turn into a raw integer one half way */
        XL_REFUSE(smix, STAGE_COMPUTE, sk0, 0, "needs an integer image", "float then raw U8 on one image", i_suld(4, 8, SD_2D, 0, SZ_32, CL_IGN),
                  i_suld(5, 8, SD_2D, 0, SZ_U8, CL_IGN), ins_exit());
        /* a key that declares the image unsigned refuses the signed int atomics, and a key-declared signed image refuses unsigned min */
        ShaderKey uk = z;
        uk.image_uint_mask = 1u << 0;
        XL_REFUSE(skm, STAGE_COMPUTE, uk, 0, "signed integer image", "SUATOM.MIN.S32 on an image the key declares unsigned",
                  i_suatom(4, 8, 6, SA_MIN, ST_S32, SD_2D, 0, CL_IGN), ins_exit());
    }
    /* ---- SULD.P: channel masks compact into consecutive registers */
    {
        SU_OK(pa, STAGE_COMPUTE, sk0, "SULD.P RGBA", i_suld_p(4, 8, SD_2D, 0, 0xf, CL_IGN), ins_exit());
        CHECK(HAS(pa, "r4 = as_type<uint>(t.x);") && HAS(pa, "r5 = as_type<uint>(t.y);") && HAS(pa, "r6 = as_type<uint>(t.z);") &&
                  HAS(pa, "r7 = as_type<uint>(t.w);"),
              "SULD.P RGBA: four registers");
        maxwell_free(pa);
        SU_OK(pg, STAGE_COMPUTE, sk0, "SULD.P GA", i_suld_p(4, 8, SD_2D, 0, 0xa, CL_IGN), ins_exit());
        CHECK(HAS(pg, "r4 = as_type<uint>(t.y);") && HAS(pg, "r5 = as_type<uint>(t.w);") && !HAS(pg, "r6 ="), "SULD.P mask G|A: compacted into R4, R5");
        maxwell_free(pg);
        ShaderKey uk = z;
        uk.image_uint_mask = 1;
        SU_OK(pu, STAGE_COMPUTE, uk, "SULD.P on a uint image", i_suld_p(4, 8, SD_2D, 0, 0x1, CL_IGN), ins_exit());
        CHECK(HAS(pu, "r4 = t.x;") && HAS(pu, "texture2d<uint, access::read>"), "SULD.P integer image: raw channel");
        maxwell_free(pu);
        XL_REFUSE(pm0, STAGE_COMPUTE, sk0, 0, "empty channel mask", "SULD.P mask 0", i_suld_p(4, 8, SD_2D, 0, 0, CL_IGN), ins_exit());
    }
    /* ---- clamps and byte addressing */
    {
        SU_OK(ci, STAGE_COMPUTE, sk0, "SULD.IGN", i_suld(4, 8, SD_2D, 0, SZ_32, CL_IGN), ins_exit());
        CHECK(HAS(ci, "bool ok = cx < im0.get_width() && cy < im0.get_height();") && HAS(ci, "ok ? im0.read(uint2(cx, cy)) : float4(0)"),
              "IGN: out of range reads zero");
        maxwell_free(ci);
        SU_OK(ct, STAGE_COMPUTE, sk0, "SULD.TRAP", i_suld(4, 8, SD_2D, 0, SZ_32, CL_TRAP), ins_exit());
        CHECK(HAS(ct, "bool ok = cx < im0.get_width()"), "TRAP behaves as IGN (no trap on the GPU)");
        maxwell_free(ct);
        SU_OK(cc, STAGE_COMPUTE, sk0, "SULD.CLAMP", i_suld(4, 8, SD_2D_ARRAY, 0, SZ_32, CL_CLAMP), ins_exit());
        CHECK(HAS(cc, "cx = min(cx, im0.get_width() - 1u);") && HAS(cc, "cy = min(cy, im0.get_height() - 1u);") &&
                  HAS(cc, "cz = min(cz, im0.get_array_size() - 1u);") && HAS(cc, "bool ok = true;"),
              "CLAMP clamps each coordinate (and the layer)");
        maxwell_free(cc);
        SU_OK(cz, STAGE_COMPUTE, sk0, "SULD.ZERO", i_suld(4, 8, SD_3D, 0, SZ_32, CL_ZERO), ins_exit());
        CHECK(HAS(cz, "cz < im0.get_depth()"), "ZERO checks all three extents");
        maxwell_free(cz);
        SU_OK(ba, STAGE_COMPUTE, sk0, "SULD.BA", i_suld_ba(4, 8, SD_1D, 0, SZ_64, CL_IGN), ins_exit());
        CHECK(HAS(ba, "cx >>= 3;"), "byte addressing divides x by the access size");
        maxwell_free(ba);
        SU_OK(bab, STAGE_COMPUTE, sk0, "SULD.BA.U8", i_suld_ba(4, 8, SD_BUFFER, 0, SZ_U8, CL_IGN), ins_exit());
        CHECK(!HAS(bab, "cx >>="), "byte addressing of bytes needs no shift");
        maxwell_free(bab);
    }
    /* ---- SUST */
    {
        SU_OK(w32, STAGE_COMPUTE, sk0, "SUST.D.32", i_sust(4, 8, SD_2D, 1, SZ_32, CL_IGN), ins_exit());
        CHECK(HAS(w32, "texture2d<float, access::write> im0 [[texture(64)]]") &&
                  HAS(w32, "if (ok) im0.write(float4(as_type<float>(r4), 0.0f, 0.0f, 1.0f), uint2(cx, cy));"),
              "SUST.D.32: write-only float image");
        CHECK(w32->img[0].access == IMGACC_WRITE, "access write");
        maxwell_free(w32);
        SU_OK(w128, STAGE_COMPUTE, sk0, "SUST.D.128", i_sust(4, 8, SD_3D, 1, SZ_128, CL_IGN), ins_exit());
        CHECK(HAS(w128, "float4(as_type<float>(r4), as_type<float>(r5), as_type<float>(r6), as_type<float>(r7)), uint3(cx, cy, cz)"),
              "SUST.128 reads four registers");
        maxwell_free(w128);
        SU_OK(wb, STAGE_COMPUTE, sk0, "SUST.D.U8", i_sust(4, 8, SD_2D_ARRAY, 1, SZ_U8, CL_IGN), ins_exit());
        CHECK(HAS(wb, "texture2d_array<uint, access::write>") && HAS(wb, "uint4((r4 & 0xFFu), 0, 0, 1), uint2(cx, cy), cz)"),
              "SUST.U8: masked store into a uint image");
        maxwell_free(wb);
        ShaderKey ik = z;
        ik.image_int_mask = 1u << 1;
        SU_OK(ws, STAGE_COMPUTE, ik, "SUST.D.S16", i_sust(4, 8, SD_2D, 1, SZ_S16, CL_IGN), ins_exit());
        CHECK(HAS(ws, "texture2d<int, access::write>") && HAS(ws, "int4(int(short(r4)), 0, 0, 1)"), "SUST.S16 into a signed image");
        maxwell_free(ws);
        SU_OK(wp, STAGE_COMPUTE, sk0, "SUST.P RGBA", i_sust_p(4, 8, SD_2D, 1, 0xf, CL_IGN), ins_exit());
        CHECK(HAS(wp, "im0.write(float4(as_type<float>(r4), as_type<float>(r5), as_type<float>(r6), as_type<float>(r7)), uint2(cx, cy))") &&
                  wp->img[0].access == IMGACC_WRITE,
              "SUST.P full mask: plain write");
        maxwell_free(wp);
        SU_OK(wq, STAGE_COMPUTE, sk0, "SUST.P partial", i_sust_p(4, 8, SD_2D, 1, 0x5, CL_IGN), ins_exit());
        CHECK(HAS(wq, "texture2d<float, access::read_write>") && HAS(wq, "t.x = as_type<float>(r4);") && HAS(wq, "t.z = as_type<float>(r5);") &&
                  !HAS(wq, "t.y ="),
              "SUST.P mask R|B: read-modify-write, other channels untouched");
        CHECK(wq->img[0].access == (IMGACC_READ | IMGACC_WRITE), "access read_write");
        maxwell_free(wq);
        SU_OK(wl, STAGE_COMPUTE, sk0, "SUST.D.1D_ARRAY", i_sust(4, 8, SD_1D_ARRAY, 1, SZ_32, CL_IGN), ins_exit());
        CHECK(HAS(wl, ", cx, cy);") || HAS(wl, "cx, cy)"), "1D array: x then layer");
        maxwell_free(wl);
        SU_OK(wf, STAGE_FRAGMENT, sk0, "SUST in a fragment shader", i_sust(4, 8, SD_2D, 1, SZ_32, CL_IGN), ins_exit());
        CHECK(HAS(wf, "fragment FOut main0(") && HAS(wf, "im0 [[texture(64)]]"), "fragment shaders take images after the textures");
        maxwell_free(wf);
    }
    /* ---- one shader, several images and a read/write pair */
    {
        SU_OK(mi, STAGE_COMPUTE, sk0, "two images", i_suld(4, 8, SD_2D, 2, SZ_32, CL_IGN), i_sust(4, 8, SD_2D, 3, SZ_32, CL_IGN),
              i_suld(5, 8, SD_2D, 2, SZ_32, CL_IGN), i_sust(5, 8, SD_2D, 2, SZ_32, CL_IGN), ins_exit());
        CHECK(mi->nimg == 2 && mi->img[0].handle_offset == 8 && mi->img[1].handle_offset == 12, "images numbered by first use, the same handle shares a slot");
        CHECK(mi->img[0].access == (IMGACC_READ | IMGACC_WRITE) && mi->img[1].access == IMGACC_WRITE, "access merged per image");
        CHECK(HAS(mi, "access::read_write> im0 [[texture(64)]]") && HAS(mi, "access::write> im1 [[texture(65)]]"), "declarations");
        maxwell_free(mi);
        XL_REFUSE(md, STAGE_COMPUTE, sk0, 0, "used as 2D and as 3D", "one image as two dimensions", i_suld(4, 8, SD_2D, 2, SZ_32, CL_IGN),
                  i_suld(5, 8, SD_3D, 2, SZ_32, CL_IGN), ins_exit());
        XL_REFUSE(mr, STAGE_COMPUTE, sk0, 0, "reserved dimension", "reserved dimension 6", i_su(0xeb0, 4, 8, 0, 0, 0, SZ_32, 0, 0, 0, 6), ins_exit());
        /* too many images */
        uint64_t many[12];
        for (int i = 0; i < 9; i++) many[i] = i_suld(4, 8, SD_2D, i, SZ_32, CL_IGN);
        many[9] = ins_exit();
        ShaderInfo *mm = xl_ins(STAGE_COMPUTE, many, 10, sk0, 0);
        CHECK(REFUSED(mm, "more than 8 distinct surface images"), "the ninth image is refused: %s", mm->error);
        maxwell_free(mm);
    }
    /* ---- bindless handles */
    {
        SU_OK(bl, STAGE_COMPUTE, sk0, "SULD.B", i_mov_cb(6, 4, 0x40), i_suld_b(4, 8, 6, SD_2D, SZ_32, CL_IGN), i_sust_b(4, 8, 6, SD_2D, SZ_32, CL_IGN),
              ins_exit());
        CHECK(bl->nimg == 1 && bl->img[0].bindless && bl->img[0].bindless_cbuf == 4 && bl->img[0].bindless_offset == 0x40,
              "bindless image: handle from c4+0x40");
        CHECK(bl->img[0].access == (IMGACC_READ | IMGACC_WRITE) && HAS(bl, "im0 [[texture(64)]]"), "SULD.B and SUST.B share the slot");
        maxwell_free(bl);
        SU_OK(bc2, STAGE_COMPUTE, sk0, "SULD.B from c2", i_mov_cb(6, 2, 0x130), i_suld_b(4, 8, 6, SD_2D, SZ_32, CL_IGN), ins_exit());
        CHECK(bc2->nimg == 1 && !bc2->img[0].bindless && bc2->img[0].handle_offset == 0x130, "a bindless handle read from c2 is an ordinary table entry");
        maxwell_free(bc2);
        XL_REFUSE(bn, STAGE_COMPUTE, sk0, 0, "not a constant buffer load", "SULD.B with a computed handle", i_suld_b(4, 8, 9, SD_2D, SZ_32, CL_IGN),
                  ins_exit());
    }
    /* ---- atomics */
    {
        static const struct {
            int op, type;
            const char *fn, *kind, *bits;
        } ops[] = {
            {SA_ADD, ST_U32, "atomic_fetch_add", "uint", "r4 = old.x;"},
            {SA_MIN, ST_U32, "atomic_fetch_min", "uint", "r4 = old.x;"},
            {SA_MAX, ST_U32, "atomic_fetch_max", "uint", "r4 = old.x;"},
            {SA_AND, ST_U32, "atomic_fetch_and", "uint", "r4 = old.x;"},
            {SA_OR, ST_U32, "atomic_fetch_or", "uint", "r4 = old.x;"},
            {SA_XOR, ST_U32, "atomic_fetch_xor", "uint", "r4 = old.x;"},
            {SA_MIN, ST_S32, "atomic_fetch_min", "int", "r4 = as_type<uint>(old.x);"},
            {SA_MAX, ST_S32, "atomic_fetch_max", "int", "r4 = as_type<uint>(old.x);"},
            {SA_ADD, ST_S32, "atomic_fetch_add", "uint", "r4 = old.x;"},
        };
        for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++) {
            uint64_t ins[] = {i_suatom(4, 8, 6, ops[i].op, ops[i].type, SD_2D, 5, CL_IGN), ins_exit()};
            ShaderInfo *si = xl_ins(STAGE_COMPUTE, ins, 2, sk0, 0);
            CHECK(si->msl != NULL, "SUATOM op %d type %d: %s", ops[i].op, ops[i].type, si->error);
            if (!si->msl) {
                maxwell_free(si);
                continue;
            }
            char want[160];
            snprintf(want, sizeof want, "im0.%s(uint2(cx, cy), ", ops[i].fn);
            CHECK(HAS(si, want), "SUATOM op %d: %s", ops[i].op, want);
            snprintf(want, sizeof want, "texture2d<%s, access::read_write> im0 [[texture(64)]]", ops[i].kind);
            CHECK(HAS(si, want), "SUATOM op %d declares %s", ops[i].op, want);
            CHECK(HAS(si, ops[i].bits), "SUATOM op %d returns the old value: %s", ops[i].op, ops[i].bits);
            CHECK(si->needs_msl31 && si->img[0].atomic && si->img[0].access == (IMGACC_READ | IMGACC_WRITE), "SUATOM op %d: MSL 3.1, atomic image, read_write",
                  ops[i].op);
            MSL_COMPILES31(si->msl, "SUATOM");
            maxwell_free(si);
        }
        SU_OK(rd, STAGE_COMPUTE, sk0, "SURED.ADD", i_sured(6, 8, 6, SA_ADD, ST_U32, SD_3D, 0, CL_IGN), ins_exit());
        CHECK(HAS(rd, "im0.atomic_fetch_add(uint3(cx, cy, cz), uint4(r6))") && HAS(rd, "(void)old;") && !HAS(rd, "r6 = old"),
              "SURED discards the old value, operand from the Rd field");
        MSL_COMPILES31(rd->msl, "SURED");
        maxwell_free(rd);
        SU_OK(rl, STAGE_COMPUTE, sk0, "SURED.OR 1D array", i_sured(6, 8, 6, SA_OR, ST_U32, SD_1D_ARRAY, 0, CL_CLAMP), ins_exit());
        CHECK(HAS(rl, "im0.atomic_fetch_or(cx, cy, uint4(r6))") && HAS(rl, "cy = min(cy, im0.get_array_size() - 1u);"), "SURED on a 1D array with CLAMP");
        maxwell_free(rl);
        SU_OK(rb, STAGE_COMPUTE, sk0, "SUATOM bindless", i_mov_cb(9, 3, 0x10), i_su(0xea1, 4, 8, 9, 0, 0, SA_ADD, 0, ST_U32, 0, SD_2D) | (6ull << 36),
              ins_exit());
        CHECK(rb->nimg == 1 && rb->img[0].bindless && HAS(rb, "uint4(r6)"), "SUATOM.B: operand register in bits 36-43");
        maxwell_free(rb);
        SU_OK(ra, STAGE_FRAGMENT, sk0, "SUATOM in a fragment shader", i_suatom(4, 8, 6, SA_ADD, ST_U32, SD_BUFFER, 0, CL_IGN), ins_exit());
        CHECK(HAS(ra, "texture_buffer<uint, access::read_write> im0"), "buffer image atomics");
        maxwell_free(ra);
        /* unsupported by Metal, each named with the reason */
        XL_REFUSE(ai, STAGE_COMPUTE, sk0, 0, "SUATOM.INC.2D: INC (wrapping increment)", "SUATOM.INC", i_suatom(4, 8, 6, SA_INC, ST_U32, SD_2D, 0, CL_IGN),
                  ins_exit());
        XL_REFUSE(ad, STAGE_COMPUTE, sk0, 0, "SUATOM.DEC.2D: DEC (wrapping decrement)", "SUATOM.DEC", i_suatom(4, 8, 6, SA_DEC, ST_U32, SD_2D, 0, CL_IGN),
                  ins_exit());
        XL_REFUSE(ae, STAGE_COMPUTE, sk0, 0, "SUATOM.EXCH.2D: EXCH has no Metal texture atomic", "SUATOM.EXCH",
                  i_suatom(4, 8, 6, SA_EXCH, ST_U32, SD_2D, 0, CL_IGN), ins_exit());
        XL_REFUSE(ac, STAGE_COMPUTE, sk0, 0, "SUATOM.CAS.2D: CAS has no Metal texture atomic", "SUATOM.CAS",
                  i_suatom(4, 8, 6, SA_CAS, ST_U32, SD_2D, 0, CL_IGN), ins_exit());
        XL_REFUSE(a6, STAGE_COMPUTE, sk0, 0, "64-bit surface atomics", "SUATOM.ADD.U64", i_suatom(4, 8, 6, SA_ADD, ST_U64, SD_2D, 0, CL_IGN), ins_exit());
        XL_REFUSE(a7, STAGE_COMPUTE, sk0, 0, "64-bit surface atomics", "SUATOM.ADD.S64", i_suatom(4, 8, 6, SA_ADD, ST_S64, SD_2D, 0, CL_IGN), ins_exit());
        XL_REFUSE(af, STAGE_COMPUTE, sk0, 0, "floating-point surface atomics", "SUATOM.ADD.F32", i_suatom(4, 8, 6, SA_ADD, ST_F32, SD_2D, 0, CL_IGN),
                  ins_exit());
        XL_REFUSE(sr, STAGE_COMPUTE, sk0, 0, "SURED.CAS.2D: CAS", "SURED.CAS", i_sured(6, 8, 6, SA_CAS, ST_U32, SD_2D, 0, CL_IGN), ins_exit());
        /* an image already read as float is refused as an atomic target; one used as an atomic first is integer from then on */
        XL_REFUSE(af2, STAGE_COMPUTE, sk0, 0, "needs an integer image", "atomic on an image already read as float", i_suld(4, 8, SD_2D, 0, SZ_32, CL_IGN),
                  i_sured(6, 8, 6, SA_ADD, ST_U32, SD_2D, 0, CL_IGN), ins_exit());
        SU_OK(ai2, STAGE_COMPUTE, sk0, "atomic then load", i_sured(6, 8, 6, SA_ADD, ST_U32, SD_2D, 0, CL_IGN), i_suld(4, 8, SD_2D, 0, SZ_32, CL_IGN),
              ins_exit());
        CHECK(HAS(ai2, "texture2d<uint, access::read_write> im0") && HAS(ai2, "r4 = t.x;"), "an atomic image is a uint image for later loads too");
        maxwell_free(ai2);
    }
    /* ---- other opcodes in the 0xea / 0xeb ranges stay refused and named */
    XL_REFUSE(ou, STAGE_COMPUTE, sk0, 0, "surface access (opcode eb8", "unknown 0xeb8 variant", 0xeb80000000000000ull | PT_BITS, ins_exit());
    XL_REFUSE(ou2, STAGE_COMPUTE, sk0, 0, "surface access (opcode ea4", "SUATOM.CAS-class 0xea4", 0xea40000000000000ull | PT_BITS, ins_exit());
#undef SU_OK
}
#include "t_xlate_stages.inc"
#include "t_xlate_rare.inc"
