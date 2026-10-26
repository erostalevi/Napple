/* surface.inc - Maxwell surface instructions (SULD, SUST, SUATOM, SURED) -> MSL texture read / write / atomic calls.
 * Included by maxwell_translate.c after the texture helpers; uses its Ctx, fail(), bits(), reg(), bindless_index().
 *
 * Instruction layout (the translator's decode; opcode = bits 52-63, bit 52 = bindless form):
 *   0xeb0 SULD   0xeb2 SUST   0xeb4 SURED   0xea0 SUATOM   (odd = ".B": the image handle is the c-buffer value last loaded into Rb)
 *   Rd 0-7   (SULD destination, SUST/SURED data source, SUATOM old value)      Ra 8-15  first coordinate register (x, y, z-or-layer, consecutive)
 *   Rb 20-27 (bindless handle, else SUATOM operand; SURED takes its operand from Rd)    Rc 36-43 (SUATOM operand of the bindless form)
 *   SULD/SUST: BA 26 (byte addressing), size 28-30 (0 U8, 1 S8, 2 U16, 3 S16, 4 B32, 5 B64, 6 B128), P 31 (formatted: channel mask 32-35 instead of the size)
 *   SUATOM/SURED: op 28-31 (as ATOM: 0 ADD 1 MIN 2 MAX 3 INC 4 DEC 5 AND 6 OR 7 XOR 8 EXCH 9 CAS), type 32-34 (0 U32, 1 S32, 2 U64, 3 F32, 4 S64)
 *   image index 36-46 (non-bindless; handle at c2 + 4 * index), clamp 47-48 (0 IGN, 1 TRAP, 2 CLAMP, 3 ZERO), dimension 49-51 (0 1D, 1 BUFFER, 2 1D_ARRAY, 3 2D, 4 2D_ARRAY, 5 3D)
 * Status: tested against hand-assembled words only (t_xlate); the field positions follow the public Maxwell decoders from memory and have not been checked
 * against a shader binary from a game. */

enum { SF_SULD, SF_SUST, SF_SURED, SF_SUATOM };

static const char *const sf_dim_name[8] = {"1D", "BUFFER", "1D_ARRAY", "2D", "2D_ARRAY", "3D", "?6", "?7"};
static const int sf_dim_type[8] = {TEXTYPE_1D, TEXTYPE_BUFFER, TEXTYPE_1D_ARRAY, TEXTYPE_2D, TEXTYPE_2D_ARRAY, TEXTYPE_3D, -1, -1};
static const int sf_dim_nsp[8] = {1, 1, 1, 2, 2, 3, 0, 0};   // spatial coordinates
static const int sf_dim_arr[8] = {0, 0, 1, 0, 1, 0, 0, 0};   // array layer coordinate (follows the spatial ones)
static const char *const sf_size_name[8] = {"U8", "S8", "U16", "S16", "32", "64", "128", "?7"};
static const char *const sf_atom_name[16] = {"ADD", "MIN", "MAX", "INC", "DEC", "AND", "OR", "XOR", "EXCH", "CAS", "?10", "?11", "?12", "?13", "?14", "?15"};

static const char *img_msl_type(int type) {
    switch (type) {
        case TEXTYPE_1D: return "texture1d";
        case TEXTYPE_1D_ARRAY: return "texture1d_array";
        case TEXTYPE_3D: return "texture3d";
        case TEXTYPE_2D_ARRAY: return "texture2d_array";
        case TEXTYPE_BUFFER: return "texture_buffer";
        default: return "texture2d";
    }
}

static const char *img_dim_name(int type) {
    return sf_dim_name[type == TEXTYPE_1D         ? 0
                       : type == TEXTYPE_BUFFER   ? 1
                       : type == TEXTYPE_1D_ARRAY ? 2
                       : type == TEXTYPE_2D       ? 3
                       : type == TEXTYPE_2D_ARRAY ? 4
                                                  : 5];
}

// Find or create the image slot for a handle. `bindless_pseudo` >= 8192 = user uniform buffer entry (bindless_index()), else the c2 byte offset / 4.
static int img_slot(Ctx *c, uint32_t index, int type, int want_int, const char *what) {
    ShaderInfo *info = c->info;
    uint32_t bit = index >= 8192 ? 31u : (index < 31 ? index : 31u);
    int s = -1;
    for (int i = 0; i < info->nimg; i++) {
        const ShaderImage *m = &info->img[i];
        if (index >= 8192 ? (m->bindless && m->bindless_cbuf == (uint32_t)c->bl_cb[index - 8192] && m->bindless_offset == (uint32_t)c->bl_off[index - 8192])
                          : (!m->bindless && m->handle_offset == index * 4)) {
            s = i;
            break;
        }
    }
    if (s < 0) {
        if (info->nimg >= MAX_IMAGES) {
            fail(c, "%s: more than %d distinct surface images", what, MAX_IMAGES);
            return -1;
        }
        s = info->nimg++;
        ShaderImage *m = &info->img[s];
        m->type = (uint8_t)type;
        if (index >= 8192) {
            m->bindless = true;
            m->bindless_cbuf = (uint32_t)c->bl_cb[index - 8192];
            m->bindless_offset = (uint32_t)c->bl_off[index - 8192];
        } else m->handle_offset = index * 4;
        if (c->key && ((c->key->image_int_mask >> bit) & 1)) m->kind = IMGKIND_INT, m->kind_fixed = true;
        else if (c->key && ((c->key->image_uint_mask >> bit) & 1)) m->kind = IMGKIND_UINT, m->kind_fixed = true;
    }
    ShaderImage *m = &info->img[s];
    if (m->type != type) {
        fail(c, "%s: image used as %s and as %s in one shader", what, img_dim_name(m->type), img_dim_name(type));
        return -1;
    }
    if (want_int) {
        // want_int: 1 any integer kind, 2 signed, 3 unsigned
        if (m->kind_fixed || m->access) {
            if (m->kind == IMGKIND_FLOAT || (want_int == 2 && m->kind != IMGKIND_INT) || (want_int == 3 && m->kind != IMGKIND_UINT)) {
                fail(c, "%s: needs %s integer image but it is %s%s", what,
                     want_int == 2   ? "a signed"
                     : want_int == 3 ? "an unsigned"
                                     : "an",
                     m->kind == IMGKIND_FLOAT ? "float"
                     : m->kind == IMGKIND_INT ? "signed integer"
                                              : "unsigned integer",
                     m->kind_fixed ? " (ShaderKey.image_int_mask / image_uint_mask)" : " (an earlier access in this shader)");
                return -1;
            }
        } else {
            m->kind = want_int == 2 ? IMGKIND_INT : IMGKIND_UINT;
            m->kind_fixed = true;
        }
    }
    return s;
}

// register read (RZ = 0)
static const char *sfr(Ctx *c, int r) {
    static char bufs[8][16];
    static int bi;
    char *o = bufs[bi++ & 7];
    return reg(c, r, o);
}

static void emit_surface(Ctx *c, uint64_t w, int sf, bool bindless) {
    int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8), rb = (int)bits(w, 20, 8);
    int clampm = (int)bits(w, 47, 2), dim = (int)bits(w, 49, 3);
    char name[64];
    const char *base = sf == SF_SULD ? "SULD" : sf == SF_SUST ? "SUST" : sf == SF_SURED ? "SURED" : "SUATOM";
    int size = 0, pmode = 0, mask = 0, aop = 0, atype = 0;
    bool ba = false;
    if (sf == SF_SULD || sf == SF_SUST) {
        ba = bits(w, 26, 1);
        size = (int)bits(w, 28, 3);
        pmode = (int)bits(w, 31, 1);
        mask = (int)bits(w, 32, 4);
        snprintf(name, sizeof name, "%s.%s%s.%s", base, pmode ? "P" : "D", ba ? ".BA" : "", sf_dim_name[dim]);
        if (!pmode) {
            size_t l = strlen(name);
            snprintf(name + l, sizeof name - l, ".%s", sf_size_name[size]);
        }
    } else {
        aop = (int)bits(w, 28, 4);
        atype = (int)bits(w, 32, 3);
        snprintf(name, sizeof name, "%s.%s.%s", base, sf_atom_name[aop], sf_dim_name[dim]);
    }
    if (sf_dim_type[dim] < 0) {
        fail(c, "%s: reserved dimension %d", name, dim);
        return;
    }
    int type = sf_dim_type[dim], nsp = sf_dim_nsp[dim], arr = sf_dim_arr[dim], ncoord = nsp + arr;
    if (a != RZ && a + ncoord > RZ) {
        fail(c, "%s: coordinate registers run past R254", name);
        return;
    }
    // ---- unsupported combinations, named
    if (sf == SF_SULD || sf == SF_SUST) {
        if (pmode && mask == 0) {
            fail(c, "%s: empty channel mask", name);
            return;
        }
        if (!pmode && size > 6) {
            fail(c, "%s: reserved size %d", name, size);
            return;
        }
        if (pmode && ba) {
            fail(c, "%s: byte addressing with a formatted (.P) access is not defined", name);
            return;
        }
    } else {
        static const char *const why[16] = {NULL,
                                            NULL,
                                            NULL,
                                            "INC (wrapping increment) has no Metal texture atomic: MSL 3.1 provides add, sub, and, or, xor, min and max only",
                                            "DEC (wrapping decrement) has no Metal texture atomic: MSL 3.1 provides add, sub, and, or, xor, min and max only",
                                            NULL,
                                            NULL,
                                            NULL,
                                            "EXCH has no Metal texture atomic (no atomic_exchange / compare_exchange on textures in MSL 3.1)",
                                            "CAS has no Metal texture atomic (no atomic_compare_exchange on textures in MSL 3.1)",
                                            "reserved atomic op",
                                            "reserved atomic op",
                                            "reserved atomic op",
                                            "reserved atomic op",
                                            "reserved atomic op",
                                            "reserved atomic op"};
        if (why[aop]) {
            fail(c, "%s: %s", name, why[aop]);
            return;
        }
        if (atype == 2 || atype == 4) {
            fail(
                c,
                "%s: 64-bit surface atomics are not supported (Metal texture atomics are 32-bit; ulong textures need MSL 3.1 on Apple9 only and have no Maxwell-compatible form)",
                name);
            return;
        }
        if (atype == 3) {
            fail(c, "%s: floating-point surface atomics are not supported (Metal texture atomics are integer only)", name);
            return;
        }
        if (atype > 4) {
            fail(c, "%s: reserved atomic type %d", name, atype);
            return;
        }
    }
    // ---- handle
    uint32_t index = (uint32_t)bits(w, 36, 11);
    if (bindless) {
        int v = rb == RZ ? -1 : c->reg_cb[rb];
        if (v < 0) {
            fail(c, "%s: bindless surface handle is not a constant buffer load", name);
            return;
        }
        index = bindless_index(c, v >> 24, v & 0xffffff);
    }
    if (c->fail) return;
    bool sub = (sf == SF_SULD || sf == SF_SUST) && !pmode && size < 4;
    bool atom = sf == SF_SURED || sf == SF_SUATOM;
    int want = sub ? 1 : atom ? ((aop == 1 || aop == 2) ? (atype == 1 ? 2 : 3) : 1) : 0;
    int s = img_slot(c, index, type, want, name);
    if (s < 0) return;
    ShaderImage *im = &c->info->img[s];
    if (atom) {
        // after img_slot: a key-declared integer image keeps its declared signedness, which img_slot already checked against the op type
        im->atomic = true;
        c->info->needs_msl31 = true;
    }
    int k = im->kind;
    const char *T4 = k == IMGKIND_FLOAT ? "float4" : k == IMGKIND_INT ? "int4" : "uint4";
    // ---- coordinates, bounds
    int bytes_log2 = 0;
    if (ba) bytes_log2 = size <= 1 ? 0 : size <= 3 ? 1 : size - 2;
    Str *b = &c->body;
    sp(b, "    { uint cx = %s%s;", sfr(c, a), "");
    if (ba && bytes_log2) sp(b, " cx >>= %d;", bytes_log2);
    if (ncoord > 1) sp(b, " uint cy = %s;", sfr(c, a == RZ ? RZ : a + 1));
    if (ncoord > 2) sp(b, " uint cz = %s;", sfr(c, a == RZ ? RZ : a + 2));
    sp(b, "\n");
    const char *dimx[3] = {"get_width", "get_height", "get_depth"};
    const char *cv[3] = {"cx", "cy", "cz"};
    char ok[256] = "true";
    int cmp_n = 0;
    for (int i = 0; i < nsp; i++) {
        if (clampm == 2) sp(b, "      %s = min(%s, im%d.%s() - 1u);\n", cv[i], cv[i], s, dimx[i]);
        else {
            char t[64];
            snprintf(t, sizeof t, "%s%s < im%d.%s()", cmp_n ? " && " : "", cv[i], s, dimx[i]);
            if (!cmp_n) ok[0] = 0;
            strcat(ok, t);
            cmp_n++;
        }
    }
    if (arr) {
        const char *lv = nsp == 1 ? "cy" : "cz";
        if (clampm == 2) sp(b, "      %s = min(%s, im%d.get_array_size() - 1u);\n", lv, lv, s);
        else {
            char t[64];
            snprintf(t, sizeof t, "%s%s < im%d.get_array_size()", cmp_n ? " && " : "", lv, s);
            if (!cmp_n) ok[0] = 0;
            strcat(ok, t);
            cmp_n++;
        }
    }
    sp(b, "      bool ok = %s;\n", ok);
    char coord[64], lay[8] = "";
    if (nsp == 1) strcpy(coord, "cx");
    else if (nsp == 2) strcpy(coord, "uint2(cx, cy)");
    else strcpy(coord, "uint3(cx, cy, cz)");
    if (arr) strcpy(lay, nsp == 1 ? ", cy" : ", cz");
    // component <-> 32-bit register bits
    const char *tobits[3] = {"as_type<uint>(%s)", "as_type<uint>(%s)", "%s"};
    const char *frombits[3] = {"as_type<float>(%s)", "as_type<int>(%s)", "%s"};
    char ex[4][160];
    if (sf == SF_SULD) {
        sp(b, "      %s t = ok ? im%d.read(%s%s) : %s(0);\n", T4, s, coord, lay, T4);
        im->access |= IMGACC_READ;
        const char *cn[4] = {"x", "y", "z", "w"};
        int outn = 0, srcc[4];
        if (pmode) {
            for (int ch = 0; ch < 4; ch++)
                if (mask & (1 << ch)) srcc[outn++] = ch;
        } else {
            outn = size == 5 ? 2 : size == 6 ? 4 : 1;
            for (int i = 0; i < outn; i++) srcc[i] = i;
        }
        for (int i = 0; i < outn; i++) {
            char comp[16], bits_[64];
            snprintf(comp, sizeof comp, "t.%s", cn[srcc[i]]);
            snprintf(bits_, sizeof bits_, tobits[k], comp);
            if (!pmode && size == 0) snprintf(ex[i], sizeof ex[i], "(%s & 0xFFu)", bits_);
            else if (!pmode && size == 1) snprintf(ex[i], sizeof ex[i], "uint(int(char(%s)))", bits_);
            else if (!pmode && size == 2) snprintf(ex[i], sizeof ex[i], "(%s & 0xFFFFu)", bits_);
            else if (!pmode && size == 3) snprintf(ex[i], sizeof ex[i], "uint(int(short(%s)))", bits_);
            else snprintf(ex[i], sizeof ex[i], "%s", bits_);
        }
        for (int i = 0; i < outn; i++)
            if (d != RZ) {
                if (d + i > 254) break;
                c->reg_used[d + i] = 1;
                c->reg_cb[d + i] = -1;
                sp(b, "      r%d = %s;\n", d + i, ex[i]);
            }
    } else if (sf == SF_SUST) {
        im->access |= IMGACC_WRITE;
        char v[4][80];
        for (int i = 0; i < 4; i++) snprintf(v[i], sizeof v[i], k == IMGKIND_FLOAT ? "0.0f" : "0");
        if (k == IMGKIND_FLOAT) snprintf(v[3], sizeof v[3], "1.0f");
        else snprintf(v[3], sizeof v[3], "1");
        int ch_of[4], n = 0;
        if (pmode) {
            for (int ch = 0; ch < 4; ch++)
                if (mask & (1 << ch)) ch_of[n++] = ch;
        } else {
            n = size == 5 ? 2 : size == 6 ? 4 : 1;
            for (int i = 0; i < n; i++) ch_of[i] = i;
        }
        for (int i = 0; i < n; i++) {
            const char *src = sfr(c, d == RZ ? RZ : d + i);
            char raw[64];
            if (!pmode && size == 0) snprintf(raw, sizeof raw, k == IMGKIND_INT ? "int(char(%s))" : "(%s & 0xFFu)", src);
            else if (!pmode && size == 1) snprintf(raw, sizeof raw, k == IMGKIND_INT ? "int(char(%s))" : "(%s & 0xFFu)", src);
            else if (!pmode && size == 2) snprintf(raw, sizeof raw, k == IMGKIND_INT ? "int(short(%s))" : "(%s & 0xFFFFu)", src);
            else if (!pmode && size == 3) snprintf(raw, sizeof raw, k == IMGKIND_INT ? "int(short(%s))" : "(%s & 0xFFFFu)", src);
            else snprintf(raw, sizeof raw, frombits[k], src);
            snprintf(v[ch_of[i]], sizeof v[0], "%s", raw);
        }
        if (pmode && mask != 15) {
            // unselected channels keep their value: read-modify-write
            im->access |= IMGACC_READ;
            sp(b, "      if (ok) { %s t = im%d.read(%s%s);\n", T4, s, coord, lay);
            const char *cn[4] = {"x", "y", "z", "w"};
            for (int ch = 0; ch < 4; ch++)
                if (mask & (1 << ch)) sp(b, "        t.%s = %s;\n", cn[ch], v[ch]);
            sp(b, "        im%d.write(t, %s%s); }\n", s, coord, lay);
        } else {
            sp(b, "      if (ok) im%d.write(%s(%s, %s, %s, %s), %s%s);\n", s, T4, v[0], v[1], v[2], v[3], coord, lay);
        }
    } else {
        im->access |= IMGACC_READ | IMGACC_WRITE;
        int opr = sf == SF_SURED ? d : bindless ? (int)bits(w, 36, 8) : rb;   // SURED: data in Rd like RED
        static const char *const fn[8] = {"add", "min", "max", NULL, NULL, "and", "or", "xor"};
        char val[64];
        snprintf(val, sizeof val, frombits[k], sfr(c, opr));
        sp(b, "      %s old = ok ? im%d.atomic_fetch_%s(%s%s, %s(%s)) : %s(0);\n", T4, s, fn[aop], coord, lay, T4, val, T4);
        if (sf == SF_SUATOM && d != RZ) {
            c->reg_used[d] = 1;
            c->reg_cb[d] = -1;
            char ob[48];
            snprintf(ob, sizeof ob, tobits[k], "old.x");
            sp(b, "      r%d = %s;\n", d, ob);
        } else sp(b, "      (void)old;\n");
    }
    sp(b, "    }\n");
}

// declare the image arguments of main0
static void emit_image_args(Str *args, const ShaderInfo *info) {
    for (int i = 0; i < info->nimg; i++) {
        const ShaderImage *m = &info->img[i];
        const char *acc = (m->access & IMGACC_READ) && (m->access & IMGACC_WRITE) ? "read_write" : (m->access & IMGACC_WRITE) ? "write" : "read";
        const char *T = m->kind == IMGKIND_FLOAT ? "float" : m->kind == IMGKIND_INT ? "int" : "uint";
        sp(args, ", %s<%s, access::%s> im%d [[texture(%d)]]", img_msl_type(m->type), T, acc, i, IMAGE_TEXTURE_BASE + i);
    }
}
