// SPDX-License-Identifier: GPL-3.0-or-later
#import "server.h"

#import <CommonCrypto/CommonDigest.h>
#import <Network/Network.h>
#import <Security/Security.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

static NSString* const kWebSocketGuid = @"258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/// The address a packet to the outside would leave by: the default route's interface, which may be a VPN's
/// tunnel. No packet is sent. Empty without a network.
static NSString* routeAddress(void) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return @"";
    struct sockaddr_in far = {0};
    far.sin_family = AF_INET;
    far.sin_port = htons(1);
    inet_pton(AF_INET, "10.255.255.255", &far.sin_addr);
    struct sockaddr_in me = {0};
    socklen_t len = sizeof me;
    NSString* out = @"";
    if (connect(s, (struct sockaddr*)&far, sizeof far) == 0 && getsockname(s, (struct sockaddr*)&me, &len) == 0) {
        char buf[64];
        if (inet_ntop(AF_INET, &me.sin_addr, buf, sizeof buf) != NULL && strcmp(buf, "0.0.0.0") != 0) out = @(buf);
    }
    close(s);
    return out;
}

/// This computer's address on the local network, where a phone on the same Wi-Fi reaches it: an IPv4 address on
/// an interface that is up, broadcasts and is not point-to-point, so never a VPN's tunnel. The default route's
/// address when it is one of these, else the first on an Ethernet or Wi-Fi port (en*), else the first of any;
/// the default route's address when there is none. Empty without a network.
static NSString* lanAddress(void) {
    NSString* routed = routeAddress();
    struct ifaddrs* all = NULL;
    if (getifaddrs(&all) != 0) return routed;
    NSString *firstEn = nil, *first = nil;
    BOOL routedIsLan = NO;
    for (struct ifaddrs* i = all; i != NULL; i = i->ifa_next) {
        if (i->ifa_addr == NULL || i->ifa_addr->sa_family != AF_INET) continue;
        const unsigned flags = i->ifa_flags;
        if (!(flags & IFF_UP) || !(flags & IFF_RUNNING) || !(flags & IFF_BROADCAST) || (flags & IFF_LOOPBACK) ||
            (flags & IFF_POINTOPOINT))
            continue;
        const struct in_addr a = ((const struct sockaddr_in*)i->ifa_addr)->sin_addr;
        if ((ntohl(a.s_addr) & 0xffff0000u) == 0xa9fe0000u) continue;  // 169.254/16: no DHCP answer, unreachable
        char buf[64];
        if (inet_ntop(AF_INET, &a, buf, sizeof buf) == NULL) continue;
        NSString* address = @(buf);
        if ([address isEqualToString:routed]) routedIsLan = YES;
        if (firstEn == nil && strncmp(i->ifa_name, "en", 2) == 0) firstEn = address;
        if (first == nil) first = address;
    }
    freeifaddrs(all);
    if (routedIsLan) return routed;
    return firstEn ?: first ?: routed;
}

static BOOL run(NSArray<NSString*>* arguments, NSString** failure) {
    NSTask* task = [[NSTask alloc] init];
    task.executableURL = [NSURL fileURLWithPath:@"/usr/bin/openssl"];
    task.arguments = arguments;
    task.standardOutput = [NSPipe pipe];
    task.standardError = [NSPipe pipe];
    NSError* error = nil;
    if (![task launchAndReturnError:&error]) {
        *failure = error.localizedDescription;
        return NO;
    }
    [task waitUntilExit];
    if (task.terminationStatus != 0) {
        NSData* err = [[task.standardError fileHandleForReading] readDataToEndOfFile];
        *failure = [[NSString alloc] initWithData:err encoding:NSUTF8StringEncoding] ?: @"openssl failed";
        return NO;
    }
    return YES;
}

/*  A certificate for this address, made again when the address changes or the old one is within a week of its
    end, and the identity it makes. The system's openssl writes the files; the key is RSA because the TLS stack
    needs the private key in a keychain and the file keychain holds RSA, not EC. The keychain is the helper's own,
    beside the files, with an access list that trusts every application, so nothing prompts and a rebuilt helper
    still reads it. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
static SecIdentityRef makeIdentity(NSString* address, NSString** failure) {
    NSFileManager* files = [NSFileManager defaultManager];
    NSString* cache = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Caches/bambi-decoder-helper"];
    [files createDirectoryAtPath:cache withIntermediateDirectories:YES attributes:nil error:nil];
    NSString* cert = [cache stringByAppendingPathComponent:@"cert.pem"];
    NSString* key = [cache stringByAppendingPathComponent:@"key.pem"];
    NSString* p12 = [cache stringByAppendingPathComponent:@"identity.p12"];
    NSString* keychainPath = [cache stringByAppendingPathComponent:@"phone.keychain-db"];
    NSString* stamp = [cache stringByAppendingPathComponent:@"address.txt"];
    const char* password = "bambi";  // the keychain's; it guards nothing, the key is for a page on the local network
    NSString* made = [NSString stringWithContentsOfFile:stamp encoding:NSUTF8StringEncoding error:nil];
    NSDictionary* attrs = [files attributesOfItemAtPath:p12 error:nil];
    const BOOL fresh = attrs != nil && [[NSDate date] timeIntervalSinceDate:attrs.fileModificationDate] < 23 * 86400;
    if (!(fresh && [made isEqualToString:address] && [files fileExistsAtPath:keychainPath])) {
        if (!run(@[ @"req", @"-x509", @"-newkey", @"rsa:2048", @"-nodes", @"-days", @"30", @"-keyout", key, @"-out",
                    cert, @"-subj", @"/CN=bambi Decoder phone page", @"-addext",
                    [@"subjectAltName=IP:" stringByAppendingString:address], @"-addext", @"extendedKeyUsage=serverAuth" ],
                 failure))
            return NULL;
        if (!run(@[ @"pkcs12", @"-export", @"-inkey", key, @"-in", cert, @"-passout", @"pass:bambi", @"-out", p12 ], failure))
            return NULL;
        [files removeItemAtPath:keychainPath error:nil];
        [address writeToFile:stamp atomically:YES encoding:NSUTF8StringEncoding error:nil];
    }
    SecKeychainRef keychain = NULL;
    if ([files fileExistsAtPath:keychainPath]) {
        if (SecKeychainOpen(keychainPath.UTF8String, &keychain) != errSecSuccess) {
            *failure = @"the keychain did not open";
            return NULL;
        }
        SecKeychainUnlock(keychain, (UInt32)strlen(password), password, TRUE);
        NSDictionary* query = @{
            (id)kSecClass : (id)kSecClassIdentity,
            (id)kSecMatchSearchList : @[ (__bridge id)keychain ],
            (id)kSecReturnRef : @YES,
            (id)kSecMatchLimit : (id)kSecMatchLimitOne
        };
        CFTypeRef found = NULL;
        if (SecItemCopyMatching((__bridge CFDictionaryRef)query, &found) == errSecSuccess && found != NULL) {
            CFRelease(keychain);
            return (SecIdentityRef)found;
        }
        [files removeItemAtPath:keychainPath error:nil];  // empty or unreadable: made again below
        CFRelease(keychain);
        keychain = NULL;
    }
    if (SecKeychainCreate(keychainPath.UTF8String, (UInt32)strlen(password), password, FALSE, NULL, &keychain) !=
        errSecSuccess) {
        *failure = @"the keychain could not be made";
        return NULL;
    }
    SecAccessRef access = NULL;
    SecAccessCreate(CFSTR("bambi Decoder phone page"), NULL, &access);
    CFArrayRef acls = NULL;
    if (access != NULL && SecAccessCopyACLList(access, &acls) == errSecSuccess) {
        for (CFIndex i = 0; i < CFArrayGetCount(acls); ++i) {
            SecACLRef acl = (SecACLRef)CFArrayGetValueAtIndex(acls, i);
            CFArrayRef apps = NULL;
            CFStringRef description = NULL;
            SecKeychainPromptSelector selector;
            if (SecACLCopyContents(acl, &apps, &description, &selector) == errSecSuccess) {
                SecACLSetContents(acl, NULL, description, 0);  // every application, without a prompt
                if (apps != NULL) CFRelease(apps);
                if (description != NULL) CFRelease(description);
            }
        }
        CFRelease(acls);
    }
    NSData* bytes = [NSData dataWithContentsOfFile:p12];
    NSMutableDictionary* options = [@{
        (id)kSecImportExportPassphrase : @"bambi",
        (id)kSecImportExportKeychain : (__bridge id)keychain
    } mutableCopy];
    if (access != NULL) options[(id)kSecImportExportAccess] = (__bridge id)access;
    CFArrayRef items = NULL;
    const OSStatus status =
        bytes == nil ? errSecIO : SecPKCS12Import((__bridge CFDataRef)bytes, (__bridge CFDictionaryRef)options, &items);
    SecIdentityRef identity = NULL;
    if (status == errSecSuccess && items != NULL && CFArrayGetCount(items) > 0) {
        NSDictionary* item = (__bridge NSDictionary*)CFArrayGetValueAtIndex(items, 0);
        identity = (__bridge SecIdentityRef)item[(id)kSecImportItemIdentity];
        if (identity != NULL) CFRetain(identity);
    }
    if (identity == NULL) *failure = [NSString stringWithFormat:@"the identity did not import (%d)", (int)status];
    if (items != NULL) CFRelease(items);
    if (access != NULL) CFRelease(access);
    CFRelease(keychain);
    return identity;
}
#pragma clang diagnostic pop

// ---- one connection: HTTP until it upgrades, then WebSocket frames -------------------------------------------

@interface PhoneConnection : NSObject
@property(strong) nw_connection_t connection;
@property(strong) NSMutableData* buffer;
@property BOOL websocket;
@property(weak) PhoneServer* server;
@end

@interface PhoneServer ()
@property(copy) NSString* url;
@property(copy) NSString* state;
@property long updates;
@property int port;
@property(strong) NSData* page;
@property(copy) void (^onOrientation)(float, float, float);
@property(strong) dispatch_queue_t queue;
@property(strong) nw_listener_t listener;
@property SecIdentityRef identity;
@property(strong) NSMutableSet<PhoneConnection*>* connections;
@property(copy) NSString* phone;      ///< what the phone said of itself, once it did
@property(copy) NSString* transport;  ///< websocket or post
- (void)took:(NSDictionary*)message over:(NSString*)transport reply:(void (^)(NSData*))reply;
@end

@implementation PhoneConnection
- (void)send:(NSData*)bytes {
    dispatch_data_t d = dispatch_data_create(bytes.bytes, bytes.length, self.server.queue, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    nw_connection_send(self.connection, d, NW_CONNECTION_DEFAULT_MESSAGE_CONTEXT, true, ^(nw_error_t error) {
    });
}
- (void)respond:(NSString*)status body:(NSData*)body type:(NSString*)type {
    NSString* head = [NSString stringWithFormat:@"HTTP/1.1 %@\r\nContent-Type: %@\r\nContent-Length: %lu\r\n"
                                                 "Cache-Control: no-store\r\nConnection: keep-alive\r\n\r\n",
                                                status, type, (unsigned long)body.length];
    NSMutableData* out = [[head dataUsingEncoding:NSUTF8StringEncoding] mutableCopy];
    [out appendData:body];
    [self send:out];
}
- (NSData*)frame:(uint8_t)opcode payload:(NSData*)payload {
    NSMutableData* f = [NSMutableData data];
    const uint8_t first = 0x80 | opcode;
    [f appendBytes:&first length:1];
    const NSUInteger n = payload.length;
    if (n < 126) {
        const uint8_t len = (uint8_t)n;
        [f appendBytes:&len length:1];
    } else if (n < 65536) {
        const uint8_t head[3] = {126, (uint8_t)(n >> 8), (uint8_t)n};
        [f appendBytes:head length:3];
    } else {
        uint8_t head[9] = {127};
        for (int i = 0; i < 8; ++i) head[1 + i] = (uint8_t)(n >> (56 - 8 * i));
        [f appendBytes:head length:9];
    }
    [f appendData:payload];
    return f;
}
/// One request from the buffer's front; NO when a whole one is not there yet.
- (BOOL)takeRequest {
    NSRange end = [self.buffer rangeOfData:[@"\r\n\r\n" dataUsingEncoding:NSUTF8StringEncoding]
                                   options:0
                                     range:NSMakeRange(0, self.buffer.length)];
    if (end.location == NSNotFound) return NO;
    NSString* head = [[NSString alloc] initWithData:[self.buffer subdataWithRange:NSMakeRange(0, end.location)]
                                           encoding:NSUTF8StringEncoding];
    if (head == nil) {
        nw_connection_cancel(self.connection);
        return NO;
    }
    NSArray<NSString*>* lines = [head componentsSeparatedByString:@"\r\n"];
    NSArray<NSString*>* request = [lines[0] componentsSeparatedByString:@" "];
    NSMutableDictionary<NSString*, NSString*>* headers = [NSMutableDictionary dictionary];
    for (NSUInteger i = 1; i < lines.count; ++i) {
        const NSRange colon = [lines[i] rangeOfString:@":"];
        if (colon.location == NSNotFound) continue;
        NSString* key = [[lines[i] substringToIndex:colon.location] lowercaseString];
        NSString* value = [[lines[i] substringFromIndex:colon.location + 1]
            stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        headers[key] = value;
    }
    const NSUInteger bodyLength = (NSUInteger)[headers[@"content-length"] integerValue];
    const NSUInteger total = end.location + 4 + bodyLength;
    if (self.buffer.length < total) return NO;
    NSData* body = [self.buffer subdataWithRange:NSMakeRange(end.location + 4, bodyLength)];
    [self.buffer replaceBytesInRange:NSMakeRange(0, total) withBytes:NULL length:0];
    NSString* method = request.count > 0 ? request[0] : @"";
    NSString* path = request.count > 1 ? [request[1] componentsSeparatedByString:@"?"][0] : @"";

    if ([[headers[@"upgrade"] lowercaseString] isEqualToString:@"websocket"] && headers[@"sec-websocket-key"] != nil) {
        NSData* keyed = [[headers[@"sec-websocket-key"] stringByAppendingString:kWebSocketGuid]
            dataUsingEncoding:NSUTF8StringEncoding];
        unsigned char digest[CC_SHA1_DIGEST_LENGTH];
        CC_SHA1(keyed.bytes, (CC_LONG)keyed.length, digest);
        NSString* accept = [[NSData dataWithBytes:digest length:sizeof digest] base64EncodedStringWithOptions:0];
        NSString* reply = [NSString stringWithFormat:@"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                                                      "Connection: Upgrade\r\nSec-WebSocket-Accept: %@\r\n\r\n",
                                                     accept];
        [self send:[reply dataUsingEncoding:NSUTF8StringEncoding]];
        self.websocket = YES;
        return YES;
    }
    if ([method isEqualToString:@"GET"] && [path isEqualToString:@"/"]) {
        [self respond:@"200 OK" body:self.server.page type:@"text/html; charset=utf-8"];
    } else if ([method isEqualToString:@"POST"] && [path isEqualToString:@"/post"]) {
        NSDictionary* message = [NSJSONSerialization JSONObjectWithData:body options:0 error:nil];
        if ([message isKindOfClass:[NSDictionary class]]) [self.server took:message over:@"post" reply:nil];
        [self respond:@"204 No Content" body:[NSData data] type:@"text/plain"];
    } else {
        [self respond:@"404 Not Found" body:[@"not here" dataUsingEncoding:NSUTF8StringEncoding] type:@"text/plain"];
    }
    return YES;
}
/// One frame from the buffer's front; NO when a whole one is not there yet.
- (BOOL)takeFrame {
    const uint8_t* b = self.buffer.bytes;
    NSUInteger have = self.buffer.length;
    if (have < 2) return NO;
    const uint8_t opcode = b[0] & 0x0F;
    const BOOL masked = (b[1] & 0x80) != 0;
    uint64_t n = b[1] & 0x7F;
    NSUInteger at = 2;
    if (n == 126) {
        if (have < 4) return NO;
        n = ((uint64_t)b[2] << 8) | b[3];
        at = 4;
    } else if (n == 127) {
        if (have < 10) return NO;
        n = 0;
        for (int i = 0; i < 8; ++i) n = (n << 8) | b[2 + i];
        at = 10;
    }
    if (n > 1 << 20) {
        nw_connection_cancel(self.connection);
        return NO;
    }
    const NSUInteger maskAt = at;
    if (masked) at += 4;
    if (have < at + n) return NO;
    NSMutableData* payload = [NSMutableData dataWithBytes:b + at length:(NSUInteger)n];
    if (masked) {
        uint8_t* p = payload.mutableBytes;
        for (NSUInteger i = 0; i < n; ++i) p[i] ^= b[maskAt + (i % 4)];
    }
    [self.buffer replaceBytesInRange:NSMakeRange(0, at + (NSUInteger)n) withBytes:NULL length:0];
    if (opcode == 0x8) {
        [self send:[self frame:0x8 payload:[NSData data]]];
        nw_connection_cancel(self.connection);
        return NO;
    }
    if (opcode == 0x9) {
        [self send:[self frame:0xA payload:payload]];
    } else if (opcode == 0x1) {
        NSDictionary* message = [NSJSONSerialization JSONObjectWithData:payload options:0 error:nil];
        if ([message isKindOfClass:[NSDictionary class]]) {
            __weak PhoneConnection* weakSelf = self;
            [self.server took:message
                         over:@"websocket"
                        reply:^(NSData* json) {
                          PhoneConnection* me = weakSelf;
                          if (me != nil) [me send:[me frame:0x1 payload:json]];
                        }];
        }
    }
    return YES;
}
- (void)drain {
    while (self.websocket ? [self takeFrame] : [self takeRequest]) {
    }
}
- (void)receive {
    __weak PhoneConnection* weakSelf = self;
    nw_connection_receive(self.connection, 1, 65536,
                          ^(dispatch_data_t content, nw_content_context_t context, bool complete, nw_error_t error) {
                            PhoneConnection* me = weakSelf;
                            if (me == nil) return;
                            if (content != NULL)
                                dispatch_data_apply(content, ^bool(dispatch_data_t region, size_t offset,
                                                                   const void* bytes, size_t size) {
                                  [me.buffer appendBytes:bytes length:size];
                                  return true;
                                });
                            [me drain];
                            if (error != NULL || complete)
                                nw_connection_cancel(me.connection);
                            else
                                [me receive];
                          });
}
@end

// ---- the server --------------------------------------------------------------------------------------------

@implementation PhoneServer
- (instancetype)initWithPort:(int)port
                        page:(NSData*)page
               onOrientation:(void (^)(float, float, float))onOrientation {
    self = [super init];
    if (self) {
        _port = port;
        _page = page;
        _onOrientation = onOrientation;
        _queue = dispatch_queue_create("org.bambi.decoder.helper.phone", DISPATCH_QUEUE_SERIAL);
        _connections = [NSMutableSet set];
        _url = @"";
        _state = @"not started";
        _transport = @"";
    }
    return self;
}
- (void)dealloc {
    if (_identity != NULL) CFRelease(_identity);
}
- (BOOL)start:(NSError**)error {
    const id fail = ^(NSString* why) {
      if (error != NULL)
          *error = [NSError errorWithDomain:@"bambi" code:1 userInfo:@{NSLocalizedDescriptionKey : why}];
      self.state = [@"the page cannot be served: " stringByAppendingString:why];
      return NO;
    };
    NSString* address = lanAddress();
    if (address.length == 0) return ((BOOL(^)(NSString*))fail)(@"no network");
    NSString* why = nil;
    self.identity = makeIdentity(address, &why);
    if (self.identity == NULL) return ((BOOL(^)(NSString*))fail)(why ?: @"no certificate");
    SecIdentityRef identity = self.identity;
    nw_parameters_t parameters = nw_parameters_create_secure_tcp(
        ^(nw_protocol_options_t tls) {
          sec_protocol_options_t options = nw_tls_copy_sec_protocol_options(tls);
          sec_protocol_options_set_local_identity(options, sec_identity_create(identity));
        },
        NW_PARAMETERS_DEFAULT_CONFIGURATION);
    nw_parameters_set_reuse_local_address(parameters, true);
    self.listener = nw_listener_create_with_port([NSString stringWithFormat:@"%d", self.port].UTF8String, parameters);
    if (self.listener == NULL) return ((BOOL(^)(NSString*))fail)(@"no listener");
    nw_listener_set_queue(self.listener, self.queue);
    __weak PhoneServer* weakSelf = self;
    nw_listener_set_new_connection_handler(self.listener, ^(nw_connection_t connection) {
      PhoneServer* me = weakSelf;
      if (me == nil) return;
      PhoneConnection* c = [[PhoneConnection alloc] init];
      c.connection = connection;
      c.buffer = [NSMutableData data];
      c.server = me;
      [me.connections addObject:c];
      nw_connection_set_queue(connection, me.queue);
      nw_connection_set_state_changed_handler(connection, ^(nw_connection_state_t state, nw_error_t e) {
        if (state == nw_connection_state_failed || state == nw_connection_state_cancelled) {
            PhoneServer* inner = weakSelf;
            if (inner != nil) [inner.connections removeObject:c];
        }
      });
      nw_connection_start(connection);
      [c receive];
    });
    nw_listener_set_state_changed_handler(self.listener, ^(nw_listener_state_t state, nw_error_t e) {
      PhoneServer* me = weakSelf;
      if (me == nil) return;
      if (state == nw_listener_state_failed)
          me.state = [NSString stringWithFormat:@"the page cannot be served: port %d refused", me.port];
    });
    nw_listener_start(self.listener);
    self.url = [NSString stringWithFormat:@"https://%@:%d/", address, self.port];
    self.state = @"waiting for a phone to open the page";
    return YES;
}
- (void)stop {
    if (self.listener != NULL) nw_listener_cancel(self.listener);
    for (PhoneConnection* c in [self.connections copy]) nw_connection_cancel(c.connection);
    [self.connections removeAllObjects];
}
- (void)took:(NSDictionary*)message over:(NSString*)transport reply:(void (^)(NSData*))reply {
    NSString* type = [message[@"type"] isKindOfClass:[NSString class]] ? message[@"type"] : @"";
    if ([type isEqualToString:@"o"]) {
        const float yaw = [message[@"yaw"] floatValue], pitch = [message[@"pitch"] floatValue],
                    roll = [message[@"roll"] floatValue];
        if (isfinite(yaw) && isfinite(pitch) && isfinite(roll)) {
            self.updates += 1;
            self.transport = transport;
            if (self.onOrientation != nil) self.onOrientation(yaw, pitch, roll);
        }
    } else if ([type isEqualToString:@"hello"]) {
        NSString* ua = [message[@"ua"] isKindOfClass:[NSString class]] ? message[@"ua"] : @"a phone";
        NSString* mount = [message[@"mount"] isKindOfClass:[NSString class]] ? message[@"mount"] : @"";
        self.phone = [NSString stringWithFormat:@"%@%@", [ua containsString:@"iPhone"] ? @"an iPhone" : [ua containsString:@"Android"] ? @"an Android phone" : @"a phone",
                                                         [mount isEqualToString:@"face"] ? @", held upright" : [mount isEqualToString:@"top"] ? @", worn on the band" : @""];
        self.transport = transport;
        self.state = [NSString stringWithFormat:@"%@ opened the page (%@)", self.phone, transport];
    } else if ([type isEqualToString:@"ping"] && reply != nil) {
        NSData* pong = [NSJSONSerialization dataWithJSONObject:@{@"type" : @"pong", @"t" : message[@"t"] ?: @0}
                                                       options:0
                                                         error:nil];
        if (pong != nil) reply(pong);
    }
}
@end
