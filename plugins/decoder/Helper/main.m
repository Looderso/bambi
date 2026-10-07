// SPDX-License-Identifier: GPL-3.0-or-later
//  bambi Decoder's helper: reads the headphones' orientation with CoreMotion and sends it to the decoder as OSC
//  over localhost UDP. A separate app, because macOS asks the requesting PROCESS for a motion usage text and no
//  host carries one; launched by the decoder through Launch Services so that this process, not the host, is the
//  one asked. No window, no Dock icon. Quits when the host process is gone, or when the decoder asks.
//
//      bambi Decoder Helper --host-pid <pid> --port <udp port>
//
//  Sends /quaternion w x y z (SPARTA's form, which the decoder reads) on every motion update, and
//  /bambi/helper/status "<text>" twice a second: the permission, whether headphones are connected, the count.
#import <Cocoa/Cocoa.h>
#import <CoreMotion/CoreMotion.h>
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
static void sendQuaternion(float w, float x, float y, float z) {
    char buf[64];
    size_t o = putString(buf, 0, "/quaternion");
    o = putString(buf, o, ",ffff");
    float v[4] = {w, x, y, z};
    for (int i = 0; i < 4; ++i) {
        uint32_t u;
        memcpy(&u, &v[i], 4);
        u = htonl(u);
        memcpy(buf + o, &u, 4);
        o += 4;
    }
    sendto(sock, buf, o, 0, (struct sockaddr*)&dest, sizeof dest);
}
static void sendStatus(const char* s) {
    char buf[512];
    size_t o = putString(buf, 0, "/bambi/helper/status");
    o = putString(buf, o, ",s");
    o = putString(buf, o, s);
    sendto(sock, buf, o, 0, (struct sockaddr*)&dest, sizeof dest);
}

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
@property(atomic) BOOL connected;
@property(atomic) long updates;
@property(atomic, copy) NSString* lastError;
@end

@implementation Helper
- (void)applicationDidFinishLaunching:(NSNotification*)n {
    logLine([NSString stringWithFormat:@"launched, pid %d, host pid %d", getpid(), self.hostPid]);
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
                                      }];
}
- (void)applicationWillTerminate:(NSNotification*)n {
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
        for (int i = 1; i + 1 < argc; ++i) {
            if (strcmp(argv[i], "--host-pid") == 0) hostPid = atoi(argv[i + 1]);
            if (strcmp(argv[i], "--port") == 0) port = atoi(argv[i + 1]);
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
        app.delegate = h;
        [app run];
    }
    return 0;
}
