// SPDX-License-Identifier: GPL-3.0-or-later
#include "HelperLink.h"

#include <juce_core/juce_core.h>

#if JUCE_MAC
#import <Cocoa/Cocoa.h>
#include <mutex>
#include <unistd.h>

struct HelperLink::Impl {
    std::mutex lock;
    NSRunningApplication* app = nil;
    bool pending = false;  ///< a launch asked for, its completion not yet in
};

HelperLink::HelperLink() : impl_(std::make_shared<Impl>()) {}
HelperLink::~HelperLink() { terminate(); }
bool HelperLink::available() { return true; }

std::string HelperLink::bundledHelperPath() {
    //  .../bambi Decoder.vst3/Contents/MacOS/bambi Decoder -> .../Contents/Helpers/bambi Decoder Helper.app
    const juce::File app = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                               .getParentDirectory()
                               .getParentDirectory()
                               .getChildFile("Helpers/bambi Decoder Helper.app");
    return app.isDirectory() ? app.getFullPathName().toStdString() : std::string();
}

namespace {
/// The launch itself, on the shared state: the completion may land after the launcher is gone.
void start(const std::shared_ptr<HelperLink::Impl>& impl, const std::string& appPath, int port, bool phone) {
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:appPath.c_str()]];
    NSWorkspaceOpenConfiguration* config = [NSWorkspaceOpenConfiguration configuration];
    config.activates = NO;
    config.addsToRecentItems = NO;
    config.createsNewApplicationInstance = NO;
    NSMutableArray<NSString*>* arguments = [NSMutableArray arrayWithArray:@[
        @"--host-pid", [NSString stringWithFormat:@"%d", getpid()], @"--port", [NSString stringWithFormat:@"%d", port]
    ]];
    if (phone) [arguments addObject:@"--phone"];
    config.arguments = arguments;
    [[NSWorkspace sharedWorkspace] openApplicationAtURL:url
                                          configuration:config
                                      completionHandler:^(NSRunningApplication* app, NSError* e) {
                                        std::lock_guard<std::mutex> g(impl->lock);
                                        impl->pending = false;
                                        if (e == nil) impl->app = app;
                                      }];
}

/// Polls every 100 ms, up to five seconds, until `old` has quit, then starts.
void startWhenGone(const std::shared_ptr<HelperLink::Impl>& impl, NSRunningApplication* old, const std::string& appPath,
                   int port, bool phone, int triesLeft) {
    if (old == nil || old.terminated || triesLeft <= 0) {
        start(impl, appPath, port, phone);
        return;
    }
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(100 * NSEC_PER_MSEC)), dispatch_get_main_queue(),
                   ^{ startWhenGone(impl, old, appPath, port, phone, triesLeft - 1); });
}
}  // namespace

bool HelperLink::launch(const std::string& appPath, int port, bool phone) {
    if (appPath.empty() || running()) return false;
    {
        std::lock_guard<std::mutex> g(impl_->lock);
        if (impl_->pending) return false;
        impl_->pending = true;
    }
    start(impl_, appPath, port, phone);
    return true;
}

void HelperLink::relaunch(const std::string& appPath, int port, bool phone) {
    if (appPath.empty()) return;
    NSRunningApplication* old = nil;
    {
        std::lock_guard<std::mutex> g(impl_->lock);
        if (impl_->pending) return;  // a launch under way: the next connect sees its mode and asks again
        old = impl_->app;
        impl_->app = nil;
        impl_->pending = true;
    }
    if (old != nil) [old terminate];
    startWhenGone(impl_, old, appPath, port, phone, 50);
}

void HelperLink::terminate() {
    std::lock_guard<std::mutex> g(impl_->lock);
    if (impl_->app != nil) {
        [impl_->app terminate];
        impl_->app = nil;
    }
}

bool HelperLink::running() const {
    std::lock_guard<std::mutex> g(impl_->lock);
    if (impl_->app != nil && impl_->app.terminated) impl_->app = nil;  // it quit on its own: with the host, or asked
    return impl_->app != nil;
}
#else
struct HelperLink::Impl {};
HelperLink::HelperLink() = default;
HelperLink::~HelperLink() = default;
bool HelperLink::available() { return false; }
std::string HelperLink::bundledHelperPath() { return {}; }
bool HelperLink::launch(const std::string&, int, bool) { return false; }
void HelperLink::relaunch(const std::string&, int, bool) {}
void HelperLink::terminate() {}
bool HelperLink::running() const { return false; }
#endif
