/* sim_shared.m - Foundation / AVFoundation parts of the hardware simulators shared by the iOS panel (the Mac overlay has its own copy in simulators.m):
   the amiibo library loader (cache first, then a background fetch, repository database converted to the API document) and the webcam IR source.
   Runtime symbols are weak references so the frontend library still links without the runtime (tests). */
#import <AVFoundation/AVFoundation.h>
#include <pthread.h>
#include "amiibo_lib_core.h"
#include "irs_core.h"
#include "sim_shared.h"

extern AmiiboPicker *amiibo_service_picker(void) __attribute__((weak));
extern void nfp_service_set_source(const NfcTagSource *src) __attribute__((weak));
extern const NfcTagSource *amiibo_picker_source(AmiiboPicker *p) __attribute__((weak));
extern size_t amiibo_picker_count(const AmiiboPicker *p) __attribute__((weak));
extern const AmiiboInfo *amiibo_picker_get(const AmiiboPicker *p, size_t i) __attribute__((weak));
extern size_t amiibo_picker_search(const AmiiboPicker *p, const char *query, size_t *idx, size_t max) __attribute__((weak));
extern int amiibo_picker_select_hex(AmiiboPicker *p, const char *id_hex, int flags) __attribute__((weak));
extern int amiibo_picker_import_dump(AmiiboPicker *p, const char *dump_path) __attribute__((weak));
extern void amiibo_picker_remove(AmiiboPicker *p) __attribute__((weak));
extern int amiibo_picker_present(AmiiboPicker *p) __attribute__((weak));
extern int amiibo_lib_refresh(AmiiboLib *l, const AmiiboConfig *c, const char *fetched, size_t n) __attribute__((weak));
extern int amiibo_config_source_is_remote(const AmiiboConfig *c) __attribute__((weak));
extern void irs_service_set_source(const IrFrameSource *src) __attribute__((weak));
extern int irs_adapter_rgb_to_ir(const uint8_t *src, int w, int h, int stride, int channels, int bgr, const IrAdapterOpts *opts, uint8_t *out)
    __attribute__((weak));

/* amiiboapi.com answers 404 since the project was archived; the repository database is the working default. */
#define AMIIBO_REPO_DATABASE "https://raw.githubusercontent.com/N3evin/AmiiboAPI/master/database/amiibo.json"
#define AMIIBO_REPO_IMAGES "https://raw.githubusercontent.com/N3evin/AmiiboAPI/master/images/icon_%@-%@.png"

/* The repository database ({"amiibos":{"0x<id>":{name}}, "characters", "amiibo_series", "game_series", "types"}) converted to the API document the
   runtime parser reads ({"amiibo":[{head,tail,name,character,amiiboSeries,gameSeries,type,image}]}). Other inputs are returned as they are. */
static NSData *amiibo_convert_database(NSData *data) {
    NSDictionary *root = [NSJSONSerialization JSONObjectWithData:data options:0 error:NULL];
    if (![root isKindOfClass:NSDictionary.class] || ![root[@"amiibos"] isKindOfClass:NSDictionary.class]) return data;
    NSDictionary *chars = root[@"characters"], *series = root[@"amiibo_series"], *games = root[@"game_series"], *types = root[@"types"];
    NSMutableArray *out = [NSMutableArray new];
    [root[@"amiibos"] enumerateKeysAndObjectsUsingBlock:^(NSString *k, NSDictionary *v, BOOL *stop) {
        (void)stop;
        NSString *id = [k.lowercaseString hasPrefix:@"0x"] ? [k.lowercaseString substringFromIndex:2] : k.lowercaseString;
        if (id.length != 16 || ![v isKindOfClass:NSDictionary.class] || ![v[@"name"] isKindOfClass:NSString.class]) return;
        NSString *head = [id substringToIndex:8], *tail = [id substringFromIndex:8];
        NSString *(^look)(NSDictionary *, NSString *) = ^NSString *(NSDictionary *t, NSString *key) {
            NSString *x = [t isKindOfClass:NSDictionary.class] ? t[[@"0x" stringByAppendingString:key]] : nil;
            return [x isKindOfClass:NSString.class] ? x : @"";
        };
        [out addObject:@{
            @"head" : head, @"tail" : tail, @"name" : v[@"name"], @"character" : look(chars, [id substringToIndex:4]),
            @"amiiboSeries" : look(series, [tail substringWithRange:NSMakeRange(4, 2)]), @"gameSeries" : look(games, [id substringToIndex:3]),
            @"type" : look(types, [head substringWithRange:NSMakeRange(6, 2)]), @"image" : [NSString stringWithFormat:@AMIIBO_REPO_IMAGES, head, tail]
        }];
    }];
    if (!out.count) return data;
    return [NSJSONSerialization dataWithJSONObject:@{@"amiibo" : out} options:0 error:NULL] ?: data;
}

void sim_amiibo_load(AmiiboPicker *p, BOOL fetch, void (^done)(NSString *status)) {
    if (!p || !amiibo_lib_refresh) {
        if (done) done(@"Amiibo support is not available in this build.");
        return;
    }
    pthread_mutex_lock(&p->lock);
    amiibo_lib_refresh(&p->lib, &p->cfg, NULL, 0);
    pthread_mutex_unlock(&p->lock);
    if (done) done(amiibo_picker_count(p) ? nil : @"The amiibo list is empty: it is downloaded once and cached.");
    const char *src = p->cfg.source;
    if (!fetch || !amiibo_config_source_is_remote(&p->cfg)) return;
    if (!getenv("NAPPLE_AMIIBO_DB") && !strcmp(src, AMIIBO_DEFAULT_SOURCE)) src = AMIIBO_REPO_DATABASE;
    NSURL *u = [NSURL URLWithString:@(src)];
    if (!u) return;
    [[[NSURLSession sharedSession] dataTaskWithURL:u
                                 completionHandler:^(NSData *data, NSURLResponse *resp, NSError *err) {
                                     NSInteger code = [resp isKindOfClass:NSHTTPURLResponse.class] ? ((NSHTTPURLResponse *)resp).statusCode : 200;
                                     dispatch_async(dispatch_get_main_queue(), ^{
                                         if (!err && data.length && code < 400) {
                                             NSData *doc = amiibo_convert_database(data);
                                             pthread_mutex_lock(&p->lock);
                                             amiibo_lib_refresh(&p->lib, &p->cfg, doc.bytes, doc.length);
                                             pthread_mutex_unlock(&p->lock);
                                             if (done) done(nil);
                                         } else if (done && !amiibo_picker_count(p))
                                             done([NSString stringWithFormat:@"Could not download the amiibo list (%@).", err ? err.localizedDescription : [NSString stringWithFormat:@"HTTP %ld", (long)code]]);
                                     });
                                 }] resume];
}

@interface FECamera : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@property(nonatomic, strong) AVCaptureSession *session;
@property(nonatomic, strong) dispatch_queue_t queue;
@property(nonatomic) int mirror, running;
@property(nonatomic, copy) NSString *deviceName;
@end
@implementation FECamera {
    pthread_mutex_t _lock;
    uint8_t _frame[IR_W * IR_H];
    uint64_t _ts;
    int _fresh;
}
- (instancetype)init {
    if ((self = [super init])) {
        pthread_mutex_init(&_lock, NULL);
        _queue = dispatch_queue_create("app.napple.ircam", DISPATCH_QUEUE_SERIAL);
    }
    return self;
}
- (int)start {
    if (self.session) {
        if (!self.session.running) [self.session startRunning];
        return 0;
    }
    AVCaptureDevice *dev = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
    if (!dev) return -1;
    NSError *err = nil;
    AVCaptureDeviceInput *in = [AVCaptureDeviceInput deviceInputWithDevice:dev error:&err];
    if (!in) return -1;
    AVCaptureSession *s = [AVCaptureSession new];
    s.sessionPreset = AVCaptureSessionPreset640x480;
    if (![s canAddInput:in]) return -1;
    [s addInput:in];
    AVCaptureVideoDataOutput *out = [AVCaptureVideoDataOutput new];
    out.videoSettings = @{(id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA)};
    out.alwaysDiscardsLateVideoFrames = YES;
    [out setSampleBufferDelegate:self queue:self.queue];
    if (![s canAddOutput:out]) return -1;
    [s addOutput:out];
    self.session = s;
    self.deviceName = dev.localizedName;
    [s startRunning];
    return 0;
}
- (void)stop {
    if (self.session.running) [self.session stopRunning];
}
- (void)captureOutput:(AVCaptureOutput *)o didOutputSampleBuffer:(CMSampleBufferRef)buf fromConnection:(AVCaptureConnection *)conn {
    (void)o;
    (void)conn;
    CVImageBufferRef px = CMSampleBufferGetImageBuffer(buf);
    if (!px || !irs_adapter_rgb_to_ir) return;
    CVPixelBufferLockBaseAddress(px, kCVPixelBufferLock_ReadOnly);
    IrAdapterOpts opts = {.mirror = self.mirror, .stretch = 1, .threshold = 0, .binarize = 0};
    uint8_t tmp[IR_W * IR_H];
    int ok = irs_adapter_rgb_to_ir(CVPixelBufferGetBaseAddress(px), (int)CVPixelBufferGetWidth(px), (int)CVPixelBufferGetHeight(px),
                                   (int)CVPixelBufferGetBytesPerRow(px), 4, 1, &opts, tmp);
    CVPixelBufferUnlockBaseAddress(px, kCVPixelBufferLock_ReadOnly);
    if (ok != 0) return;
    pthread_mutex_lock(&_lock);
    memcpy(_frame, tmp, sizeof _frame);
    _ts = (uint64_t)(CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(buf)) * 1e9);
    _fresh = 1;
    pthread_mutex_unlock(&_lock);
}
- (int)copyFrame:(uint8_t *)out ts:(uint64_t *)ts {
    pthread_mutex_lock(&_lock);
    int f = _fresh;
    if (f) {
        memcpy(out, _frame, sizeof _frame);
        if (ts) *ts = _ts;
        _fresh = 0;
    }
    pthread_mutex_unlock(&_lock);
    return f;
}
@end


static FECamera *g_cam;
static BOOL g_cam_installed;
static void (^g_cam_changed)(void);
static int cam_request_access(void *ctx, void (*done)(void *, int), void *cbctx) {
    (void)ctx;
    switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo]) {
    case AVAuthorizationStatusAuthorized: return IR_ACCESS_GRANTED;
    case AVAuthorizationStatusNotDetermined:
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo
                                 completionHandler:^(BOOL granted) {
                                     if (done) done(cbctx, granted ? 1 : 0);
                                     dispatch_async(dispatch_get_main_queue(), ^{ if (g_cam_changed) g_cam_changed(); });
                                 }];
        return IR_ACCESS_PENDING;
    default: return IR_ACCESS_DENIED;
    }
}
static int cam_start(void *ctx) {
    (void)ctx;
    __block int r = 0;
    if (NSThread.isMainThread) r = [g_cam start];
    else dispatch_sync(dispatch_get_main_queue(), ^{ r = [g_cam start]; });
    dispatch_async(dispatch_get_main_queue(), ^{ if (g_cam_changed) g_cam_changed(); });
    return r;
}
static void cam_stop(void *ctx) {
    (void)ctx;
    dispatch_async(dispatch_get_main_queue(), ^{
        [g_cam stop];
        if (g_cam_changed) g_cam_changed();
    });
}
static int cam_get_frame(void *ctx, uint8_t *out, uint64_t *ts) {
    (void)ctx;
    return [g_cam copyFrame:out ts:ts];
}
static const IrFrameSource g_cam_source = {
    .ctx = NULL, .name = "webcam", .request_access = cam_request_access, .start = cam_start, .stop = cam_stop, .get_frame = cam_get_frame};

void sim_camera_install(void) {
    if (g_cam_installed || !irs_service_set_source || !irs_adapter_rgb_to_ir) return;
    if (!g_cam) g_cam = [FECamera new];
    irs_service_set_source(&g_cam_source);
    g_cam_installed = YES;
}
BOOL sim_camera_available(void) { return irs_service_set_source && irs_adapter_rgb_to_ir; }
int sim_camera_auth(void) {
    switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo]) {
    case AVAuthorizationStatusAuthorized: return 1;
    case AVAuthorizationStatusDenied: return 2;
    case AVAuthorizationStatusRestricted: return 3;
    default: return 0;
    }
}
BOOL sim_camera_running(void) { return g_cam.session.running; }
NSString *sim_camera_name(void) { return g_cam.deviceName; }
BOOL sim_camera_mirror(void) { return g_cam.mirror != 0; }
void sim_camera_set_mirror(BOOL on) { g_cam.mirror = on; }
void sim_camera_request(void) { cam_request_access(NULL, NULL, NULL); }
void sim_camera_on_change(void (^cb)(void)) { g_cam_changed = [cb copy]; }

/* ---- amiibo picker helpers for the iOS panel ---- */
AmiiboPicker *sim_amiibo_picker(void) { return amiibo_service_picker ? amiibo_service_picker() : NULL; }
static BOOL g_reader_installed;
void sim_amiibo_install_reader(AmiiboPicker *p) {
    if (g_reader_installed || !p || !nfp_service_set_source || !amiibo_picker_source) return;
    nfp_service_set_source(amiibo_picker_source(p));
    g_reader_installed = YES;
}
size_t sim_amiibo_count(AmiiboPicker *p) { return p && amiibo_picker_count ? amiibo_picker_count(p) : 0; }
const AmiiboInfo *sim_amiibo_get(AmiiboPicker *p, size_t i) { return amiibo_picker_get(p, i); }
size_t sim_amiibo_search(AmiiboPicker *p, const char *q, size_t *idx, size_t max) { return amiibo_picker_search(p, q, idx, max); }
int sim_amiibo_place(AmiiboPicker *p, const char *hex, int fresh) {
    sim_amiibo_install_reader(p);
    return amiibo_picker_select_hex(p, hex, fresh ? AMIIBO_PICK_NEW : AMIIBO_PICK_REUSE);
}
void sim_amiibo_remove(AmiiboPicker *p) { amiibo_picker_remove(p); }
int sim_amiibo_present(AmiiboPicker *p) { return p && amiibo_picker_present ? amiibo_picker_present(p) : 0; }
