/* notif:a alarm settings store (portable): the alarms an application registers through INotificationServicesForApplication (switchbrew 'Glue
 * services', libnx notif; layout facts only). An alarm setting is 0x40 bytes and starts with {u16 alarm_setting_id, u8 kind, u8 muted}; the service
 * assigns the id at registration and keeps an application parameter of up to NOTIF_PARAM_MAX bytes beside it. The console raises the alarm through the
 * HOME menu at the scheduled time; napple has no HOME menu, so alarms are kept (for the life of the process) and never fire. Limits are napple's choice
 * (NOTIF_ALARM_MAX), recorded in docs/architecture/runtime-services.md. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define NOTIF_SETTING_SIZE 0x40u
#define NOTIF_PARAM_MAX 0x400u
#define NOTIF_ALARM_MAX 8u

typedef struct {
    uint8_t setting[NOTIF_SETTING_SIZE];
    uint8_t param[NOTIF_PARAM_MAX];
    uint32_t param_size;
    int used;
} NotifAlarm;
typedef struct {
    NotifAlarm a[NOTIF_ALARM_MAX];
    uint16_t next_id; /* last id handed out; ids start at 1 and are never reused while the process runs */
    int initialized;
} NotifStore;

enum { NOTIF_OK = 0, NOTIF_ERR_FULL = 1, NOTIF_ERR_NOT_FOUND = 2, NOTIF_ERR_INVALID = 3 };

void notif_init(NotifStore *s);
/* Registers a setting (at least NOTIF_SETTING_SIZE bytes) with its parameter (at most NOTIF_PARAM_MAX bytes); the stored copy carries the new id,
 * returned in *id. */
int notif_register(NotifStore *s, const void *setting, size_t setting_size, const void *param, size_t param_size, uint16_t *id);
/* Replaces the setting whose id is the first u16 of `setting`, and its parameter. */
int notif_update(NotifStore *s, const void *setting, size_t setting_size, const void *param, size_t param_size);
/* Copies whole settings, oldest registration first, into `out` (cap bytes); returns how many were copied. */
uint32_t notif_list(const NotifStore *s, void *out, size_t cap);
/* Copies the parameter of alarm `id` (at most cap bytes) and sets *size to the stored size. */
int notif_load_param(const NotifStore *s, uint16_t id, void *out, size_t cap, uint32_t *size);
int notif_delete(NotifStore *s, uint16_t id);
