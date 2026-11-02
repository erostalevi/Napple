/* set:sys settings store (portable): the system settings an application or a firmware applet can write through set:sys and read back. A console keeps
 * them in its system save data; napple keeps them for the life of the process (nothing persists across runs, recorded as a decision in
 * docs/architecture/runtime-services.md). Unset values fall back to what set / set:sys answered before the store existed (host language, NAPPLE_REGION,
 * NAPPLE_COLOR_SET, NAPPLE_TIMEZONE, NAPPLE_DEVICE_NICKNAME), so a title that never writes a setting sees no change.
 *
 * Wire sizes are the switchbrew ISystemSettingsServer / libnx setsys structure sizes (layout facts only). Blobs are kept as raw bytes: the service
 * never interprets them beyond the documented defaults below, which is what the console's settings server does with them too (it stores them). */
#pragma once
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#define SYSSET_BACKLIGHT_SIZE 0x28u     /* SetSysBacklightSettings */
#define SYSSET_NOTIFICATION_SIZE 0x18u  /* SetSysNotificationSettings */
#define SYSSET_CLOCK_CONTEXT_SIZE 0x20u /* nn::time::SystemClockContext */
#define SYSSET_TIME_POINT_SIZE 0x18u    /* nn::time::SteadyClockTimePoint */
#define SYSSET_UUID_SIZE 0x10u
#define SYSSET_AUDIO_VOLUME_SIZE 8u     /* SetSysAudioVolume */
#define SYSSET_AUDIO_DEVICES 4u         /* AudioOutputMode targets the store accepts (speaker, headphone, usb, hdmi ...) */
#define SYSSET_NICKNAME_SIZE 0x80u
#define SYSSET_SERIAL_SIZE 0x18u
#define SYSSET_TZ_SIZE 0x24u
#define SYSSET_NETWORK_ENTRY 0x400u   /* SetSysNetworkSettings */
#define SYSSET_NETWORK_MAX 32u
#define SYSSET_BLUETOOTH_ENTRY 0x200u /* SetSysBluetoothDevicesSettings */
#define SYSSET_BLUETOOTH_MAX 10u
#define SYSSET_EULA_ENTRY 0x30u       /* SetSysEulaVersion */
#define SYSSET_EULA_MAX 8u

enum { SYSSET_PLATFORM_GLOBAL = 1, SYSSET_PLATFORM_CHINA = 2 };

typedef struct {
    pthread_mutex_t mu;
    uint64_t language;        /* 0: follow the host language */
    int32_t region;           /* -1: from the language / NAPPLE_REGION */
    int32_t color_set;        /* -1: from NAPPLE_COLOR_SET */
    int32_t platform_region;  /* SYSSET_PLATFORM_* */
    uint32_t account_settings;
    uint8_t lock_screen, quest, console_info_upload, auto_app_download;
    uint8_t has_tz, has_nickname;
    char tz[SYSSET_TZ_SIZE];
    char nickname[SYSSET_NICKNAME_SIZE];
    char serial[SYSSET_SERIAL_SIZE];
    uint8_t backlight[SYSSET_BACKLIGHT_SIZE];
    uint8_t notification[SYSSET_NOTIFICATION_SIZE];
    uint8_t user_clock_ctx[SYSSET_CLOCK_CONTEXT_SIZE];
    uint8_t tz_updated[SYSSET_TIME_POINT_SIZE];
    uint8_t ext_clock_source[SYSSET_UUID_SIZE];
    uint8_t audio_volume[SYSSET_AUDIO_DEVICES][SYSSET_AUDIO_VOLUME_SIZE];
    uint8_t network[SYSSET_NETWORK_MAX][SYSSET_NETWORK_ENTRY];
    uint32_t nnetwork;
    uint8_t bluetooth[SYSSET_BLUETOOTH_MAX][SYSSET_BLUETOOTH_ENTRY];
    uint32_t nbluetooth;
    uint8_t eula[SYSSET_EULA_MAX][SYSSET_EULA_ENTRY];
    uint32_t neula;
} SysSettings;

/* Defaults of an offline retail console: no lock screen, uploads and automatic downloads off, platform region Global, audio volume 8 of 15 on every
 * output (napple's choice: the console default is not documented), backlight with automatic brightness off and brightness 0.5 (napple's choice),
 * serial number `serial` (NAPPLE_SERIAL) when it is 1..23 printable characters, else "XAW00000000000" (the original model's prefix, all digits zero). */
void sysset_init(SysSettings *s, const char *serial);
/* The process-wide store, initialised on first use (with getenv("NAPPLE_SERIAL")). */
SysSettings *sysset_global(void);

/* Blob fields by set:sys command id: the Get and Set of a pair share one field. Returns the field and its size, or NULL for an id that is not a blob
 * getter/setter (7/8, 9/10, 13/14, 15/16, 17/18, 25/26, 27/28, 29/30, 47/48, 150/151). */
uint8_t *sysset_blob(SysSettings *s, unsigned cmd, uint32_t *size);

/* List settings (network profiles 1/2, bluetooth devices 11/12, EULA versions 21/22): Set replaces the whole list with `n` entries of `entry` bytes
 * (at most the list maximum; a trailing partial entry is dropped) and returns the count kept; Get copies up to `cap` bytes of whole entries and returns
 * the count copied. `which` is 0 network, 1 bluetooth, 2 EULA. */
uint32_t sysset_list_set(SysSettings *s, int which, const void *data, uint64_t size);
uint32_t sysset_list_get(SysSettings *s, int which, void *out, uint64_t cap);
uint32_t sysset_list_entry_size(int which);

/* Audio volume of an output device (GetAudioVolume 19 / SetAudioVolume 20). Returns -1 for a device id the store does not track. */
int sysset_audio_volume_get(SysSettings *s, uint32_t device, uint8_t out[SYSSET_AUDIO_VOLUME_SIZE]);
int sysset_audio_volume_set(SysSettings *s, uint32_t device, const uint8_t in[SYSSET_AUDIO_VOLUME_SIZE]);

/* Strings: a NUL-terminated copy limited to the field size (the last byte is always NUL). */
void sysset_set_tz(SysSettings *s, const char *name, size_t n);
void sysset_set_nickname(SysSettings *s, const char *name, size_t n);
