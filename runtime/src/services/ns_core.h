/* ns core: application language selection and storage space (portable). */
#pragma once
#include <stdint.h>

/* nn::ns ApplicationLanguage */
enum {
    NS_LANG_AMERICAN_ENGLISH,
    NS_LANG_BRITISH_ENGLISH,
    NS_LANG_JAPANESE,
    NS_LANG_FRENCH,
    NS_LANG_GERMAN,
    NS_LANG_LATIN_AMERICAN_SPANISH,
    NS_LANG_SPANISH,
    NS_LANG_ITALIAN,
    NS_LANG_DUTCH,
    NS_LANG_CANADIAN_FRENCH,
    NS_LANG_PORTUGUESE,
    NS_LANG_RUSSIAN,
    NS_LANG_KOREAN,
    NS_LANG_TRADITIONAL_CHINESE,
    NS_LANG_SIMPLIFIED_CHINESE,
    NS_LANG_BRAZILIAN_PORTUGUESE,
    NS_LANG_COUNT
};

uint64_t ns_language_code(int lang);      /* "en-US" etc. as 8 ASCII bytes; 0 when out of range */
int ns_language_from_code(uint64_t code); /* -1 when unknown */
/* Chooses the title language: the desired language if the title supports it (bit n of `supported` = ApplicationLanguage n), else the closest
 * regional variant, else the lowest supported language. -1 when nothing is supported. The fallback pairs are host-chosen. */
int ns_select_language(uint32_t supported, uint64_t desired_code);

/* Free / total bytes of the volume holding `path`; 0 on error. */
uint64_t ns_free_space(const char *path);
uint64_t ns_total_space(const char *path);

/* ApplicationRecord (0x18 bytes: u64 application id, u8 record type 3 = installed, u8 unknown, 6 bytes padding, u8 unknown[8]); unverified layout. */
#define NS_APP_RECORD_SIZE 0x18u
uint64_t ns_parse_title_id(const char *hex); /* 16 hex digits -> id; 0 when malformed */
void ns_app_record(uint64_t id, uint8_t out[NS_APP_RECORD_SIZE]);

/* control.nacp: the title's NACP (0x4000 bytes) as `napple build` / the mod glue writes it next to the data directory (<data_dir>/control.nacp).
 * ns_control_path fills `out` (capacity `cap`) and returns 1 when the file exists and is a plausible NACP (1..NS_NACP_SIZE bytes); 0 otherwise.
 * ns_control_load reads it zero-padded to NS_NACP_SIZE into buf and returns the size (NS_NACP_SIZE) or 0 when absent or invalid. */
#define NS_NACP_SIZE 0x4000u
int ns_control_path(const char *data_dir, char *out, unsigned cap);
unsigned ns_control_load(const char *data_dir, uint8_t buf[NS_NACP_SIZE]);
