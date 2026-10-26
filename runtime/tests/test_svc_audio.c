/* Unit tests: audio service rows over libnapple_audio (null sink): device events, audout count/played/flush, manual-execution renderer. */
#include "../src/services/audio.c"
#include "svc_test.h"
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

#endif
    /* manual execution: the non-manual renderer refuses ExecuteAudioRendererRendering, the manual one renders */
    Renderer plain = {0};
    IpcObject fake = {"IAudioRenderer", rend_cmds, sizeof rend_cmds / sizeof rend_cmds[0], &plain, NULL, 1};
    svc_call(&fake, 11, NULL, 0, &c); CHECK(c.result != 0, "ExecuteAudioRendererRendering on a threaded renderer is an error");
    printf("{\"test\":\"svc_audio\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
