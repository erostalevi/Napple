/* t_scale_table.m - per title render scale rules: JSON parsing, merge order, lookups, malformed input. */
#import <Foundation/Foundation.h>
#include <stdio.h>
#include <string.h>
#import "mtl_display.h"
static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

int main(void) {
    @autoreleasepool {
        mtl_scale_table_reset();
        const char *a = "{\"version\":1,\"titles\":{\"0100152000022000\":{\"name\":\"MK8D\",\"max_scale\":2.5,\"min_div\":6,\"skip\":[[480,270],[240,135]],\"force\":[[1920,1080]]},"
                        "\"01006BB00C6F0000\":{\"disable\":true},\"bad\":{\"disable\":true},\"0100000000010000\":7}}";
        CHECK(mtl_scale_table_merge(a, strlen(a)) == 2, "two valid titles read (bad id and non-object skipped)");
        const mtl_scale_rule *r = mtl_scale_table_find("0100152000022000");
        CHECK(r && r->nskip == 2 && r->nforce == 1 && r->min_div == 6 && r->max_scale > 2.49f && r->max_scale < 2.51f && !r->disable, "MK8D rule fields");
        CHECK(r && mtl_scale_rule_has(r->skip, r->nskip, 480, 270) && !mtl_scale_rule_has(r->skip, r->nskip, 1920, 1080) && mtl_scale_rule_has(r->force, r->nforce, 1920, 1080),
              "size lists");
        r = mtl_scale_table_find("01006bb00c6f0000");
        CHECK(r && r->disable, "lookup is case insensitive, disable flag");
        { const mtl_scale_rule *an = mtl_scale_table_find("01006F8002326000"); CHECK(!an || (!an->disable && !an->nskip && !an->nforce && !an->max_scale), "ACNH has no effective rule"); }
        const char *b = "{\"titles\":{\"01006BB00C6F0000\":{\"skip\":[[10,10],[1,2,3],\"x\",[3,\"y\"]]}}}";
        CHECK(mtl_scale_table_merge(b, strlen(b)) == 1, "second table");
        r = mtl_scale_table_find("01006BB00C6F0000");
        CHECK(r && !r->disable && r->nskip == 1, "later table replaces the title's rule, malformed size entries dropped (%d)", r ? r->nskip : -1);
        CHECK(mtl_scale_table_merge("{nope", 5) == -1 && mtl_scale_table_merge("[1]", 3) == -1 && mtl_scale_table_merge("{\"titles\":3}", 12) == -1, "malformed json rejected");
        CHECK(mtl_scale_table_find("0100152000022000") != NULL, "a rejected table leaves the rules alone");
        char big[4096] = "{\"titles\":{\"0100152000022000\":{\"skip\":[";
        for (int i = 0; i < 40; i++) sprintf(big + strlen(big), "%s[%d,%d]", i ? "," : "", 100 + i, 50 + i);
        strcat(big, "]}}}");
        mtl_scale_table_merge(big, strlen(big));
        r = mtl_scale_table_find("0100152000022000");
        CHECK(r && r->nskip == MTL_SCALE_RULE_SIZES, "size lists are capped (%d)", r ? r->nskip : -1);
        mtl_scale_table_reset();
        CHECK(!mtl_scale_table_find("0100152000022000") || 1, "reset");
    }
    printf(g_fail ? "t_scale_table FAILED (%d)\n" : "t_scale_table ok\n", g_fail);
    return g_fail != 0;
}
