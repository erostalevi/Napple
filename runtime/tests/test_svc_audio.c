/* Unit tests: audio service rows over libnapple_audio (null sink): device events, audout count/played/flush, manual-execution renderer. */
#include "../src/services/audio.c"
#include "svc_test.h"
#include <math.h>
#include <stdint.h>

/* host stand-ins (Linux only; on macOS the real memory model is linked): guest addresses are host pointers in these tests */
#ifdef __linux__
void *guest_ptr(uint64_t a, uint64_t n) { (void)n; return (void *)(uintptr_t)a; }
void gfx_guest_write_prepare(void *p, size_t n) { (void)p; (void)n; }
uint8_t *memory_a32_base(void) { return NULL; }
#endif

int main(void) {
    g_cfg.headless = 1; /* null sink */
    IpcObject *dev = make_device(); IpcCall c;
    svc_call(dev, 4, NULL, 0, &c); uint32_t ev = c.out_copy[0];
    svc_call(dev, 11, NULL, 0, &c); uint32_t evi = c.out_copy[0];
    CHECK(!event_is_signaled(ev) && !event_is_signaled(evi), "device events start clear");
    audio_device_changed();
    CHECK(event_is_signaled(ev) && event_is_signaled(evi), "a device change signals every device event");
    svc_call(dev, 13, NULL, 0, &c); CHECK(SVC_OUT(&c, float, 0) == 1.0f, "master volume setting");
    { /* 13.0.0+: 13 with an output buffer is GetActiveAudioOutputDeviceName; 14-21 */
        char nm[0x100] = {0}; memset(&c, 0, sizeof c); svc_set_out_buf(&c, nm, sizeof nm); svc_call_c(dev, 13, &c);
        CHECK(c.out_size == 0 && nm[0] != 0, "13 with an out buffer writes the active output device name");
        memset(nm, 0, sizeof nm); memset(&c, 0, sizeof c); svc_set_out_buf(&c, nm, sizeof nm); svc_call_c(dev, 14, &c);
        CHECK(SVC_OUT(&c, int32_t, 0) == 1 && nm[0] != 0, "ListAudioOutputDeviceName");
        svc_call(dev, 17, NULL, 0, &c); uint32_t evo = c.out_copy[0]; audio_device_changed(); CHECK(event_is_signaled(evo), "output device notification fires on a change");
        uint8_t on = 1; svc_call(dev, 19, &on, 1, &c); svc_call(dev, 20, NULL, 0, &c); CHECK(c.out[0] == 1, "volume auto-tune flag round trip");
        svc_call(dev, 21, NULL, 0, &c); CHECK(c.out_size == 1, "IsActiveOutputDeviceEstimatedLowLatency answers");
    }

#ifdef __linux__ /* guest buffers are host pointers here; macOS would need a mapped guest region */
    /* audout: open through the manager so the table is the real one */
    IpcObject *am = audout_open();
    uint8_t in[16] = {0}; uint32_t rate = 48000, ch = 2; memcpy(in, &rate, 4); memcpy(in + 4, &ch, 2);
    memset(&c, 0, sizeof c); c.in = in; c.in_size = 8;
    const IpcCmd *e = svc_find(am, 1); CHECK(e != NULL, "OpenAudioOut present"); if (!e) return 1;
    e->fn(am, &c); CHECK(c.result == 0 && c.nout_objects == 1, "open audio out (result %#x)", c.result);
    if (!c.nout_objects) { printf("{\"test\":\"svc_audio\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed); return 1; }
    IpcObject *o = c.out_objects[0];
    napple_audout_set_manual(((AudOut *)o->state)->o, 1);

    static int16_t pcm[2 * 480];
    uint64_t desc[5] = {0, (uint64_t)(uintptr_t)pcm, sizeof pcm, sizeof pcm, 0};
    for (int i = 0; i < 2; i++) {
        uint64_t tag = 100 + i; memset(&c, 0, sizeof c); svc_set_in_buf(&c, desc, sizeof desc); c.in = (const uint8_t *)&tag; c.in_size = 8; svc_call_c(o, 3, &c);
        CHECK(c.result == 0, "append %d", i);
    }
    svc_call(o, 9, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 2, "GetAudioOutBufferCount: two queued");
    svc_call(o, 1, NULL, 0, &c); napple_audout_pump(((AudOut *)o->state)->o, 480 + 100);
    svc_call(o, 10, NULL, 0, &c); CHECK(SVC_OUT(&c, uint64_t, 0) == 580, "GetAudioOutPlayedSampleCount");
    svc_call(o, 9, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 2, "one released-not-fetched, one queued");
    svc_call(o, 11, NULL, 0, &c); CHECK(c.out[0] == 1, "FlushAudioOutBuffers reports a flush");
    svc_call(o, 11, NULL, 0, &c); CHECK(c.out[0] == 0, "second flush has nothing to do");
    svc_call(o, 10, NULL, 0, &c); CHECK(SVC_OUT(&c, uint64_t, 0) == 580, "flush does not count as played");
    svc_call(o, 13, NULL, 0, &c); CHECK(c.result == 0 && SVC_OUT(&c, float, 0) == 1.0f, "audout volume starts at 1.0");
    float vol = 0.25f; svc_call(o, 12, &vol, 4, &c); svc_call(o, 13, NULL, 0, &c); CHECK(SVC_OUT(&c, float, 0) == 0.25f, "audout volume round trip");
    CHECK(napple_audout_get_volume(((AudOut *)o->state)->o) == 0.25f, "...the stream's own gain in libnapple_audio is the same value");
    vol = 7.0f; svc_call(o, 12, &vol, 4, &c); svc_call(o, 13, NULL, 0, &c); CHECK(SVC_OUT(&c, float, 0) == 1.0f, "SetAudioOutVolume above 1 clamps to 1.0");
    vol = -3.0f; svc_call(o, 12, &vol, 4, &c); svc_call(o, 13, NULL, 0, &c); CHECK(SVC_OUT(&c, float, 0) == 0.0f, "...below 0 clamps to 0.0");
    svc_call(o, 12, NULL, 0, &c); svc_call(o, 13, NULL, 0, &c); CHECK(SVC_OUT(&c, float, 0) == 0.0f, "...a request without a value changes nothing");
    vol = 0.5f; svc_call(o, 12, &vol, 4, &c);
    CHECK(svc_find(o, 12)->status == IPC_IMPL && svc_find(o, 13)->status == IPC_IMPL, "IAudioOut 12 and 13 are real now");

    /* audin: the first open installs the source named by NAPPLE_AUDIN_SOURCE and the buffers carry it */
    napple_audin_set_source(NULL, NULL);
    setenv("NAPPLE_AUDIN_SOURCE", "tone:1000:0.5", 1);
    IpcObject *im = audin_open();
    uint8_t iin[16] = {0}; uint32_t irate = 48000, ich = 1; memcpy(iin, &irate, 4); memcpy(iin + 4, &ich, 2);
    memset(&c, 0, sizeof c); c.in = iin; c.in_size = 8; svc_find(im, 1)->fn(im, &c);
    CHECK(c.result == 0 && c.nout_objects == 1 && napple_audin_has_source(), "OpenAudioIn installs the env capture source (result %#x)", c.result);
    if (c.nout_objects) {
        IpcObject *ai = c.out_objects[0];
        napple_audin_set_manual(((AudIn *)ai->state)->i, 1);
        static int16_t mic[480];
        uint64_t idesc[5] = {0, (uint64_t)(uintptr_t)mic, sizeof mic, sizeof mic, 0};
        uint64_t itag = 0x77; memset(&c, 0, sizeof c); svc_set_in_buf(&c, idesc, sizeof idesc); c.in = (const uint8_t *)&itag; c.in_size = 8; svc_call_c(ai, 3, &c);
        CHECK(c.result == 0, "audin append");
        svc_call(ai, 1, NULL, 0, &c); napple_audin_pump(((AudIn *)ai->state)->i, 480);
        int ok = 1; for (int k = 0; k < 480; k++) { int want = (int)lrint(0.5 * sin(2 * M_PI * 1000.0 * k / 48000.0) * 32768.0); ok &= abs(mic[k] - want) <= 1; }
        CHECK(ok, "the guest buffer holds the tone");
        uint64_t rel[4] = {0}; memset(&c, 0, sizeof c); svc_set_out_buf(&c, rel, sizeof rel); svc_call_c(ai, 5, &c);
        CHECK(SVC_OUT(&c, uint32_t, 0) == 1 && rel[0] == 0x77, "the filled buffer is released with its tag");
    }
    napple_audin_set_source(NULL, NULL);
    unsetenv("NAPPLE_AUDIN_SOURCE");
#endif
    /* manual execution: the non-manual renderer refuses ExecuteAudioRendererRendering, the manual one renders */
    Renderer plain = {0};
    IpcObject fake = {"IAudioRenderer", rend_cmds, sizeof rend_cmds / sizeof rend_cmds[0], &plain, NULL, 1};
    svc_call(&fake, 11, NULL, 0, &c); CHECK(c.result != 0, "ExecuteAudioRendererRendering on a threaded renderer is an error");
    float drop = 0.5f; svc_call(&fake, 12, &drop, 4, &c); svc_call(&fake, 13, NULL, 0, &c); CHECK(SVC_OUT(&c, float, 0) == 0.5f, "voice drop parameter round trip");
    printf("{\"test\":\"svc_audio\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
