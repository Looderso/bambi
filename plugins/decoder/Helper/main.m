// SPDX-License-Identifier: GPL-3.0-or-later
//  bambi Decoder's helper: reads the headphones' orientation with CoreMotion and sends it to the decoder as OSC
//  over localhost UDP. A separate app, because macOS asks the requesting PROCESS for a motion usage text and no
//  host carries one; launched by the decoder through Launch Services so that this process, not the host, is the
//  one asked. No window, no Dock icon. Quits when the host process is gone, or when the decoder asks.
//
//      bambi Decoder Helper --host-pid <pid> --port <udp port> [--phone]
//
//  Sends /quaternion w x y z (SPARTA's form, which the decoder reads) on every motion update, and
//  /bambi/helper/status "<text>" twice a second: the permission, whether headphones are connected, the count.
//  With --phone it also serves the phone page (server.h) and sends /bambi/phone/ypr yaw pitch roll for every
//  orientation the phone sends, and /bambi/helper/phone "<url>" "<state>" twice a second.
#import <Cocoa/Cocoa.h>
#import <CoreMotion/CoreMotion.h>

#import "phone_page.h"
#import "server.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

static FILE* logFile;
static void logLine(NSString* s) {
    if (logFile == NULL) return;
    NSString* line = [NSString stringWithFormat:@"%@  helper: %@\n", [NSDate date], s];
    fputs(line.UTF8String, logFile);
    fflush(logFile);
}

static int sock = -1;
static struct sockaddr_in dest;

static size_t pad4(size_t n) { return (n + 3) & ~(size_t)3; }
static size_t putString(char* buf, size_t o, const char* s) {
    size_t n = strlen(s) + 1;
    memset(buf + o, 0, pad4(n));
    memcpy(buf + o, s, n);
    return o + pad4(n);
}
/// An OSC message of floats to the decoder.
static void sendFloats(const char* address, const float* v, int n) {
    char buf[128];
    char tags[8] = ",";
    for (int i = 0; i < n && i < 6; ++i) tags[1 + i] = 'f';
    size_t o = putString(buf, 0, address);
    o = putString(buf, o, tags);
    for (int i = 0; i < n && i < 6; ++i) {
        uint32_t u;
        memcpy(&u, &v[i], 4);
        u = htonl(u);
        memcpy(buf + o, &u, 4);
        o += 4;
    }
    sendto(sock, buf, o, 0, (struct sockaddr*)&dest, sizeof dest);
}
/// An OSC message of strings to the decoder; `second` may be NULL.
static void sendStrings(const char* address, const char* first, const char* second) {
    char buf[1024];
    if (strlen(first) + (second ? strlen(second) : 0) > 900) return;
    size_t o = putString(buf, 0, address);
    o = putString(buf, o, second ? ",ss" : ",s");
    o = putString(buf, o, first);
    if (second) o = putString(buf, o, second);
    sendto(sock, buf, o, 0, (struct sockaddr*)&dest, sizeof dest);
}
static void sendQuaternion(float w, float x, float y, float z) {
    const float v[4] = {w, x, y, z};
    sendFloats("/quaternion", v, 4);
}
static void sendStatus(const char* s) { sendStrings("/bambi/helper/status", s, NULL); }

static const char* statusName(CMAuthorizationStatus s) {
    switch (s) {
        case CMAuthorizationStatusNotDetermined: return "permission not asked yet";
        case CMAuthorizationStatusRestricted: return "motion restricted on this mac";
        case CMAuthorizationStatusDenied: return "permission denied: System Settings, Privacy, Motion";
        case CMAuthorizationStatusAuthorized: return "permission granted";
    }
    return "unknown";
}

@interface Helper : NSObject <NSApplicationDelegate, CMHeadphoneMotionManagerDelegate>
@property(strong) CMHeadphoneMotionManager* manager;
@property(strong) NSOperationQueue* queue;
@property pid_t hostPid;
@property BOOL phone;  ///< serve the phone page
@property(strong) PhoneServer* server;
@property long phoneSeen;  ///< the server's count at the last status, for the rate
@property(atomic) BOOL connected;
@property(atomic) long updates;
@property(atomic, copy) NSString* lastError;
@end

@implementation Helper
- (void)applicationDidFinishLaunching:(NSNotification*)n {
    logLine([NSString stringWithFormat:@"launched, pid %d, host pid %d%@", getpid(), self.hostPid,
                                       self.phone ? @", serving the phone page" : @""]);
    if (self.phone) {
        self.server = [[PhoneServer alloc] initWithPort:8443
                                                   page:[NSData dataWithBytes:kPhonePage length:kPhonePageSize]
                                          onOrientation:^(float yaw, float pitch, float roll) {
                                            const float v[3] = {yaw, pitch, roll};
                                            sendFloats("/bambi/phone/ypr", v, 3);
                                          }];
        NSError* error = nil;
        if (![self.server start:&error]) logLine([@"phone page: " stringByAppendingString:error.localizedDescription]);
        else logLine([@"phone page at " stringByAppendingString:self.server.url]);
    }
    self.manager = [[CMHeadphoneMotionManager alloc] init];
    self.manager.delegate = self;
    self.queue = [[NSOperationQueue alloc] init];
    self.queue.maxConcurrentOperationCount = 1;
    [self.manager startDeviceMotionUpdatesToQueue:self.queue
                                      withHandler:^(CMDeviceMotion* m, NSError* e) {
                                          if (e != nil) {
                                              self.lastError = e.localizedDescription;
                                              logLine(self.lastError);
                                              return;
                                          }
                                          if (m == nil) return;
                                          CMQuaternion q = m.attitude.quaternion;
                                          sendQuaternion((float)q.w, (float)q.x, (float)q.y, (float)q.z);
                                          self.updates++;
                                      }];
    [NSTimer scheduledTimerWithTimeInterval:0.5
                                    repeats:YES
                                      block:^(NSTimer* t) {
                                          if (self.hostPid > 0 && kill(self.hostPid, 0) != 0 && errno == ESRCH) {
                                              logLine(@"host gone, quitting");
                                              [NSApp terminate:nil];
                                          }
                                          NSString* s = [NSString stringWithFormat:@"%s | headphones %s | %ld updates%@%@",
                                                                                   statusName([CMHeadphoneMotionManager authorizationStatus]),
                                                                                   self.connected ? "connected" : "not connected", self.updates,
                                                                                   self.lastError ? @" | " : @"", self.lastError ?: @""];
                                          sendStatus(s.UTF8String);
                                          if (self.server != nil) {
                                              const long now = self.server.updates;
                                              NSString* state = self.server.state;
                                              if (now > self.phoneSeen)
                                                  state = [NSString stringWithFormat:@"%@, %ld/s", state, 2 * (now - self.phoneSeen)];
                                              self.phoneSeen = now;
                                              sendStrings("/bambi/helper/phone", self.server.url.UTF8String, state.UTF8String);
                                          }
                                      }];
}
- (void)applicationWillTerminate:(NSNotification*)n {
    [self.server stop];
    [self.manager stopDeviceMotionUpdates];
    logLine(@"terminating");
}
- (void)headphoneMotionManagerDidConnect:(CMHeadphoneMotionManager*)m {
    self.connected = YES;
    logLine(@"headphones connected");
}
- (void)headphoneMotionManagerDidDisconnect:(CMHeadphoneMotionManager*)m {
    self.connected = NO;
    logLine(@"headphones disconnected");
}
@end

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        NSString* path = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Logs/bambi-decoder-helper.log"];
        logFile = fopen(path.UTF8String, "a");
        pid_t hostPid = 0;
        int port = 9000;
        BOOL phone = NO;
        for (int i = 1; i < argc; ++i) {
            if (strcmp(argv[i], "--host-pid") == 0 && i + 1 < argc) hostPid = atoi(argv[i + 1]);
            if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = atoi(argv[i + 1]);
            if (strcmp(argv[i], "--phone") == 0) phone = YES;
        }
        sock = socket(AF_INET, SOCK_DGRAM, 0);
        memset(&dest, 0, sizeof dest);
        dest.sin_family = AF_INET;
        dest.sin_port = htons((uint16_t)port);
        dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        NSApplication* app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyAccessory];
        Helper* h = [[Helper alloc] init];
        h.hostPid = hostPid;
        h.phone = phone;
        app.delegate = h;
        [app run];
    }
    return 0;
}
