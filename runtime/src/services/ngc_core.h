/* ngc core: the profanity filter's word matcher (portable). The word lists are not shipped; a list the user supplies (NAPPLE_NGC_WORDLIST, one
 * word per line, '#' comments) makes Check flag and Mask replace matching text. Text is UTF-16 code units (the filter works on UTF-16), compared
 * ASCII case-insensitively; a word matches as a substring (unverified against the console's own matching rules). With no list nothing matches. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define NGC_MAX_WORDS 4096
#define NGC_MAX_WORD_LEN 32
typedef struct {
    uint16_t w[NGC_MAX_WORDS][NGC_MAX_WORD_LEN];
    uint8_t len[NGC_MAX_WORDS];
    unsigned count;
    uint32_t version; /* bumped on every (re)load, reported by GetContentVersion */
} NgcWords;

void ngc_words_clear(NgcWords *);
/* Loads words from a text buffer (one per line). Returns the number of words kept. */
unsigned ngc_words_load_text(NgcWords *, const char *text, size_t n);
/* Loads the file named by NAPPLE_NGC_WORDLIST; no variable or no file leaves an empty list. Returns the number of words. */
unsigned ngc_words_load_env(NgcWords *);
/* Number of matches of any word in `text` (n UTF-16 units). */
unsigned ngc_check(const NgcWords *, const uint16_t *text, size_t n);
/* Replaces every matched unit with '*'; returns the number of units masked. */
size_t ngc_mask(const NgcWords *, uint16_t *text, size_t n);
