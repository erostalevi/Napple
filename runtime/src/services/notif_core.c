/* notif:a alarm settings store (portable): see notif_core.h. */
#include "notif_core.h"
#include <string.h>

void notif_init(NotifStore *s) { memset(s, 0, sizeof *s); }

static NotifAlarm *find(NotifStore *s, uint16_t id) {
    for (unsigned i = 0; id && i < NOTIF_ALARM_MAX; i++) {
        uint16_t v;
        memcpy(&v, s->a[i].setting, 2);
        if (s->a[i].used && v == id) return &s->a[i];
    }
    return NULL;
}
static void put(NotifAlarm *a, const void *setting, const void *param, size_t param_size) {
    memcpy(a->setting, setting, NOTIF_SETTING_SIZE);
    a->param_size = (uint32_t)param_size;
    memset(a->param, 0, sizeof a->param);
    if (param_size) memcpy(a->param, param, param_size);
}

int notif_register(NotifStore *s, const void *setting, size_t setting_size, const void *param, size_t param_size, uint16_t *id) {
    if (!setting || setting_size < NOTIF_SETTING_SIZE || param_size > NOTIF_PARAM_MAX || (param_size && !param)) return NOTIF_ERR_INVALID;
    NotifAlarm *slot = NULL;
    for (unsigned i = 0; i < NOTIF_ALARM_MAX && !slot; i++)
        if (!s->a[i].used) slot = &s->a[i];
    if (!slot) return NOTIF_ERR_FULL;
    if (++s->next_id == 0) s->next_id = 1;
    put(slot, setting, param, param_size);
    memcpy(slot->setting, &s->next_id, 2);
    slot->used = 1; /* used entries stay packed at the front (delete compacts), so the first free slot keeps registration order */
    if (id) *id = s->next_id;
    return NOTIF_OK;
}

int notif_update(NotifStore *s, const void *setting, size_t setting_size, const void *param, size_t param_size) {
    if (!setting || setting_size < NOTIF_SETTING_SIZE || param_size > NOTIF_PARAM_MAX || (param_size && !param)) return NOTIF_ERR_INVALID;
    uint16_t id;
    memcpy(&id, setting, 2);
    NotifAlarm *a = find(s, id);
    if (!a) return NOTIF_ERR_NOT_FOUND;
    put(a, setting, param, param_size);
    return NOTIF_OK;
}

uint32_t notif_list(const NotifStore *s, void *out, size_t cap) {
    uint32_t n = 0;
    for (unsigned i = 0; i < NOTIF_ALARM_MAX; i++) {
        if (!s->a[i].used) continue;
        if (!out || (size_t)(n + 1) * NOTIF_SETTING_SIZE > cap) break;
        memcpy((uint8_t *)out + (size_t)n * NOTIF_SETTING_SIZE, s->a[i].setting, NOTIF_SETTING_SIZE);
        n++;
    }
    return n;
}

int notif_load_param(const NotifStore *s, uint16_t id, void *out, size_t cap, uint32_t *size) {
    NotifAlarm *a = find((NotifStore *)s, id);
    if (!a) return NOTIF_ERR_NOT_FOUND;
    size_t n = a->param_size < cap ? a->param_size : cap;
    if (n && out) memcpy(out, a->param, n);
    if (size) *size = a->param_size;
    return NOTIF_OK;
}

int notif_delete(NotifStore *s, uint16_t id) {
    NotifAlarm *a = find(s, id);
    if (!a) return NOTIF_ERR_NOT_FOUND;
    unsigned at = (unsigned)(a - s->a);
    for (unsigned i = at; i + 1 < NOTIF_ALARM_MAX; i++) s->a[i] = s->a[i + 1];
    memset(&s->a[NOTIF_ALARM_MAX - 1], 0, sizeof s->a[0]);
    return NOTIF_OK;
}
