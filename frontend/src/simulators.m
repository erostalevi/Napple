/* In-game overlay pages for the hardware simulators (docs/guides/hardware-simulators.md, "Mac wiring list"):
   the amiibo picker (nfp/nfc tag source) and the IR camera (webcam -> IrFrameSource).
   The frontend is a static library linked into the runtime; the runtime symbols are weak references so the frontend's own
   test binaries (no runtime) still link: with the symbols absent the pages say the feature is unavailable. */
#import <AVFoundation/AVFoundation.h>
#include <pthread.h>
#include "fe_internal.h"
#include "amiibo_lib_core.h"
#include "irs_core.h"

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

static NSTextField *small_note(NSString *t) {
    NSTextField *n = fe_label(t, 10, NO);
    n.textColor = NSColor.secondaryLabelColor;
    n.lineBreakMode = NSLineBreakByWordWrapping;
    n.preferredMaxLayoutWidth = 300;
    return n;
}
static NSStackView *vpage(void) {
    NSStackView *s = [NSStackView new];
    s.orientation = NSUserInterfaceLayoutOrientationVertical;
    s.alignment = NSLayoutAttributeLeading;
    s.spacing = 8;
    s.edgeInsets = NSEdgeInsetsMake(12, 12, 12, 12);
    return s;
}
static NSStackView *hrow(NSArray<NSView *> *views) {
    NSStackView *r = [NSStackView stackViewWithViews:views];
    r.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    r.spacing = 6;
    return r;
}
static NSButton *button(NSString *title, void (^blk)(id)) {
    NSButton *b = [NSButton buttonWithTitle:title target:nil action:nil];
    b.bezelStyle = NSBezelStyleRounded;
    b.controlSize = NSControlSizeSmall;
    b.font = [NSFont systemFontOfSize:11];
    fe_bind(b, blk);
    return b;
}

/* ===================================================================== amiibo ===================================================================== */

/* Table data source over the picker's search results. */
@interface FEAmiiboList : NSObject <NSTableViewDataSource, NSTableViewDelegate>
@property(nonatomic) AmiiboPicker *picker;
@property(nonatomic, strong) NSMutableArray<NSNumber *> *rows; /* indices into the library */
@property(nonatomic, weak) NSTableView *table;
@property(nonatomic, weak) NSTextField *status;
- (void)search:(NSString *)q;
@end
@implementation FEAmiiboList
- (void)search:(NSString *)q {
    self.rows = [NSMutableArray new];
    if (!self.picker) return;
    size_t cap = amiibo_picker_count(self.picker);
    if (!cap) return;
    size_t *idx = calloc(cap, sizeof *idx);
    size_t n = amiibo_picker_search(self.picker, q.UTF8String ?: "", idx, cap);
    for (size_t i = 0; i < n && i < cap; i++) [self.rows addObject:@(idx[i])];
    free(idx);
    [self.table reloadData];
}
- (NSInteger)numberOfRowsInTableView:(NSTableView *)t {
    (void)t;
    return (NSInteger)self.rows.count;
}
- (NSView *)tableView:(NSTableView *)t viewForTableColumn:(NSTableColumn *)col row:(NSInteger)row {
    const AmiiboInfo *a = amiibo_picker_get(self.picker, self.rows[(NSUInteger)row].unsignedLongValue);
    NSString *text = @"";
    if (a) text = [col.identifier isEqualToString:@"name"] ? @(a->name) : [NSString stringWithFormat:@"%s  %s", a->series, a->type];
    NSTableCellView *cell = [t makeViewWithIdentifier:col.identifier owner:self];
    if (!cell) {
        cell = [NSTableCellView new];
        cell.identifier = col.identifier;
        NSTextField *l = fe_label(@"", 11, NO);
        l.lineBreakMode = NSLineBreakByTruncatingTail;
        l.translatesAutoresizingMaskIntoConstraints = NO;
        [cell addSubview:l];
        cell.textField = l;
        [NSLayoutConstraint activateConstraints:@[
            [l.leadingAnchor constraintEqualToAnchor:cell.leadingAnchor constant:2], [l.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor constant:-2],
            [l.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor]
        ]];
    }
    cell.textField.stringValue = text;
    cell.textField.textColor = [col.identifier isEqualToString:@"name"] ? NSColor.labelColor : NSColor.secondaryLabelColor;
    return cell;
}
- (const AmiiboInfo *)selected {
    NSInteger r = self.table.selectedRow;
    if (r < 0 || (NSUInteger)r >= self.rows.count) return NULL;
    return amiibo_picker_get(self.picker, self.rows[(NSUInteger)r].unsignedLongValue);
}
@end

static FEAmiiboList *g_list;
static BOOL g_amiibo_installed, g_fetching;

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

/* The reader appears to the game only once the user places something: until then nfp/nfc keep answering "no device" (opening the tab changes nothing;
   Link's Awakening aborted at boot when the reader was installed while the tab was merely open). */
static void amiibo_install_reader(AmiiboPicker *picker) {
    if (g_amiibo_installed || !nfp_service_set_source || !amiibo_picker_source) return;
    nfp_service_set_source(amiibo_picker_source(picker));
    g_amiibo_installed = YES;
}

static void amiibo_status(NSTextField *status, AmiiboPicker *p) {
    if (!status) return;
    if (!p) {
        status.stringValue = @"Amiibo support is not available in this build.";
        return;
    }
    size_t n = amiibo_picker_count(p);
    NSString *where = amiibo_picker_present(p) ? @"An amiibo is on the reader." : @"No amiibo on the reader.";
    if (!n) status.stringValue = [NSString stringWithFormat:@"%@  The amiibo list is empty: press Reload (needs internet once; it is cached).", where];
    else status.stringValue = [NSString stringWithFormat:@"%@  %zu amiibo in the list.", where, n];
    if (p->last_error[0]) status.stringValue = [status.stringValue stringByAppendingFormat:@"\n%s", p->last_error];
}

/* Loads the library: the cache / local source first (instant), then a fetch of the configured URL in the background. */
static void amiibo_load(AmiiboPicker *p, NSTextField *status, BOOL fetch) {
    if (!p || !amiibo_lib_refresh) return;
    pthread_mutex_lock(&p->lock);
    amiibo_lib_refresh(&p->lib, &p->cfg, NULL, 0);
    pthread_mutex_unlock(&p->lock);
    [g_list search:@""];
    amiibo_status(status, p);
    if (!fetch || g_fetching || !amiibo_config_source_is_remote(&p->cfg)) return;
    const char *src = p->cfg.source;
    if (!getenv("NAPPLE_AMIIBO_DB") && !strcmp(src, AMIIBO_DEFAULT_SOURCE)) src = AMIIBO_REPO_DATABASE;
    NSURL *u = [NSURL URLWithString:@(src)];
    if (!u) return;
    g_fetching = YES;
    NSURLSessionDataTask *t = [[NSURLSession sharedSession]
        dataTaskWithURL:u
      completionHandler:^(NSData *data, NSURLResponse *resp, NSError *err) {
          NSInteger code = [resp isKindOfClass:NSHTTPURLResponse.class] ? ((NSHTTPURLResponse *)resp).statusCode : 200;
          dispatch_async(dispatch_get_main_queue(), ^{
              g_fetching = NO;
              if (!err && data.length && code < 400) {
                  NSData *doc = amiibo_convert_database(data);
                  pthread_mutex_lock(&p->lock);
                  amiibo_lib_refresh(&p->lib, &p->cfg, doc.bytes, doc.length);
                  pthread_mutex_unlock(&p->lock);
                  [g_list search:@""];
              } else if (status && !amiibo_picker_count(p)) {
                  status.stringValue = [NSString stringWithFormat:@"Could not download the amiibo list (%@). Set NAPPLE_AMIIBO_DB to a local amiibo.json.",
                                                                  err ? err.localizedDescription : [NSString stringWithFormat:@"HTTP %ld", (long)code]];
                  return;
              }
              amiibo_status(status, p);
          });
      }];
    [t resume];
}

NSView *fe_amiibo_page(NappleFE *fe) {
    (void)fe;
    NSStackView *p = vpage();
    AmiiboPicker *picker = amiibo_service_picker ? amiibo_service_picker() : NULL;
    if (!picker) {
        [p addArrangedSubview:fe_label(@"Amiibo support is not available in this build.", 12, NO)];
        return p;
    }
    if (!g_list) g_list = [FEAmiiboList new];
    g_list.picker = picker;

    NSSearchField *search = [[NSSearchField alloc] initWithFrame:NSMakeRect(0, 0, 318, 22)];
    search.placeholderString = @"Search amiibo";
    search.controlSize = NSControlSizeSmall;
    search.font = [NSFont systemFontOfSize:11];
    [search.widthAnchor constraintEqualToConstant:318].active = YES;
    [p addArrangedSubview:search];

    NSTableView *table = [[NSTableView alloc] initWithFrame:NSMakeRect(0, 0, 318, 240)];
    NSTableColumn *c1 = [[NSTableColumn alloc] initWithIdentifier:@"name"];
    c1.title = @"amiibo";
    c1.width = 150;
    NSTableColumn *c2 = [[NSTableColumn alloc] initWithIdentifier:@"series"];
    c2.title = @"Series";
    c2.width = 150;
    [table addTableColumn:c1];
    [table addTableColumn:c2];
    table.rowHeight = 18;
    table.headerView = nil;
    table.usesAlternatingRowBackgroundColors = NO;
    table.backgroundColor = NSColor.clearColor;
    table.dataSource = g_list;
    table.delegate = g_list;
    g_list.table = table;
    NSScrollView *sv = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 318, 240)];
    sv.documentView = table;
    sv.hasVerticalScroller = YES;
    sv.drawsBackground = NO;
    sv.borderType = NSBezelBorder;
    [sv.widthAnchor constraintEqualToConstant:318].active = YES;
    [sv.heightAnchor constraintEqualToConstant:240].active = YES;
    [p addArrangedSubview:sv];

    NSTextField *status = small_note(@"");
    g_list.status = status;
    fe_bind(search, ^(NSSearchField *s) { [g_list search:s.stringValue]; });

    void (^place)(int) = ^(int flags) {
        const AmiiboInfo *a = [g_list selected];
        if (!a) {
            status.stringValue = @"Choose an amiibo in the list first.";
            return;
        }
        amiibo_install_reader(picker);
        int r = amiibo_picker_select_hex(picker, a->id, flags);
        if (r == 0) [fe toast:[NSString stringWithFormat:@"%s placed on the reader", a->name]];
        amiibo_status(status, picker);
        if (r != 0 && !picker->last_error[0]) status.stringValue = [status.stringValue stringByAppendingFormat:@"\nCould not place it (code %d).", r];
    };
    [p addArrangedSubview:hrow(@[
        button(@"Place", ^(id x) { (void)x; place(AMIIBO_PICK_REUSE); }),
        button(@"Place new copy", ^(id x) { (void)x; place(AMIIBO_PICK_NEW); }),
        button(@"Remove", ^(id x) {
            (void)x;
            amiibo_picker_remove(picker);
            amiibo_status(status, picker);
        }),
    ])];
    [p addArrangedSubview:hrow(@[
        button(@"Reload list", ^(id x) { (void)x; amiibo_load(picker, status, YES); }),
        button(@"Import dump…", ^(id x) {
            (void)x;
            NSOpenPanel *op = [NSOpenPanel openPanel];
            op.message = @"Choose a 540-byte amiibo dump (.bin). Your key_retail.bin must be in ~/.switch or NAPPLE_AMIIBO_KEY.";
            op.allowedContentTypes = @[ [UTType typeWithFilenameExtension:@"bin"] ?: UTTypeData ];
            if ([op runModal] != NSModalResponseOK) return;
            amiibo_install_reader(picker);
            int r = amiibo_picker_import_dump(picker, op.URL.path.UTF8String);
            if (r == 0) [fe toast:@"Dump imported and placed on the reader"];
            amiibo_status(status, picker);
            if (r != 0 && !picker->last_error[0]) status.stringValue = [status.stringValue stringByAppendingFormat:@"\nImport failed (code %d).", r];
        }),
    ])];
    [p addArrangedSubview:status];
    [p addArrangedSubview:small_note(@"Virtual amiibo keep their game data on this Mac (Application Support/Napple/system/amiibo). "
                                     @"Names and pictures come from the community AmiiboAPI list; Napple ships no amiibo data or keys. "
                                     @"Place one when the game asks you to tap an amiibo; remove it when the game asks you to take it away.")];
    amiibo_load(picker, status, amiibo_picker_count(picker) == 0);
    return p;
}

/* ===================================================================== IR camera ==================================================================== */

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
static NSTextField *g_cam_status; /* overlay label, refreshed when the answer arrives */

static void cam_refresh_status(void);
static int cam_request_access(void *ctx, void (*done)(void *, int), void *cbctx) {
    (void)ctx;
    switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo]) {
    case AVAuthorizationStatusAuthorized: return IR_ACCESS_GRANTED;
    case AVAuthorizationStatusNotDetermined:
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo
                                 completionHandler:^(BOOL granted) {
                                     if (done) done(cbctx, granted ? 1 : 0);
                                     dispatch_async(dispatch_get_main_queue(), ^{ cam_refresh_status(); });
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
    dispatch_async(dispatch_get_main_queue(), ^{ cam_refresh_status(); });
    return r;
}
static void cam_stop(void *ctx) {
    (void)ctx;
    dispatch_async(dispatch_get_main_queue(), ^{
        [g_cam stop];
        cam_refresh_status();
    });
}
static int cam_get_frame(void *ctx, uint8_t *out, uint64_t *ts) {
    (void)ctx;
    return [g_cam copyFrame:out ts:ts];
}
static const IrFrameSource g_cam_source = {
    .ctx = NULL, .name = "webcam", .request_access = cam_request_access, .start = cam_start, .stop = cam_stop, .get_frame = cam_get_frame};

/* Installs the webcam as the IR frame source once per process; the permission prompt only appears when a game starts an IR processor. */
void fe_simulators_install(void) {
    if (g_cam_installed || !irs_service_set_source || !irs_adapter_rgb_to_ir) return;
    if (!g_cam) g_cam = [FECamera new];
    irs_service_set_source(&g_cam_source);
    g_cam_installed = YES;
}

static NSString *cam_state_text(void) {
    switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo]) {
    case AVAuthorizationStatusAuthorized:
        return g_cam.session.running ? [NSString stringWithFormat:@"Webcam in use as the IR camera (%@).", g_cam.deviceName ?: @"camera"]
                                     : @"Webcam allowed. It turns on when the game starts the IR camera.";
    case AVAuthorizationStatusDenied: return @"Webcam denied: the game sees the IR sensor as not connected. Allow it in System Settings > Privacy & Security > Camera.";
    case AVAuthorizationStatusRestricted: return @"Webcam restricted on this Mac: the game sees the IR sensor as not connected.";
    default: return @"Not asked yet. Napple asks for the webcam only when a game starts the IR camera.";
    }
}
static void cam_refresh_status(void) {
    if (g_cam_status) g_cam_status.stringValue = cam_state_text();
}

NSView *fe_camera_page(NappleFE *fe) {
    (void)fe;
    NSStackView *p = vpage();
    if (!irs_service_set_source || !irs_adapter_rgb_to_ir) {
        [p addArrangedSubview:fe_label(@"The IR camera simulator is not available in this build.", 12, NO)];
        return p;
    }
    fe_simulators_install();
    [p addArrangedSubview:fe_label(@"Joy-Con IR camera", 12, YES)];
    NSTextField *st = small_note(cam_state_text());
    g_cam_status = st;
    [p addArrangedSubview:st];
    NSMutableArray<NSView *> *buttons = [NSMutableArray new];
    if ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo] == AVAuthorizationStatusNotDetermined)
        [buttons addObject:button(@"Allow webcam now", ^(id x) {
                     (void)x;
                     cam_request_access(NULL, NULL, NULL);
                 })];
    else if ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo] == AVAuthorizationStatusDenied)
        [buttons addObject:button(@"Open Privacy Settings", ^(id x) {
                     (void)x;
                     [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_Camera"]];
                 })];
    [buttons addObject:button(@"Refresh", ^(id x) { (void)x; cam_refresh_status(); })];
    [p addArrangedSubview:hrow(buttons)];
    [p addArrangedSubview:fe_check(@"Mirror the image", g_cam.mirror != 0, ^(NSButton *b) { g_cam.mirror = b.state == NSControlStateValueOn; })];
    [p addArrangedSubview:small_note(@"Games that use the IR camera (moment, clustering or image transfer) see your webcam as a 320x240 near-IR sensor. "
                                     @"Until you answer the permission prompt the game sees a dark frame; nothing is captured while no game uses it. "
                                     @"Headless runs: NAPPLE_IR_SOURCE=blob:x,y,r | dark | flat:v.")];
    return p;
}
