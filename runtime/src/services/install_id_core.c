/* Per-installation identity (install_id_core.h). */
#include "install_id_core.h"
#include "../sha256.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int install_id_get(const char *root, uint8_t out[16]) {
    static uint8_t fallback[16];
    static int have_fallback;
    char path[1024];
    if (root && *root && snprintf(path, sizeof path, "%s/system/installation-id", root) < (int)sizeof path) {
        FILE *f = fopen(path, "rb");
        if (f) {
            size_t n = fread(out, 1, 16, f);
            fclose(f);
            if (n == 16) return 0;
        }
        uint8_t id[16];
        arc4random_buf(id, sizeof id);
        char dir[1024];
        snprintf(dir, sizeof dir, "%s/system", root);
        mkdir(root, 0755);
        mkdir(dir, 0755);
        char tmp[1100];
        snprintf(tmp, sizeof tmp, "%s.%d", path, (int)getpid());
        int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) {
            int ok = write(fd, id, sizeof id) == (ssize_t)sizeof id;
            close(fd);
            /* link() fails when another process made the id meanwhile: then its id wins */
            if (ok && (link(tmp, path) == 0 || errno == EEXIST)) {
                unlink(tmp);
                FILE *g = fopen(path, "rb");
                if (g) {
                    size_t n = fread(out, 1, 16, g);
                    fclose(g);
                    if (n == 16) return 0;
                }
            }
            unlink(tmp);
        }
    }
    if (!have_fallback) arc4random_buf(fallback, sizeof fallback), have_fallback = 1;
    memcpy(out, fallback, 16);
    return -1;
}

void install_pseudo_device_id(const uint8_t inst[16], uint64_t app, const uint8_t user[16], uint8_t out[16]) {
    uint8_t in[16 + 8 + 16 + 16], h[32];
    memcpy(in, "napple-pseudo-id", 16);
    memcpy(in + 16, &app, 8);
    memcpy(in + 24, inst, 16);
    memcpy(in + 40, user, 16);
    sha256(in, sizeof in, h);
    memcpy(out, h, 16);
    out[6] = (uint8_t)((out[6] & 0x0f) | 0x40); /* version 4 */
    out[8] = (uint8_t)((out[8] & 0x3f) | 0x80); /* RFC 4122 variant */
}

void install_play_history_key(const uint8_t inst[16], const uint8_t user[16], int local, uint8_t out[0x40]) {
    memset(out, 0, 0x40);
    out[0] = 0x01, out[1] = 0x01; /* type 0x101 */
    uint8_t r;
    arc4random_buf(&r, 1);
    out[2] = r & 7; /* key index */
    out[4] = local ? 1 : 0;
    memcpy(out + 0x10, user, 16);
    uint8_t in[16 + 0x20];
    memcpy(in, inst, 16);
    memcpy(in + 16, out, 0x20);
    sha256(in, sizeof in, out + 0x20);
}
