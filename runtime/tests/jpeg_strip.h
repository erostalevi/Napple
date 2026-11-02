/* Test helper: takes a JPEG file apart into what the NVJPG engine is programmed with (JpegRawParams + the entropy-coded segment). Written against the
 * JPEG marker syntax (ITU T.81 B.2) independently of jpeg_core.c's own parser, so a table-handling bug in the core shows up as a pixel mismatch. */
#pragma once
#include "../src/services/jpeg_core.h"
#include <string.h>

static int strip_jpeg(const uint8_t *d, size_t n, JpegRawParams *rp, const uint8_t **scan, size_t *scan_n) {
    memset(rp, 0, sizeof *rp);
    size_t p = 2;
    int ids[3] = {0}, seen_sof = 0;
    if (n < 4 || d[0] != 0xff || d[1] != 0xd8) return -1;
    while (p + 4 <= n) {
        if (d[p] != 0xff) return -1;
        int m = d[p + 1];
        size_t len = (size_t)(d[p + 2] << 8 | d[p + 3]), q = p + 4, end = p + 2 + len;
        if (end > n) return -1;
        if (m == 0xdb) {
            while (q < end) {
                int pq = d[q] >> 4, tq = d[q] & 15; q++;
                for (int i = 0; i < 64; i++) { rp->q[tq][i] = pq ? (uint16_t)(d[q] << 8 | d[q + 1]) : d[q]; q += pq ? 2 : 1; }
            }
        } else if (m == 0xc4) {
            while (q < end) {
                int tc = d[q] >> 4, th = d[q] & 15, tot = 0; q++;
                rp->huff[tc][th].bits[0] = 0;
                for (int i = 1; i <= 16; i++) tot += rp->huff[tc][th].bits[i] = d[q++];
                memcpy(rp->huff[tc][th].vals, d + q, (size_t)tot); q += (size_t)tot;
            }
        } else if (m == 0xc0) {
            rp->height = d[q + 1] << 8 | d[q + 2]; rp->width = d[q + 3] << 8 | d[q + 4]; rp->ncomp = d[q + 5];
            for (int i = 0; i < rp->ncomp; i++) { ids[i] = d[q + 6 + 3 * i]; rp->h[i] = d[q + 7 + 3 * i] >> 4; rp->v[i] = d[q + 7 + 3 * i] & 15; rp->tq[i] = d[q + 8 + 3 * i]; }
            seen_sof = 1;
        } else if (m == 0xdd) {
            rp->restart_interval = d[q] << 8 | d[q + 1];
        } else if (m == 0xda) {
            if (!seen_sof) return -1;
            for (int i = 0; i < d[q]; i++) { /* components appear in the scan header in frame order for every encoder the tests use */
                if (d[q + 1 + 2 * i] != ids[i]) return -1;
                rp->dc_t[i] = d[q + 2 + 2 * i] >> 4; rp->ac_t[i] = d[q + 2 + 2 * i] & 15;
            }
            *scan = d + end;
            *scan_n = n - end;
            if (*scan_n >= 2 && (*scan)[*scan_n - 2] == 0xff && (*scan)[*scan_n - 1] == 0xd9) *scan_n -= 2; /* the EOI is not entropy data */
            return 0;
        }
        p = end;
    }
    return -1;
}
