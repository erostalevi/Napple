/* Per-installation identity (portable): the random id behind GetPseudoDeviceId and the friends service's play-history keys.
 *
 * An installation is a user-data root (--save-root, default ~/Library/Application Support/Napple). Its id is 16 random bytes kept in
 * <root>/system/installation-id, made on first use, so every Napple installation (and every private save root on one Mac) presents itself as a
 * different console, and the same one across runs. Nothing here comes from or imitates a console's unique data. */
#pragma once
#include <stdint.h>

/* The installation id of `root` (created when missing). 0 ok; -1 when it could not be read or stored (out then holds a process-lifetime random id). */
int install_id_get(const char *root, uint8_t out[16]);
/* GetPseudoDeviceId: a UUID (version 4 / RFC 4122 variant bits) that is stable for (installation, application, user) and differs between them. */
void install_pseudo_device_id(const uint8_t inst[16], uint64_t application_id, const uint8_t user[16], uint8_t out[16]);
/* friend GetPlayHistoryRegistrationKey: the 0x40-byte key {u16 type 0x101, u8 key index, u8 0, u8 local, 11 reserved, user id (16), 32-byte keyed hash of
 * the first 0x20 bytes}. The hash is keyed with the installation id (only Napple's own friends service ever reads these keys). */
void install_play_history_key(const uint8_t inst[16], const uint8_t user[16], int local, uint8_t out[0x40]);
