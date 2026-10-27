/* iOS process shell helpers: where the game and the saves live. iOS has no exec, so there is no launcher (macOS: packaging/launcher.c);
 * the runtime is the app executable and finds its data inside the bundle. */
#import <Foundation/Foundation.h>
#include "ios_platform.h"
#include "napple.h"
#include <stdio.h>
#include <stdlib.h>

#if NAPPLE_IOS
const char *ios_default_data_dir(void) {
    static char path[4096];
    @autoreleasepool {
        NSString *game = [NSBundle.mainBundle.bundlePath stringByAppendingPathComponent:@"game"];
        const char *over = getenv("NAPPLE_VERSION");
        NSString *ver = over ? @(over) : nil;
        if (!ver) {
            NSString *d = [NSString stringWithContentsOfFile:[game stringByAppendingPathComponent:@"default"] encoding:NSUTF8StringEncoding error:nil];
            ver = [[d componentsSeparatedByString:@"\n"].firstObject stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        }
        if (!ver.length || [ver containsString:@"/"] || [ver hasPrefix:@"."]) return NULL;
        snprintf(path, sizeof path, "%s", [game stringByAppendingPathComponent:ver].fileSystemRepresentation);
    }
    return path;
}

const char *ios_default_save_root(void) {
    static char path[4096];
    @autoreleasepool {
        NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
        NSString *root = [docs stringByAppendingPathComponent:@"Napple"];
        [NSFileManager.defaultManager createDirectoryAtPath:root withIntermediateDirectories:YES attributes:nil error:nil];
        snprintf(path, sizeof path, "%s", root.fileSystemRepresentation);
    }
    return path;
}

const char *ios_shared_mii_dir(void) {
    static char path[4096];
    @autoreleasepool {
        NSString *group = [NSBundle.mainBundle objectForInfoDictionaryKey:@"NappleAppGroup"];
        if (![group isKindOfClass:NSString.class] || !group.length) return NULL;
        NSURL *u = [NSFileManager.defaultManager containerURLForSecurityApplicationGroupIdentifier:group];
        if (!u) return NULL;
        NSString *dir = [u.path stringByAppendingPathComponent:@"Napple/mii"];
        if (![NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil]) return NULL;
        snprintf(path, sizeof path, "%s", dir.fileSystemRepresentation);
    }
    return path;
}

void ios_install_fonts(const char *data_dir, const char *save_root) {
    @autoreleasepool {
        NSFileManager *fm = NSFileManager.defaultManager;
        NSString *src = [[@(data_dir) stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"system/fonts"];
        NSString *dst = [@(save_root) stringByAppendingPathComponent:@"system/fonts"];
        NSArray<NSString *> *names = [fm contentsOfDirectoryAtPath:src error:nil];
        if (!names.count) return;
        [fm createDirectoryAtPath:dst withIntermediateDirectories:YES attributes:nil error:nil];
        for (NSString *n in names) {
            if (![n hasSuffix:@".ttf"] || [n containsString:@"/"]) continue;
            NSString *s = [src stringByAppendingPathComponent:n], *d = [dst stringByAppendingPathComponent:n];
            unsigned long long a = [[fm attributesOfItemAtPath:s error:nil] fileSize], b = [[fm attributesOfItemAtPath:d error:nil] fileSize];
            if (b == a && a) continue;
            [fm removeItemAtPath:d error:nil];
            if (![fm copyItemAtPath:s toPath:d error:nil]) LOG("main", "cannot install font %s", n.UTF8String);
        }
    }
}
#endif
