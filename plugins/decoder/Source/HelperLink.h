// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <string>

/*  Launches the helper app nested in the plugin's bundle through Launch Services, so it is its own responsible
 *  process for the motion permission, and ends it again. macOS only; elsewhere every call is a no-op and
 *  available() is false.
 */
class HelperLink {
public:
    HelperLink();
    ~HelperLink();
    static bool available();
    /// The helper app inside this plugin's bundle, or empty when there is none there.
    static std::string bundledHelperPath();
    /// Launches the helper to send to `port`, serving the phone page when `phone`. False when it did not: no path,
    /// one running already, or a launch still pending.
    bool launch(const std::string& appPath, int port, bool phone);
    void terminate();
    bool running() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;  ///< shared with the launch completion, which may outlive this
};
