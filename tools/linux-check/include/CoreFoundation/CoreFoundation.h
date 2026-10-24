/* Stub for Linux syntax checks only: just the CoreFoundation names host_info.c uses. */
#pragma once
typedef const void *CFLocaleRef, *CFStringRef;
extern CFStringRef kCFLocaleLanguageCode, kCFLocaleCountryCode;
enum { kCFStringEncodingUTF8 = 0x08000100 };
CFLocaleRef CFLocaleCopyCurrent(void);
const void *CFLocaleGetValue(CFLocaleRef, CFStringRef);
int CFStringGetCString(CFStringRef, char *, long, int);
void CFRelease(const void *);
