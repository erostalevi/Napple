/* aoc:u core: the add-on content (DLC) set of the running application, read from the data directory's manifest.json (portable).
 * `napple build` embeds every selected DLC as <game>/dlc/<TITLEID>/v<version>/romfs.bin and lists it in the manifest "dlc" array:
 *   {"title_id":"0100000000011001","version":0,"dir":"../dlc/0100000000011001/v0","romfs":"romfs.bin","default_enabled":true,...}
 * (dir is relative to the manifest's own directory). A DLC title id is the application's add-on base id (application id + 0x1000) plus the
 * add-on index, which is what ListAddOnContent returns (1-based); an entry whose id is outside that range is skipped. */
#pragma once
#include <stdint.h>

#define AOC_MAX 128
#define AOC_PATH 1200

typedef struct { uint64_t title_id; uint32_t version; uint32_t index; char romfs[AOC_PATH]; } AocEntry;
typedef struct { AocEntry e[AOC_MAX]; unsigned n; uint64_t base_id; } AocSet;

uint64_t aoc_base_id(uint64_t application_id);
void aoc_init(AocSet *s, uint64_t application_id);
/* Adds one DLC given by its title id, version and the path of its romfs image. Returns 1 when kept; 0 when dropped (title id outside
 * base id + 1..0xfff, duplicate index, set full). The set stays sorted by index. The manifest reader is misc.c (aoc_load_manifest). */
int aoc_add(AocSet *s, uint64_t title_id, uint32_t version, const char *romfs_path);
unsigned aoc_count(const AocSet *s);
/* Fills out[] with up to `count` indices starting at list position `offset`; returns how many were written (0 past the end). */
unsigned aoc_list(const AocSet *s, uint32_t offset, uint32_t count, int32_t *out);
const AocEntry *aoc_find_index(const AocSet *s, int32_t index);
/* The entry whose DLC title (data) id is `data_id`, for OpenDataStorageByDataId. */
const AocEntry *aoc_find_data_id(const AocSet *s, uint64_t data_id);
