/* mtl_scale_table.m - per title render scale rules (docs/architecture/gfx.md, "Render scale": per-title table).
 *
 * The heuristic of mtl_scale.m (mtl_scale_pick_size) decides which render targets are "screen sized". Some games have targets the heuristic gets wrong at
 * other resolutions (a blur or bloom chain whose taps are tuned for one size, a reflection buffer with fixed texel offsets, a SSAO buffer sampled with
 * hard-coded kernels). A small JSON table fixes that per title without code; the future mod / fix system ships the same file format.
 *
 *   { "version": 1, "titles": { "<16 hex title id>": {
 *       "name": "free text",
 *       "disable": false,               // never scale anything for this title
 *       "max_scale": 2.0,               // cap the factor for this title (per axis)
 *       "min_div": 8,                   // rscale_min_div override
 *       "report_aspect": true,          // false: never tell the game the window's aspect (its HUD or camera breaks); the picture is only fitted

 *       "skip":  [[w, h], ...],         // guest sizes (width, height) that are never scaled
 *       "force": [[w, h], ...],         // guest sizes that are scaled even when the heuristic says no (plain 2D targets only)
 *       "note": "why" } } }
 *
 * Built-in rules (kDefaultTable) are merged with an optional file named by NAPPLE_RSCALE_TABLE (the file wins per title). */
#import "mtl_display.h"
#import "internal.h"

#define MAX_RULES 64

/* Audit of the four Testing Material titles (2026-10-27, NAPPLE_SCALE_DIAG logs at 4x, scenes compared at 1x / 2x / 4x): every screen shaped target of these games
 * (LAS: the 960x540 .. 240x135 blur chain and depth copies, SMO: 1600x900 G-buffer and 400x225, ACNH: 1920x1080 and 480x272, MK8D: 1920x1080 and 1280x720) scales
 * cleanly: the bloom / blur taps are UV offsets built from the guest's texel size, so their extent stays the same fraction of the screen, and the fragment position
 * goes through the rscale table. No effect needed a skip or force, so the rules below carry notes only (a rule with no effect is valid). */
static NSString *const kDefaultTable = @"{\"version\":1,\"titles\":{"
    "\"01006BB00C6F0000\":{\"name\":\"The Legend of Zelda: Link's Awakening\",\"note\":\"blur chain 960x540..240x135 and depth copies scale uniformly; 1x/2x/4x checked\"},"
    "\"0100000000010000\":{\"name\":\"Super Mario Odyssey\",\"note\":\"G-buffer 1600x900 (dynamic resolution) scales; 1x/2x/4x differ only at edges\"},"
    "\"01006F8002326000\":{\"name\":\"Animal Crossing: New Horizons\",\"note\":\"1920x1080 and 480x272 chains scale; IMAGE flag set on every target (not a veto)\"},"
    "\"0100152000022000\":{\"name\":\"Mario Kart 8 Deluxe\",\"note\":\"1920x1080 and 1280x720 targets scale; mipped passes fall back to 1x siblings\"}"
    "}}";

static mtl_scale_rule g_rules[MAX_RULES];
static int g_nrules;
static char g_loaded_for[20];
static const mtl_scale_rule *g_active;

static void read_sizes(id a, mtl_scale_size *out, int *n) {
    *n = 0;
    if (![a isKindOfClass:NSArray.class]) return;
    for (id e in (NSArray *)a) {
        if (![e isKindOfClass:NSArray.class] || [(NSArray *)e count] != 2 || *n >= MTL_SCALE_RULE_SIZES) continue;
        NSNumber *w = [(NSArray *)e objectAtIndex:0], *h = [(NSArray *)e objectAtIndex:1];
        if (![w isKindOfClass:NSNumber.class] || ![h isKindOfClass:NSNumber.class]) continue;
        out[*n].w = w.unsignedIntValue;
        out[*n].h = h.unsignedIntValue;
        (*n)++;
    }
}

/* Merges a table into the rule list (later tables replace a title's rule). Returns the number of titles read, -1 for malformed JSON. */
static void ensure_defaults(void);
int mtl_scale_table_merge(const char *json, size_t len) {
    ensure_defaults();
    NSData *data = [NSData dataWithBytes:json length:len];
    id root = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    if (![root isKindOfClass:NSDictionary.class]) return -1;
    NSDictionary *titles = ((NSDictionary *)root)[@"titles"];
    if (![titles isKindOfClass:NSDictionary.class]) return -1;
    int n = 0;
    for (NSString *tid in titles) {
        NSDictionary *r = titles[tid];
        if (![r isKindOfClass:NSDictionary.class] || tid.length != 16) continue;
        int slot = -1;
        for (int i = 0; i < g_nrules; i++)
            if (!strcasecmp(g_rules[i].title_id, tid.UTF8String)) slot = i;
        if (slot < 0) {
            if (g_nrules >= MAX_RULES) continue;
            slot = g_nrules++;
        }
        mtl_scale_rule *o = &g_rules[slot];
        memset(o, 0, sizeof *o);
        snprintf(o->title_id, sizeof o->title_id, "%s", tid.UTF8String);
        o->disable = [r[@"disable"] boolValue];
        o->max_scale = [r[@"max_scale"] floatValue];
        o->min_div = [r[@"min_div"] unsignedIntValue];
        o->no_aspect_report = r[@"report_aspect"] && ![r[@"report_aspect"] boolValue];
        read_sizes(r[@"skip"], o->skip, &o->nskip);
        read_sizes(r[@"force"], o->force, &o->nforce);
        n++;
    }
    g_active = NULL;
    g_loaded_for[0] = 0;
    return n;
}

void mtl_scale_table_reset(void) {
    ensure_defaults();
    g_nrules = 0;
    g_active = NULL;
    g_loaded_for[0] = 0;
}

static void ensure_defaults(void) {
    static int done;
    if (done) return;
    done = 1;
    const char *d = kDefaultTable.UTF8String;
    mtl_scale_table_merge(d, strlen(d));
    const char *path = getenv("NAPPLE_RSCALE_TABLE");
    if (path && *path) {
        NSData *f = [NSData dataWithContentsOfFile:@(path)];
        if (!f || mtl_scale_table_merge(f.bytes, f.length) < 0) LOGW("render scale table %s: cannot be read", path);
        else LOGI("render scale table %s loaded", path);
    }
}

/* The rule of a title (NULL when it has none). */
const mtl_scale_rule *mtl_scale_table_find(const char *title_id) {
    ensure_defaults();
    for (int i = 0; i < g_nrules; i++)
        if (!strcasecmp(g_rules[i].title_id, title_id)) return &g_rules[i];
    return NULL;
}

/* The rule of the running title (cached). */
const mtl_scale_rule *mtl_scale_table_active(void) {
    if (strcmp(g_loaded_for, g_gfx.title_id)) {
        g_active = mtl_scale_table_find(g_gfx.title_id);
        snprintf(g_loaded_for, sizeof g_loaded_for, "%s", g_gfx.title_id);
        if (g_active) LOGI("render scale: per-title rule for %s (disable %d, max scale %.2f, skip %d, force %d)", g_gfx.title_id, g_active->disable, g_active->max_scale,
                           g_active->nskip, g_active->nforce);
    }
    return g_active;
}

int mtl_scale_rule_has(const mtl_scale_size *list, int n, uint32_t w, uint32_t h) {
    for (int i = 0; i < n; i++)
        if (list[i].w == w && list[i].h == h) return 1;
    return 0;
}

/* Called once at graphics start: applies the title's aspect report policy. */
void gfx_scale_table_init(const char *title_id) {
    const mtl_scale_rule *r = mtl_scale_table_find(title_id);
    napple_display_set_report_aspect_allowed(!(r && r->no_aspect_report));
}
