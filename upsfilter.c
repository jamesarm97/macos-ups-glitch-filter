// upsfilter.c — seize a flaky USB HID UPS, debounce its "on battery" flag,
// and republish a clean power source to macOS.
//
// Default target: "SmartBattery" / -BMS-  VID 0x075d  PID 0x0300 (override with --vid/--pid)
//
// Build:  clang -O2 -Wall -o upsfilter upsfilter.c -framework IOKit -framework CoreFoundation
// Run:    sudo ./upsfilter --dump           list the device's HID elements and exit
//         sudo ./upsfilter                  filter + republish (default 20 s debounce)
//         sudo ./upsfilter --delay 30       custom debounce
//         sudo ./upsfilter --no-publish     seize + log only (macOS sees no UPS)
//         sudo ./upsfilter --vid 0x075d --pid 0x0300   pick a different UPS

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/ps/IOPSKeys.h>
#include <dispatch/dispatch.h>
#include <getopt.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// Exported by IOKit but not in the public headers (ioupsd uses them).
// Declared with a trailing type arg: harmless if the real function takes only one.
typedef struct OpaqueIOPSPowerSourceID *IOPSPowerSourceID;
extern IOReturn IOPSCreatePowerSource(IOPSPowerSourceID *outPS, CFStringRef type);
extern IOReturn IOPSSetPowerSourceDetails(IOPSPowerSourceID ps, CFDictionaryRef details);
extern IOReturn IOPSReleasePowerSource(IOPSPowerSourceID ps);

#define UPS_VID 0x075d
#define UPS_PID 0x0300

#define PAGE_POWER   0x84
#define PAGE_BATTERY 0x85
#define USAGE_CHANGED_STATUS 0x03   // 0x84:0x03 collection — event flags, ignore

enum { F_AC, F_DISCHG, F_CHG, F_REMCAP, F_FULLCAP, F_RUNTIME,
       F_SHUTDOWN, F_BELOWLIM, F_BATTPRESENT, F_COUNT };

static const struct { uint32_t page, usage; const char *name; } kFields[F_COUNT] = {
    [F_AC]          = {PAGE_BATTERY, 0xD0, "ACPresent"},
    [F_DISCHG]      = {PAGE_BATTERY, 0x45, "Discharging"},
    [F_CHG]         = {PAGE_BATTERY, 0x44, "Charging"},
    [F_REMCAP]      = {PAGE_BATTERY, 0x66, "RemainingCapacity"},
    [F_FULLCAP]     = {PAGE_BATTERY, 0x67, "FullChargeCapacity"},
    [F_RUNTIME]     = {PAGE_BATTERY, 0x68, "RunTimeToEmpty"},
    [F_SHUTDOWN]    = {PAGE_POWER,   0x69, "ShutdownImminent"},
    [F_BELOWLIM]    = {PAGE_BATTERY, 0x42, "BelowRemainingCapacityLimit"},
    [F_BATTPRESENT] = {PAGE_BATTERY, 0xD1, "BatteryPresent"},
};

static IOHIDDeviceRef    gDev;
static IOHIDElementRef   gFeat[F_COUNT], gIn[F_COUNT];
static CFIndex           gVal[F_COUNT];
static bool              gHave[F_COUNT];
static IOPSPowerSourceID gPS;

static int    gVid = UPS_VID, gPid = UPS_PID;
static int    gDelay   = 20;      // seconds raw "on battery" must persist
static bool   gPublish = true, gDump = false, gVerbose = false;

static bool   gOnBattery   = false;   // filtered state
static time_t gRawSince    = 0;       // when raw state went to battery (0 = on AC)
static int    gRepPct      = -1;      // reported (smoothed) percent
static unsigned gBlips     = 0;

static struct { bool onBatt, charging; int pct, mins; time_t at; } gLastPub = { .pct = -2 };

static void logmsg(const char *fmt, ...) {
    char ts[32]; time_t n = time(NULL);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&n));
    printf("%s  ", ts);
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    putchar('\n');
}

// ---------- HID element discovery / reading ----------

static bool readElement(IOHIDElementRef e, CFIndex *out) {
    IOHIDValueRef v = NULL;
    if (!e || IOHIDDeviceGetValue(gDev, e, &v) != kIOReturnSuccess || !v) return false;
    *out = IOHIDValueGetIntegerValue(v);
    return true;
}

static const char *typeName(IOHIDElementType t) {
    switch (t) {
        case kIOHIDElementTypeInput_Misc:   return "in-misc";
        case kIOHIDElementTypeInput_Button: return "in-btn ";
        case kIOHIDElementTypeInput_Axis:   return "in-axis";
        case kIOHIDElementTypeOutput:       return "output ";
        case kIOHIDElementTypeFeature:      return "feature";
        case kIOHIDElementTypeCollection:   return "collect";
        default:                            return "other  ";
    }
}

static const char *knownName(uint32_t page, uint32_t usage) {
    for (int f = 0; f < F_COUNT; f++)
        if (kFields[f].page == page && kFields[f].usage == usage) return kFields[f].name;
    return "";
}

static void discover(void) {
    CFArrayRef els = IOHIDDeviceCopyMatchingElements(gDev, NULL, kIOHIDOptionsTypeNone);
    if (!els) { logmsg("no HID elements on device"); return; }
    CFIndex n = CFArrayGetCount(els);
    for (CFIndex i = 0; i < n; i++) {
        IOHIDElementRef e = (IOHIDElementRef)CFArrayGetValueAtIndex(els, i);
        IOHIDElementType t = IOHIDElementGetType(e);
        uint32_t page = IOHIDElementGetUsagePage(e), usage = IOHIDElementGetUsage(e);
        IOHIDElementRef p = IOHIDElementGetParent(e);
        uint32_t ppage = p ? IOHIDElementGetUsagePage(p) : 0, pusage = p ? IOHIDElementGetUsage(p) : 0;

        if (gDump) {
            CFIndex v = 0; bool ok = (t != kIOHIDElementTypeCollection) && readElement(e, &v);
            printf("%s  %02X:%04X  rid=%-3u size=%-2u cnt=%-2u log=[%ld..%ld]  parent=%02X:%04X  %-28s %s%ld\n",
                   typeName(t), page, usage, IOHIDElementGetReportID(e),
                   IOHIDElementGetReportSize(e), IOHIDElementGetReportCount(e),
                   (long)IOHIDElementGetLogicalMin(e), (long)IOHIDElementGetLogicalMax(e),
                   ppage, pusage, knownName(page, usage), ok ? "= " : "", ok ? (long)v : 0L);
        }

        if (t == kIOHIDElementTypeCollection || t == kIOHIDElementTypeOutput) continue;
        if (ppage == PAGE_POWER && pusage == USAGE_CHANGED_STATUS) continue;
        for (int f = 0; f < F_COUNT; f++) {
            if (kFields[f].page != page || kFields[f].usage != usage) continue;
            if (t == kIOHIDElementTypeFeature) { if (!gFeat[f]) gFeat[f] = (IOHIDElementRef)CFRetain(e); }
            else                               { if (!gIn[f])   gIn[f]   = (IOHIDElementRef)CFRetain(e); }
        }
    }
    CFRelease(els);

    if (!gDump) {
        char buf[512] = ""; 
        for (int f = 0; f < F_COUNT; f++)
            if (gFeat[f] || gIn[f]) { strlcat(buf, " ", sizeof buf); strlcat(buf, kFields[f].name, sizeof buf); }
        logmsg("found fields:%s", buf[0] ? buf : " (none!)");
    }
}

static void readAll(void) {
    for (int f = 0; f < F_COUNT; f++) {
        CFIndex v;
        bool ok = readElement(gFeat[f], &v) || readElement(gIn[f], &v);
        if (!ok) continue;
        bool noisy = (f == F_REMCAP || f == F_RUNTIME || f == F_FULLCAP);
        if (gHave[f] && v != gVal[f] && (!noisy || gVerbose))
            logmsg("raw %-28s %ld -> %ld", kFields[f].name, (long)gVal[f], (long)v);
        gVal[f] = v; gHave[f] = true;
    }
}

// ---------- republished power source ----------

static void dictSetInt(CFMutableDictionaryRef d, const char *k, int v) {
    CFStringRef key = CFStringCreateWithCString(NULL, k, kCFStringEncodingUTF8);
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &v);
    CFDictionarySetValue(d, key, n); CFRelease(n); CFRelease(key);
}
static void dictSetStr(CFMutableDictionaryRef d, const char *k, const char *v) {
    CFStringRef key = CFStringCreateWithCString(NULL, k, kCFStringEncodingUTF8);
    CFStringRef s = CFStringCreateWithCString(NULL, v, kCFStringEncodingUTF8);
    CFDictionarySetValue(d, key, s); CFRelease(s); CFRelease(key);
}
static void dictSetBool(CFMutableDictionaryRef d, const char *k, bool v) {
    CFStringRef key = CFStringCreateWithCString(NULL, k, kCFStringEncodingUTF8);
    CFDictionarySetValue(d, key, v ? kCFBooleanTrue : kCFBooleanFalse); CFRelease(key);
}

static void publish(bool force) {
    if (!gPublish) return;
    int pct  = gRepPct < 0 ? 100 : gRepPct;
    bool chg = !gOnBattery && (gHave[F_CHG] ? gVal[F_CHG] != 0 : pct < 100);
    int mins = (gOnBattery && gHave[F_RUNTIME]) ? (int)(gVal[F_RUNTIME] / 60) : -1;
    time_t now = time(NULL);

    if (!force && gLastPub.onBatt == gOnBattery && gLastPub.pct == pct &&
        gLastPub.charging == chg && gLastPub.mins == mins && now - gLastPub.at < 60) return;

    if (!gPS) {
        IOReturn r = IOPSCreatePowerSource(&gPS, CFSTR(kIOPSUPSType));
        if (r != kIOReturnSuccess || !gPS) {
            logmsg("IOPSCreatePowerSource failed (0x%08x) — continuing without republishing", r);
            gPublish = false; gPS = NULL; return;
        }
        logmsg("created filtered UPS power source");
    }

    CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    dictSetStr (d, kIOPSNameKey, "UPS (filtered)");
    dictSetStr (d, kIOPSTypeKey, kIOPSUPSType);
    dictSetStr (d, kIOPSTransportTypeKey, kIOPSUSBTransportType);
    dictSetBool(d, kIOPSIsPresentKey, true);
    dictSetStr (d, kIOPSPowerSourceStateKey, gOnBattery ? kIOPSBatteryPowerValue : kIOPSACPowerValue);
    dictSetInt (d, kIOPSCurrentCapacityKey, pct);
    dictSetInt (d, kIOPSMaxCapacityKey, 100);
    dictSetBool(d, kIOPSIsChargingKey, chg);
    if (mins >= 0) dictSetInt(d, kIOPSTimeToEmptyKey, mins);
    dictSetInt (d, kIOPSVendorIDKey, gVid);
    dictSetInt (d, kIOPSProductIDKey, gPid);

    IOReturn r = IOPSSetPowerSourceDetails(gPS, d);
    CFRelease(d);
    if (r != kIOReturnSuccess) logmsg("IOPSSetPowerSourceDetails failed (0x%08x)", r);

    gLastPub.onBatt = gOnBattery; gLastPub.pct = pct; gLastPub.charging = chg;
    gLastPub.mins = mins; gLastPub.at = now;
}

// ---------- filtering ----------

static int rawPercent(void) {
    if (!gHave[F_REMCAP]) return -1;
    long rem = gVal[F_REMCAP];
    long full = gHave[F_FULLCAP] ? gVal[F_FULLCAP] : 100;
    long p = (full > 0 && full != 100) ? rem * 100 / full : rem;
    return p < 0 ? 0 : p > 100 ? 100 : (int)p;
}

static void tick(CFRunLoopTimerRef t, void *info) {
    (void)t; (void)info;
    readAll();

    bool rawBatt = (gHave[F_AC] && gVal[F_AC] == 0) || (gHave[F_DISCHG] && gVal[F_DISCHG] != 0);
    bool imminent = gHave[F_SHUTDOWN] && gVal[F_SHUTDOWN] != 0;
    time_t now = time(NULL);

    if (rawBatt) {
        if (!gRawSince) gRawSince = now;
        if (!gOnBattery && (now - gRawSince >= gDelay || imminent)) {
            gOnBattery = true;
            logmsg("ON BATTERY — confirmed after %lds%s", (long)(now - gRawSince),
                   imminent ? " (UPS reports shutdown imminent)" : "");
        }
    } else {
        if (gRawSince && !gOnBattery)
            logmsg("ignored %lds 'on battery' blip (#%u since start)", (long)(now - gRawSince), ++gBlips);
        if (gOnBattery) logmsg("AC restored after %lds", (long)(now - gRawSince));
        gRawSince = 0; gOnBattery = false;
    }

    // Smooth the 99<->100 jitter while on AC; report honestly on battery.
    int p = rawPercent();
    if (p >= 0 && (gRepPct < 0 || gOnBattery || p > gRepPct || gRepPct - p >= 2)) gRepPct = p;

    publish(false);
}

// ---------- lifecycle ----------

static void cleanup(void) {
    if (gPS)  { IOPSReleasePowerSource(gPS); gPS = NULL; }
    if (gDev) { IOHIDDeviceClose(gDev, kIOHIDOptionsTypeSeizeDevice); }
}

static void onRemoved(void *ctx, IOReturn r, void *sender) {
    (void)ctx; (void)r; (void)sender;
    logmsg("UPS disconnected — exiting (launchd will restart and wait for it)");
    cleanup();
    exit(2);
}

static void handleSignal(int sig) {
    signal(sig, SIG_IGN);
    dispatch_source_t s = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, sig, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(s, ^{ logmsg("signal %d — releasing UPS", sig); cleanup(); exit(0); });
    dispatch_resume(s);
}

static IOHIDDeviceRef findDevice(IOHIDManagerRef mgr) {
    CFSetRef set = IOHIDManagerCopyDevices(mgr);
    if (!set) return NULL;
    IOHIDDeviceRef dev = NULL;
    if (CFSetGetCount(set) > 0) {
        CFIndex n = CFSetGetCount(set);
        const void **vals = calloc((size_t)n, sizeof *vals);
        CFSetGetValues(set, vals);
        dev = (IOHIDDeviceRef)CFRetain(vals[0]);
        free(vals);
    }
    CFRelease(set);
    return dev;
}

int main(int argc, char **argv) {
    static struct option opts[] = {
        {"dump", no_argument, 0, 'd'}, {"delay", required_argument, 0, 't'},
        {"no-publish", no_argument, 0, 'n'}, {"verbose", no_argument, 0, 'v'},
        {"vid", required_argument, 0, 'V'}, {"pid", required_argument, 0, 'P'},
        {"help", no_argument, 0, 'h'}, {0, 0, 0, 0}};
    int c;
    while ((c = getopt_long(argc, argv, "dt:nvV:P:h", opts, NULL)) != -1) {
        switch (c) {
            case 'd': gDump = true; break;
            case 't': gDelay = atoi(optarg); if (gDelay < 1) gDelay = 1; break;
            case 'n': gPublish = false; break;
            case 'v': gVerbose = true; break;
            case 'V': gVid = (int)strtol(optarg, NULL, 0); break;
            case 'P': gPid = (int)strtol(optarg, NULL, 0); break;
            default:
                fprintf(stderr, "usage: %s [--dump] [--delay SECONDS] [--no-publish] [--verbose] [--vid ID] [--pid ID]\n", argv[0]);
                return c == 'h' ? 0 : 64;
        }
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (geteuid() != 0) fprintf(stderr, "warning: not running as root; seizing/publishing will probably fail\n");

    IOHIDManagerRef mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    int vid = gVid, pid = gPid;
    CFNumberRef nv = CFNumberCreate(NULL, kCFNumberIntType, &vid), np = CFNumberCreate(NULL, kCFNumberIntType, &pid);
    const void *keys[] = {CFSTR(kIOHIDVendorIDKey), CFSTR(kIOHIDProductIDKey)}, *vals[] = {nv, np};
    CFDictionaryRef match = CFDictionaryCreate(NULL, keys, vals, 2,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    IOHIDManagerSetDeviceMatching(mgr, match);
    CFRelease(match); CFRelease(nv); CFRelease(np);

    bool said = false;
    while (!(gDev = findDevice(mgr))) {
        if (gDump) { fprintf(stderr, "UPS %04x:%04x not found\n", gVid, gPid); return 1; }
        if (!said) { logmsg("waiting for UPS %04x:%04x ...", gVid, gPid); said = true; }
        sleep(5);
    }

    IOReturn r = IOHIDDeviceOpen(gDev, kIOHIDOptionsTypeSeizeDevice);
    if (r != kIOReturnSuccess) {
        logmsg("seize failed: 0x%08x%s", r, r == kIOReturnExclusiveAccess ? " (another client already seized it)" :
               r == kIOReturnNotPrivileged ? " (run with sudo)" : "");
        return 1;
    }

    discover();
    if (gDump) { cleanup(); return 0; }

    logmsg("seized UPS; debounce %ds; republish %s", gDelay, gPublish ? "on" : "off");
    IOHIDDeviceScheduleWithRunLoop(gDev, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
    IOHIDDeviceRegisterRemovalCallback(gDev, onRemoved, NULL);
    handleSignal(SIGTERM); handleSignal(SIGINT); handleSignal(SIGHUP);

    readAll();
    gRepPct = rawPercent();
    publish(true);

    CFRunLoopTimerRef timer = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 1.0, 1.0, 0, 0, tick, NULL);
    CFRunLoopAddTimer(CFRunLoopGetMain(), timer, kCFRunLoopDefaultMode);
    CFRunLoopRun();
    cleanup();
    return 0;
}
