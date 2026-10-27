/* Shared hardware-simulator helpers (sim_shared.m): amiibo picker access + library loader, webcam IR source. UIKit / AppKit free. */
#ifndef NAPPLE_SIM_SHARED_H
#define NAPPLE_SIM_SHARED_H
#import <Foundation/Foundation.h>
#include "amiibo_lib_core.h"

AmiiboPicker *sim_amiibo_picker(void);
/* Loads the library (cache / local source first, then a background fetch of the configured URL). done(nil) = fine; done(text) = a problem to show. May be called twice. */
void sim_amiibo_load(AmiiboPicker *p, BOOL fetch, void (^done)(NSString *status));
size_t sim_amiibo_count(AmiiboPicker *p);
const AmiiboInfo *sim_amiibo_get(AmiiboPicker *p, size_t i);
size_t sim_amiibo_search(AmiiboPicker *p, const char *query, size_t *idx, size_t max);
/* Places an amiibo (installs the reader on first use). 0 ok. */
int sim_amiibo_place(AmiiboPicker *p, const char *id_hex, int fresh);
void sim_amiibo_remove(AmiiboPicker *p);
int sim_amiibo_present(AmiiboPicker *p);

void sim_camera_install(void);
BOOL sim_camera_available(void);
int sim_camera_auth(void); /* 0 not asked, 1 allowed, 2 denied, 3 restricted */
BOOL sim_camera_running(void);
NSString *sim_camera_name(void);
BOOL sim_camera_mirror(void);
void sim_camera_set_mirror(BOOL on);
void sim_camera_request(void);
void sim_camera_on_change(void (^cb)(void));
#endif
