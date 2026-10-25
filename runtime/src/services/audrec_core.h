/* audrec core: final-output recorder buffer bookkeeping (portable). The recorder captures the console's mixed output; the host does not tap
 * the mix, so a started recorder "records" silence and completes each appended buffer at once. End times are a running count of
 * recorded sample frames (what GetFinalOutputRecorderBufferEndTime reports). Wire layouts are from memory of the public docs (unverified). */
#pragma once
#include <stdint.h>

#define AUDREC_QUEUE_MAX 32
#define AUDREC_RATE 48000u
#define AUDREC_CHANNELS 2u
#define AUDREC_FRAME_BYTES 4u          /* PCM int16, 2 channels */
typedef struct { uint64_t tag, end_time, size; } AudrecBuf;
typedef struct {
    int started;                                    /* FinalOutputRecorderState: 0 Started, 1 Stopped on the wire */
    AudrecBuf queued[AUDREC_QUEUE_MAX]; unsigned nqueued;      /* appended while stopped */
    AudrecBuf released[AUDREC_QUEUE_MAX]; unsigned nreleased;  /* completed, not yet fetched */
    uint64_t frames;                                /* sample frames recorded so far */
} AudrecQueue;

void audrec_init(AudrecQueue *q);
void audrec_start(AudrecQueue *q);                  /* completes anything queued while stopped */
void audrec_stop(AudrecQueue *q);
int audrec_append(AudrecQueue *q, uint64_t tag, uint64_t size_bytes);   /* 0, or -1 when full; released at once if started */
unsigned audrec_release(AudrecQueue *q, uint64_t *tags, unsigned max, uint64_t *last_end_time);  /* oldest first */
int audrec_contains(const AudrecQueue *q, uint64_t tag);
int audrec_end_time(const AudrecQueue *q, uint64_t tag, uint64_t *end);  /* 0 if the tag is known (queued ones report 0) */
unsigned audrec_count(const AudrecQueue *q);
