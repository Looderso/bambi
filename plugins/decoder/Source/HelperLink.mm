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

void HelperLink::launch(const std::string& appPath, int port) {
    if (appPath.empty() || running()) return;
    {
        std::lock_guard<std::mutex> g(impl_->lock);
        if (impl_->pending) return;
        impl_->pending = true;
    }
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:appPath.c_str()]];
    NSWorkspaceOpenConfiguration* config = [NSWorkspaceOpenConfiguration configuration];
    config.activates = NO;
    config.addsToRecentItems = NO;
    config.createsNewApplicationInstance = NO;
    config.arguments = @[
        @"--host-pid", [NSString stringWithFormat:@"%d", getpid()], @"--port", [NSString stringWithFormat:@"%d", port]
    ];
    std::shared_ptr<Impl> impl = impl_;
    [[NSWorkspace sharedWorkspace] openApplicationAtURL:url
                                          configuration:config
                                      completionHandler:^(NSRunningApplication* app, NSError* e) {
                                        std::lock_guard<std::mutex> g(impl->lock);
                                        impl->pending = false;
                                        if (e == nil) impl->app = app;
                                      }];
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
void HelperLink::launch(const std::string&, int) {}
void HelperLink::terminate() {}
bool HelperLink::running() const { return false; }
#endif
