/* IPC reference check: every application-reachable command id of every reference list in tests/ipc_reference must have a row in the runtime's
 * command table for that interface (IPC_IMPL, or IPC_STUB = a documented safe default). A command with no row stops the runtime (ipc.c
 * unimplemented()), so a missing row here is a guest crash waiting for the title that calls it.
 *
 * Fixture format (tests/ipc_reference/<service-or-interface>.txt, one file per interface):
 *   # interface <name> (<source>)        first line; <name> is the interface string the runtime gives the object (first argument of
 *                                        IPC_OBJECT / ipc_object_new, e.g. "hid", "IAppletResource", "audren:u"), matched exactly;
 *                                        <source> is where the list comes from (libnx wrapper, switchbrew page)
 *   id<TAB>name<TAB>min_fw<TAB>y|n       one command per line; the last field says whether an application can reach it (y) or not (n: system or
 *                                        debug only, another console model, a firmware newer than the target)
 *   # ...                                comments. Two optional comment directives help when the binding cannot be read from the source:
 *   # alias <runtime-iface>              also count the rows of tables bound to that interface string
 *   # table <file.c>:<table-identifier>  also count that command table (path relative to src/, e.g. services/mii.c:db_cmds)
 *
 * The rows are read from the sources (the .c files of src and src/services), not from a running runtime: sub-interfaces only exist after a command creates
 * them, while the tables are static. A table is bound to an interface name where a string literal is followed by the table identifier
 * (IPC_OBJECT("x", table, ...), SUB(fn, "x", table)) or where the table identifier is followed, in the same call, by a string literal
 * (new_session(table, n, "x")). Files whose service_register_* functions are never called are dead code and skipped (audin.c).
 *
 * Usage: test_ipc_reference [-v] [runtime-dir]   (default: the current directory or ./runtime). -v also lists rows the runtime has but the
 * reference does not (possible misnumbered rows) and rows whose names differ. Exit status 1 when any app-reachable id has no row. */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    unsigned id;
    char name[96];
    int stub;
} Row;
typedef struct {
    char file[96];  /* path relative to src/ */
    char ident[96]; /* table identifier */
    size_t pos, end; /* offsets of the declaration and of the table's closing brace */
    Row *rows;
    unsigned nrows;
} Table;
typedef struct {
    char iface[96];
    unsigned table;
} Binding;

static Table *tables;
static unsigned ntables, cap_tables;
static Binding *binds;
static unsigned nbinds, cap_binds;
static int verbose;

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(b);
        return NULL;
    }
    fclose(f);
    b[n] = 0;
    if (len) *len = (size_t)n;
    return b;
}

/* Comments become spaces (offsets are kept); string and character literals are left alone. */
static void strip_comments(char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '"' || s[i] == '\'') {
            char q = s[i];
            for (i++; i < n && s[i] != q; i++)
                if (s[i] == '\\') i++;
        } else if (s[i] == '/' && i + 1 < n && s[i + 1] == '*') {
            for (; i < n && !(s[i] == '*' && i + 1 < n && s[i + 1] == '/'); i++)
                if (s[i] != '\n') s[i] = ' ';
            if (i < n) s[i] = ' ', s[i + 1] = ' ', i++;
        } else if (s[i] == '/' && i + 1 < n && s[i + 1] == '/') {
            for (; i < n && s[i] != '\n'; i++) s[i] = ' ';
        }
    }
}
static int is_ident(char c) { return isalnum((unsigned char)c) || c == '_'; }
static size_t skip_ws(const char *s, size_t i) {
    while (s[i] && isspace((unsigned char)s[i])) i++;
    return i;
}
/* Reads a string literal starting at s[i] == '"' into out; returns the offset after it. */
static size_t read_string(const char *s, size_t i, char *out, size_t cap) {
    size_t o = 0;
    for (i++; s[i] && s[i] != '"'; i++) {
        if (s[i] == '\\' && s[i + 1]) i++;
        if (o + 1 < cap) out[o++] = s[i];
    }
    out[o] = 0;
    return s[i] ? i + 1 : i;
}
static size_t read_ident(const char *s, size_t i, char *out, size_t cap) {
    size_t o = 0;
    while (is_ident(s[i])) {
        if (o + 1 < cap) out[o++] = s[i];
        i++;
    }
    out[o] = 0;
    return i;
}
/* Offset of the brace closing the one at s[i] (strings skipped). */
static size_t match_brace(const char *s, size_t i) {
    int depth = 0;
    for (; s[i]; i++) {
        if (s[i] == '"' || s[i] == '\'') {
            char q = s[i];
            for (i++; s[i] && s[i] != q; i++)
                if (s[i] == '\\') i++;
        } else if (s[i] == '{') depth++;
        else if (s[i] == '}' && --depth == 0) return i;
    }
    return i;
}

static void add_binding(const char *iface, unsigned t) {
    for (unsigned i = 0; i < nbinds; i++)
        if (binds[i].table == t && !strcmp(binds[i].iface, iface)) return;
    if (nbinds == cap_binds) binds = realloc(binds, (cap_binds = cap_binds ? cap_binds * 2 : 256) * sizeof *binds);
    snprintf(binds[nbinds].iface, sizeof binds[nbinds].iface, "%s", iface);
    binds[nbinds++].table = t;
}
/* The table of this file named `ident` declared nearest before `pos` (or the first one after it). */
static int find_table(const char *file, const char *ident, size_t pos) {
    int best = -1, after = -1;
    for (unsigned t = 0; t < ntables; t++) {
        if (strcmp(tables[t].file, file) || strcmp(tables[t].ident, ident)) continue;
        if (tables[t].pos <= pos) {
            if (best < 0 || tables[t].pos > tables[best].pos) best = (int)t;
        } else if (after < 0) after = (int)t;
    }
    return best >= 0 ? best : after;
}

static void parse_tables(const char *file, const char *s) {
    for (const char *p = s; (p = strstr(p, "IpcCmd")); p += 6) {
        size_t i = (size_t)(p - s);
        if ((i && is_ident(s[i - 1])) || is_ident(s[i + 6])) continue;
        char ident[96];
        i = skip_ws(s, i + 6);
        if (!is_ident(s[i])) continue;
        i = read_ident(s, i, ident, sizeof ident);
        i = skip_ws(s, i);
        if (s[i] != '[') continue;
        while (s[i] && s[i] != ']') i++;
        i = skip_ws(s, i + 1);
        if (s[i] != '=') continue;
        i = skip_ws(s, i + 1);
        if (s[i] != '{') continue;
        size_t end = match_brace(s, i);
        if (ntables == cap_tables) tables = realloc(tables, (cap_tables = cap_tables ? cap_tables * 2 : 512) * sizeof *tables);
        Table *t = &tables[ntables++];
        memset(t, 0, sizeof *t);
        snprintf(t->file, sizeof t->file, "%s", file);
        snprintf(t->ident, sizeof t->ident, "%s", ident);
        t->pos = (size_t)(p - s);
        t->end = end;
        unsigned cap = 0;
        for (size_t j = i + 1; j < end; j++) {
            if (s[j] == '"') { /* a string outside an entry (cannot happen in a row table, but skip it) */
                char tmp[8];
                j = read_string(s, j, tmp, sizeof tmp) - 1;
                continue;
            }
            if (s[j] != '{') continue;
            size_t e = match_brace(s, j), k = skip_ws(s, j + 1);
            char *num_end;
            unsigned long id = strtoul(s + k, &num_end, 0);
            if (num_end != s + k) {
                k = skip_ws(s, (size_t)(num_end - s));
                if (s[k] == ',') {
                    k = skip_ws(s, k + 1);
                    Row r = {.id = (unsigned)id};
                    if (s[k] == '"') read_string(s, k, r.name, sizeof r.name);
                    for (size_t q = k; q + 8 <= e; q++)
                        if (!strncmp(s + q, "IPC_STUB", 8) && !is_ident(s[q + 8]) && !is_ident(s[q - 1])) r.stub = 1;
                    if (t->nrows == cap) t->rows = realloc(t->rows, (cap = cap ? cap * 2 : 16) * sizeof *t->rows);
                    t->rows[t->nrows++] = r;
                }
            }
            j = e;
        }
    }
}
static int inside_table(const char *file, size_t pos) {
    for (unsigned t = 0; t < ntables; t++)
        if (pos > tables[t].pos && pos < tables[t].end && !strcmp(tables[t].file, file)) return 1;
    return 0;
}
static void parse_bindings(const char *file, const char *s) {
    for (size_t i = 0; s[i]; i++) {
        if (s[i] == '\'') {
            for (i++; s[i] && s[i] != '\''; i++)
                if (s[i] == '\\') i++;
            continue;
        }
        if (s[i] == '"') { /* "iface", table */
            char lit[96], ident[96];
            size_t a = read_string(s, i, lit, sizeof lit), k = skip_ws(s, a);
            if (s[k] == ',' && !inside_table(file, i)) {
                k = skip_ws(s, k + 1);
                if (is_ident(s[k]) && !isdigit((unsigned char)s[k])) {
                    read_ident(s, k, ident, sizeof ident);
                    int t = find_table(file, ident, i);
                    if (t >= 0) add_binding(lit, (unsigned)t);
                }
            }
            i = a - 1;
            continue;
        }
        if (is_ident(s[i]) && (i == 0 || !is_ident(s[i - 1]))) { /* table, ..., "iface" in one call */
            char ident[96];
            size_t a = read_ident(s, i, ident, sizeof ident);
            int t = find_table(file, ident, i);
            if (t >= 0 && !inside_table(file, i)) {
                size_t k = skip_ws(s, a);
                if (s[k] == ',') {
                    int depth = 0;
                    for (k++; s[k] && s[k] != ';' && depth >= 0; k++) {
                        if (s[k] == '(' || s[k] == '{') depth++;
                        else if (s[k] == ')' || s[k] == '}') depth--;
                        else if (s[k] == '"') {
                            char lit[96];
                            size_t z = read_string(s, k, lit, sizeof lit);
                            if (depth == 0) add_binding(lit, (unsigned)t);
                            k = z - 1;
                        }
                    }
                }
            }
            i = a - 1;
        }
    }
}

/* ---- sources ---- */
typedef struct {
    char rel[96];
    char *text;
    int dead;
} Src;
static Src srcs[512];
static unsigned nsrcs;
static void load_dir(const char *root, const char *sub) {
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/src%s%s", root, *sub ? "/" : "", sub);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && nsrcs < sizeof srcs / sizeof *srcs) {
        size_t n = strlen(e->d_name);
        if (n < 3 || strcmp(e->d_name + n - 2, ".c")) continue;
        char path[1200];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        size_t len;
        char *t = slurp(path, &len);
        if (!t) continue;
        strip_comments(t, len);
        Src *x = &srcs[nsrcs++];
        snprintf(x->rel, sizeof x->rel, "%s%s%s", sub, *sub ? "/" : "", e->d_name);
        x->text = t;
    }
    closedir(d);
}
/* A file that defines service_register_* functions none of which is ever called is not part of the runtime's service set. */
static void mark_dead(void) {
    for (unsigned f = 0; f < nsrcs; f++) {
        int defines = 0, called = 0;
        for (const char *p = srcs[f].text; (p = strstr(p, "void service_register_")); p++) {
            const char *nm = p + 5;
            char name[96];
            read_ident(nm, 0, name, sizeof name);
            const char *after = nm + strlen(name);
            while (*after == ' ') after++;
            if (strncmp(after, "(void)", 6)) continue;
            const char *body = after + 6;
            while (*body == ' ') body++;
            if (*body != '{') continue; /* a declaration */
            defines = 1;
            for (unsigned g = 0; g < nsrcs && !called; g++)
                for (const char *q = srcs[g].text; (q = strstr(q, name)); q += strlen(name)) {
                    if (q > srcs[g].text && is_ident(q[-1])) continue;
                    const char *r = q + strlen(name);
                    if (is_ident(*r)) continue;
                    while (*r == ' ') r++;
                    if (strncmp(r, "()", 2)) continue; /* "(void)" is a definition or declaration */
                    called = 1;
                    break;
                }
        }
        srcs[f].dead = defines && !called;
    }
}

/* ---- fixtures ---- */
typedef struct {
    unsigned id;
    char name[96], fw[32];
    int app;
} Ref;
static int row_of(const int *tl, unsigned ntl, unsigned id, const Row **out) {
    for (unsigned k = 0; k < ntl; k++)
        for (unsigned r = 0; r < tables[tl[k]].nrows; r++)
            if (tables[tl[k]].rows[r].id == id) {
                if (out) *out = &tables[tl[k]].rows[r];
                return 1;
            }
    return 0;
}
static int g_missing, g_errors, g_app, g_fixtures;
static void check_fixture(const char *path, const char *fname) {
    size_t len;
    char *t = slurp(path, &len);
    if (!t) {
        printf("%s: cannot read\n", fname);
        g_errors++;
        return;
    }
    char iface[96] = "", source[256] = "";
    char aliases[8][96];
    unsigned naliases = 0;
    int tl[64];
    unsigned ntl = 0;
    Ref *refs = NULL;
    unsigned nrefs = 0, cap = 0, lineno = 0;
    for (char *line = strtok(t, "\n"); line; line = strtok(NULL, "\n")) {
        lineno++;
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;
        if (!n) continue;
        if (line[0] == '#') {
            const char *c = line + 1;
            while (*c == ' ') c++;
            if (!*iface && !strncmp(c, "interface ", 10)) {
                c += 10;
                size_t k = 0;
                while (c[k] && c[k] != ' ' && c[k] != '(' && k + 1 < sizeof iface) iface[k] = c[k], k++;
                iface[k] = 0;
                const char *o = strchr(c, '(');
                if (o) snprintf(source, sizeof source, "%.*s", (int)(strrchr(o, ')') ? strrchr(o, ')') - o - 1 : (long)strlen(o + 1)), o + 1);
            } else if (!strncmp(c, "alias ", 6) && naliases < 8) snprintf(aliases[naliases++], sizeof aliases[0], "%s", c + 6);
            else if (!strncmp(c, "table ", 6)) {
                char file[96], ident[96];
                const char *colon = strchr(c + 6, ':');
                if (!colon) {
                    printf("%s:%u: bad table directive\n", fname, lineno);
                    g_errors++;
                    continue;
                }
                snprintf(file, sizeof file, "%.*s", (int)(colon - (c + 6)), c + 6);
                snprintf(ident, sizeof ident, "%s", colon + 1);
                int x = find_table(file, ident, 0);
                if (x < 0) {
                    printf("%s:%u: no table %s in %s\n", fname, lineno, ident, file);
                    g_errors++;
                } else if (ntl < 64) tl[ntl++] = x;
            }
            continue;
        }
        if (!*iface) {
            printf("%s:%u: the first line must be '# interface <name> (<source>)'\n", fname, lineno);
            g_errors++;
            break;
        }
        char *f[4] = {0};
        unsigned nf = 0;
        for (char *p = line; nf < 4;) {
            f[nf++] = p;
            char *tab = strchr(p, '\t');
            if (!tab) break;
            *tab = 0;
            p = tab + 1;
        }
        char *end;
        unsigned long id = nf == 4 ? strtoul(f[0], &end, 0) : 0;
        if (nf != 4 || end == f[0] || *end || (strcmp(f[3], "y") && strcmp(f[3], "n"))) {
            printf("%s:%u: expected id<TAB>name<TAB>min_fw<TAB>y|n\n", fname, lineno);
            g_errors++;
            continue;
        }
        if (nrefs == cap) refs = realloc(refs, (cap = cap ? cap * 2 : 64) * sizeof *refs);
        Ref *r = &refs[nrefs++];
        r->id = (unsigned)id;
        snprintf(r->name, sizeof r->name, "%s", f[1]);
        snprintf(r->fw, sizeof r->fw, "%s", f[2]);
        r->app = f[3][0] == 'y';
    }
    if (!*iface) {
        printf("%s: no '# interface' line\n", fname);
        g_errors++;
        free(t);
        free(refs);
        return;
    }
    g_fixtures++;
    for (unsigned b = 0; b < nbinds && ntl < 64; b++) {
        int hit = !strcmp(binds[b].iface, iface);
        for (unsigned a = 0; a < naliases && !hit; a++) hit = !strcmp(binds[b].iface, aliases[a]);
        if (hit) {
            int dup = 0;
            for (unsigned k = 0; k < ntl; k++) dup |= tl[k] == (int)binds[b].table;
            if (!dup) tl[ntl++] = (int)binds[b].table;
        }
    }
    unsigned app = 0, have = 0, impl = 0, stub = 0, missing = 0, extra = 0, any = 0;
    for (unsigned i = 0; i < nrefs; i++) {
        int seen = 0;
        for (unsigned j = 0; j < i; j++) seen |= refs[j].id == refs[i].id;
        any += !seen && row_of(tl, ntl, refs[i].id, NULL);
    }
    for (unsigned i = 0; i < nrefs; i++) {
        int seen = 0; /* the same id listed twice (renamed across firmware): count it once */
        for (unsigned j = 0; j < i; j++) seen |= refs[j].id == refs[i].id && refs[j].app == refs[i].app;
        if (seen || !refs[i].app) continue;
        app++;
        const Row *row = NULL;
        if (row_of(tl, ntl, refs[i].id, &row)) {
            have++;
            if (row->stub) stub++;
            else impl++;
        } else missing++;
    }
    for (unsigned k = 0; k < ntl; k++)
        for (unsigned r = 0; r < tables[tl[k]].nrows; r++) {
            int known = 0;
            for (unsigned i = 0; i < nrefs && !known; i++) known = refs[i].id == tables[tl[k]].rows[r].id;
            extra += !known;
        }
    printf("%-24s %3u ids (%3u with rows, %u rows not listed) | %3u app-reachable: %3u with rows (%u impl, %u stub), missing %u | %s\n", iface, nrefs, any,
           extra, app, have, impl, stub, missing, source);
    if (!ntl && app) printf("  no command table is bound to interface '%s' (IPC_OBJECT name in src/; see the alias/table directives)\n", iface);
    for (unsigned i = 0; i < nrefs; i++) {
        int seen = 0;
        for (unsigned j = 0; j < i; j++) seen |= refs[j].id == refs[i].id && refs[j].app == refs[i].app;
        if (seen || !refs[i].app || row_of(tl, ntl, refs[i].id, NULL)) continue;
        printf("  missing %u %s [%s]\n", refs[i].id, refs[i].name, refs[i].fw);
    }
    if (verbose) {
        for (unsigned k = 0; k < ntl; k++)
            for (unsigned r = 0; r < tables[tl[k]].nrows; r++) {
                const Row *row = &tables[tl[k]].rows[r];
                int known = 0, same = 0;
                for (unsigned i = 0; i < nrefs; i++)
                    if (refs[i].id == row->id) known = 1, same |= !strcmp(refs[i].name, row->name);
                if (!known) printf("  note: row %u %s (%s:%s) is not in the reference list\n", row->id, row->name, tables[tl[k]].file, tables[tl[k]].ident);
                else if (!same) {
                    for (unsigned i = 0; i < nrefs; i++)
                        if (refs[i].id == row->id) {
                            printf("  note: row %u is named %s, the reference says %s\n", row->id, row->name, refs[i].name);
                            break;
                        }
                }
            }
    }
    g_missing += (int)missing;
    g_app += (int)app;
    free(t);
    free(refs);
}
static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int main(int argc, char **argv) {
    const char *root = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) verbose = 1;
        else root = argv[i];
    }
    if (getenv("NAPPLE_IPC_REF_VERBOSE")) verbose = 1;
    static const char *cands[] = {".", "runtime", "..", "../runtime"};
    char probe[1200];
    struct stat st;
    for (unsigned i = 0; !root && i < 4; i++) {
        snprintf(probe, sizeof probe, "%s/src/services/services.c", cands[i]);
        if (!stat(probe, &st)) root = cands[i];
    }
    if (!root) {
        fprintf(stderr, "test_ipc_reference: run from runtime/ (or pass the runtime directory)\n");
        return 2;
    }
    load_dir(root, "");
    load_dir(root, "services");
    mark_dead();
    for (unsigned f = 0; f < nsrcs; f++)
        if (!srcs[f].dead) parse_tables(srcs[f].rel, srcs[f].text);
    for (unsigned f = 0; f < nsrcs; f++)
        if (!srcs[f].dead) parse_bindings(srcs[f].rel, srcs[f].text);
    unsigned ndead = 0;
    for (unsigned f = 0; f < nsrcs; f++) ndead += srcs[f].dead;
    printf("sources: %u files (%u never registered, skipped), %u command tables, %u interface bindings\n", nsrcs, ndead, ntables, nbinds);
    if (verbose) {
        for (unsigned f = 0; f < nsrcs; f++)
            if (srcs[f].dead) printf("  skipped (service_register_* never called): %s\n", srcs[f].rel);
        for (unsigned t = 0; t < ntables; t++) {
            int bound = 0;
            for (unsigned b = 0; b < nbinds && !bound; b++) bound = binds[b].table == t;
            if (!bound) printf("  table with no interface name found: %s:%s (%u rows)\n", tables[t].file, tables[t].ident, tables[t].nrows);
        }
    }

    char dir[1200];
    snprintf(dir, sizeof dir, "%s/tests/ipc_reference", root);
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "test_ipc_reference: no fixture directory %s\n", dir);
        return 2;
    }
    char *names[1024];
    unsigned nn = 0;
    struct dirent *e;
    while ((e = readdir(d)) && nn < 1024) {
        size_t n = strlen(e->d_name);
        if (n > 4 && !strcmp(e->d_name + n - 4, ".txt")) names[nn++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, nn, sizeof *names, cmp_str);
    for (unsigned i = 0; i < nn; i++) {
        char path[2400];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        check_fixture(path, names[i]);
        free(names[i]);
    }
    printf("{\"test\":\"ipc_reference\",\"fixtures\":%d,\"app_reachable\":%d,\"missing\":%d,\"errors\":%d}\n", g_fixtures, g_app, g_missing, g_errors);
    return g_missing || g_errors || !g_fixtures;
}
