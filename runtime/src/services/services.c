/* Registration of every host service (docs/runtime-services.md). */
#include "../ipc.h"
#include "../frontend.h"
#include "napple_input.h"
#include <stdlib.h>
#include <string.h>

void service_register_lm(void);
void service_register_apm(void);
void service_register_applet(void);
void service_register_fs(void);
void service_register_settings(void);
void service_register_account(void);
void service_register_misc(void);
void service_register_time(void);
void service_register_hid(void);
void service_register_vi(void);
void service_register_pl(void);
void service_register_offline(void);
void service_register_nvdrv(void);
void service_register_audio(void);
void service_register_ovln(void);
void service_register_btm(void);
void service_register_ngc(void);
void service_register_fgm(void);
void service_register_ectx(void);
void service_register_olsc(void);
void service_register_push(void);
void service_register_setfd(void);
void service_register_pdm(void);
void service_register_hidbus(void);
void service_register_audin(void);
void service_register_irs(void);
void service_register_ldn(void);
void service_register_bcat(void);
void service_register_spsm(void);
void service_register_sfdnsres(void);
void service_register_ns_vm(void);
void service_register_ns(void);
void service_register_ncm(void);
void service_register_caps(void);
void service_register_mii(void);
void service_register_lbl(void);
void service_register_psm(void);
void service_register_audrec(void);

/* Console operation mode (docked 1080p / handheld 720p): input.json "operation_mode", NAPPLE_OP_MODE=docked|handheld overrides. Fixed for the
 * life of the process (a change needs a restart); the guest reads it through appletOE, apm and vi. */
static void init_operation_mode(void) {
    const char *e = getenv("NAPPLE_OP_MODE");
    if (e && *e) g_cfg.op_mode_handheld = !strcmp(e, "handheld");
    else g_cfg.op_mode_handheld = napple_input_operation_mode(hid_input_get()) == 1;
    LOG("main", "operation mode: %s", g_cfg.op_mode_handheld ? "handheld (720p)" : "docked (1080p)");
}

void services_init(void) {
    init_operation_mode();
    service_register_lm();
    service_register_apm();
    service_register_applet();
    service_register_fs();
    service_register_settings();
    service_register_account();
    service_register_misc();
    service_register_time();
    service_register_hid();
    service_register_vi();
    service_register_pl();
    service_register_offline();
    service_register_nvdrv();
    service_register_audio();
    service_register_ovln();
    service_register_btm();
    service_register_ngc();
    service_register_fgm();
    service_register_ectx();
    service_register_olsc();
    service_register_push();
    service_register_setfd();
    service_register_pdm();
    service_register_hidbus();
    /* audin:u is registered by audio.c (host capture source); audin.c stays for its unit test only. */
    service_register_irs();
    service_register_ldn();
    service_register_bcat();
    service_register_spsm();
    service_register_sfdnsres();
    service_register_ns_vm();
    service_register_ns();
    service_register_ncm();
    service_register_caps();
    service_register_mii();
    service_register_lbl();
    service_register_psm();
    service_register_audrec();
}
