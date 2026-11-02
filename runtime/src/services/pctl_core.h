/* pctl local settings (portable): the parts of parental control a console keeps on itself, without a pairing to the smart-device app. A PIN code (up to
 * PCTL_PIN_MAX digits; none = no restriction to unlock), the temporary unlock it grants, the default rating organization, the free-communication
 * application list and the time of the last change. Kept for the life of the process (nothing persists). Layouts are switchbrew 'Parental Control
 * services' sizes as napple reads them (unverified on a console). */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define PCTL_PIN_MAX 8u
#define PCTL_FREE_COMM_MAX 32u
#define PCTL_FREE_COMM_ENTRY 0x10u /* FreeCommunicationApplicationInfo {u64 application id, u8 restricted, u8 pad[7]} */

/* RatingOrganization, in the order of the control.nacp rating age array. */
enum {
    PCTL_ORG_CERO = 0, PCTL_ORG_GRACGCRB = 1, PCTL_ORG_GSRMR = 2, PCTL_ORG_ESRB = 3, PCTL_ORG_CLASSIND = 4, PCTL_ORG_USK = 5, PCTL_ORG_PEGI = 6,
    PCTL_ORG_PEGI_PORTUGAL = 7, PCTL_ORG_PEGI_BBFC = 8, PCTL_ORG_RUSSIAN = 9, PCTL_ORG_ACB = 10, PCTL_ORG_OFLC = 11, PCTL_ORG_IARC = 12, PCTL_ORG_COUNT = 13
};

typedef struct {
    uint64_t application_id;
    uint8_t restricted;
} PctlFreeComm;

typedef struct {
    char pin[PCTL_PIN_MAX + 1];
    uint32_t pin_len;
    int temp_unlocked;
    int32_t rating_org;
    PctlFreeComm free_comm[PCTL_FREE_COMM_MAX];
    uint32_t nfree_comm;
    int64_t last_updated; /* PosixTime of the last change, 0 = never */
} PctlLocal;

/* Default rating organization of a set:sys region (0 JPN, 1 USA, 2 EUR, 3 AUS, 4 HTK, 5 CHN): CERO, ESRB, PEGI, ACB, GRAC (Korea), IARC. */
int32_t pctl_default_org(uint32_t region);
void pctl_init(PctlLocal *p, uint32_t region);

enum { PCTL_OK = 0, PCTL_ERR_INVALID = 1, PCTL_ERR_WRONG_PIN = 2, PCTL_ERR_FULL = 3 };
/* A PIN is 4..8 ASCII digits (the length the settings applet accepts); an empty one removes it. `n` bounds the read (NUL stops it early). */
int pctl_set_pin(PctlLocal *p, const char *pin, size_t n, int64_t now);
/* Compares `pin` with the stored one; with no PIN set every unlock succeeds. On success `temp` sets the temporary unlock. */
int pctl_unlock(PctlLocal *p, const char *pin, size_t n, int temp);
int pctl_set_org(PctlLocal *p, int32_t org, int64_t now);
/* Free-communication list: add (an id already listed keeps its entry; new entries start restricted), update the restricted flag of listed ids from
 * packed entries, reset (empty), and copy whole entries from `offset`. */
int pctl_free_comm_add(PctlLocal *p, uint64_t app, int64_t now);
uint32_t pctl_free_comm_update(PctlLocal *p, const uint8_t *entries, size_t size, int64_t now);
void pctl_free_comm_reset(PctlLocal *p, int64_t now);
uint32_t pctl_free_comm_get(const PctlLocal *p, uint32_t offset, uint8_t *out, size_t cap);
