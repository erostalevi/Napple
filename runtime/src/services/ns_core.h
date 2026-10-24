/* ns core: application language selection and storage space (portable). */
#pragma once
#include <stdint.h>

/* nn::ns ApplicationLanguage */
enum { NS_LANG_AMERICAN_ENGLISH, NS_LANG_BRITISH_ENGLISH, NS_LANG_JAPANESE, NS_LANG_FRENCH, NS_LANG_GERMAN, NS_LANG_LATIN_AMERICAN_SPANISH, NS_LANG_SPANISH,
       NS_LANG_ITALIAN, NS_LANG_DUTCH, NS_LANG_CANADIAN_FRENCH, NS_LANG_PORTUGUESE, NS_LANG_RUSSIAN, NS_LANG_KOREAN, NS_LANG_TRADITIONAL_CHINESE,
       NS_LANG_SIMPLIFIED_CHINESE, NS_LANG_BRAZILIAN_PORTUGUESE, NS_LANG_COUNT };

uint64_t ns_language_code(int lang);                 /* "en-US" etc. as 8 ASCII bytes; 0 when out of range */
int ns_language_from_code(uint64_t code);            /* -1 when unknown */
/* Chooses the title language: the desired language if the title supports it (bit n of `supported` = ApplicationLanguage n), else the closest
 * regional variant, else the lowest supported language. -1 when nothing is supported. The fallback pairs are host-chosen. */
int ns_select_language(uint32_t supported, uint64_t desired_code);

/* Free / total bytes of the volume holding `path`; 0 on error. */
uint64_t ns_free_space(const char *path);
uint64_t ns_total_space(const char *path);
