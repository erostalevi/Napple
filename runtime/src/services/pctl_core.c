/* pctl local settings (portable): see pctl_core.h. */
#include "pctl_core.h"
#include <string.h>

int32_t pctl_default_org(uint32_t region) {
    static const int32_t by_region[] = {PCTL_ORG_CERO, PCTL_ORG_ESRB, PCTL_ORG_PEGI, PCTL_ORG_ACB, PCTL_ORG_GRACGCRB, PCTL_ORG_IARC};
    return region < sizeof by_region / sizeof by_region[0] ? by_region[region] : PCTL_ORG_PEGI;
}
void pctl_init(PctlLocal *p, uint32_t region) {
    memset(p, 0, sizeof *p);
    p->rating_org = pctl_default_org(region);
}

static size_t bounded_len(const char *s, size_t n) {
    size_t i = 0;
    while (s && i < n && s[i]) i++;
    return i;
}
int pctl_set_pin(PctlLocal *p, const char *pin, size_t n, int64_t now) {
    size_t len = bounded_len(pin, n);
    if (len && (len < 4 || len > PCTL_PIN_MAX)) return PCTL_ERR_INVALID;
    for (size_t i = 0; i < len; i++)
        if (pin[i] < '0' || pin[i] > '9') return PCTL_ERR_INVALID;
    memset(p->pin, 0, sizeof p->pin);
    if (len) memcpy(p->pin, pin, len);
    p->pin_len = (uint32_t)len;
    p->temp_unlocked = 0; /* a new PIN ends a temporary unlock granted with the old one */
    p->last_updated = now;
    return PCTL_OK;
}
int pctl_unlock(PctlLocal *p, const char *pin, size_t n, int temp) {
    size_t len = bounded_len(pin, n);
    if (p->pin_len && (len != p->pin_len || memcmp(pin, p->pin, len))) return PCTL_ERR_WRONG_PIN;
    if (temp) p->temp_unlocked = 1;
    return PCTL_OK;
}
int pctl_set_org(PctlLocal *p, int32_t org, int64_t now) {
    if (org < 0 || org >= PCTL_ORG_COUNT) return PCTL_ERR_INVALID;
    p->rating_org = org;
    p->last_updated = now;
    return PCTL_OK;
}
int pctl_free_comm_add(PctlLocal *p, uint64_t app, int64_t now) {
    if (!app) return PCTL_ERR_INVALID;
    for (uint32_t i = 0; i < p->nfree_comm; i++)
        if (p->free_comm[i].application_id == app) return PCTL_OK;
    if (p->nfree_comm == PCTL_FREE_COMM_MAX) return PCTL_ERR_FULL;
    p->free_comm[p->nfree_comm].application_id = app;
    p->free_comm[p->nfree_comm].restricted = 1;
    p->nfree_comm++;
    p->last_updated = now;
    return PCTL_OK;
}
uint32_t pctl_free_comm_update(PctlLocal *p, const uint8_t *entries, size_t size, int64_t now) {
    uint32_t changed = 0;
    for (size_t off = 0; entries && off + PCTL_FREE_COMM_ENTRY <= size; off += PCTL_FREE_COMM_ENTRY) {
        uint64_t app;
        memcpy(&app, entries + off, 8);
        for (uint32_t i = 0; i < p->nfree_comm; i++)
            if (p->free_comm[i].application_id == app) {
                p->free_comm[i].restricted = entries[off + 8] != 0;
                changed++;
            }
    }
    if (changed) p->last_updated = now;
    return changed;
}
void pctl_free_comm_reset(PctlLocal *p, int64_t now) {
    memset(p->free_comm, 0, sizeof p->free_comm);
    p->nfree_comm = 0;
    p->last_updated = now;
}
uint32_t pctl_free_comm_get(const PctlLocal *p, uint32_t offset, uint8_t *out, size_t cap) {
    uint32_t n = 0;
    for (uint32_t i = offset; out && i < p->nfree_comm && (size_t)(n + 1) * PCTL_FREE_COMM_ENTRY <= cap; i++, n++) {
        uint8_t *e = out + (size_t)n * PCTL_FREE_COMM_ENTRY;
        memset(e, 0, PCTL_FREE_COMM_ENTRY);
        memcpy(e, &p->free_comm[i].application_id, 8);
        e[8] = p->free_comm[i].restricted;
    }
    return n;
}
