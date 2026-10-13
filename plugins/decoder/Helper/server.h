// SPDX-License-Identifier: GPL-3.0-or-later
//  The phone page's server: HTTPS on a certificate made for this computer's address, the page at /, a WebSocket
//  at /ws and POSTs at /post carrying the phone's orientation as the head's yaw, pitch and roll in degrees.
#import <Foundation/Foundation.h>

@interface PhoneServer : NSObject
/// `onOrientation` is called on the server's queue for every orientation the phone sends.
- (instancetype)initWithPort:(int)port
                        page:(NSData*)page
               onOrientation:(void (^)(float yawDeg, float pitchDeg, float rollDeg))onOrientation;
/// Finds the address, makes or reuses the certificate, starts listening. NO with `error` when any of that fails.
- (BOOL)start:(NSError**)error;
- (void)stop;
@property(readonly, copy) NSString* url;    ///< https://<address>:<port>/ once started, else empty
@property(readonly, copy) NSString* state;  ///< what a status line says: waiting, or the phone and its rate
@property(readonly) long updates;           ///< orientations received so far
@end
