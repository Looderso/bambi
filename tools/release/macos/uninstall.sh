#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Removes what the bambi installer put on this Mac: the plugins, the live host, its virtual audio driver and this
# script, then restarts Core Audio so the driver is unloaded. The Max for Live device in Live's User Library and the
# presets in ~/Library are left alone. Needs an administrator:
#
#   sudo "/Library/Application Support/bambi/uninstall.sh"
set -u
if [ "$(id -u)" -ne 0 ]; then
    echo "Run with administrator rights: sudo \"$0\"" >&2
    exit 1
fi
rm -rf /Library/Audio/Plug-Ins/VST3/bambi\ *.vst3 /Library/Audio/Plug-Ins/CLAP/bambi\ *.clap
rm -rf "/Applications/bambi live host.app"
driver=/Library/Audio/Plug-Ins/HAL/bambi.driver
if [ -d "$driver" ]; then
    rm -rf "$driver"
    killall coreaudiod 2>/dev/null
fi
for id in $(pkgutil --pkgs='org\.bambi\.pkg\..*'); do pkgutil --forget "$id" >/dev/null; done
rm -rf "/Library/Application Support/bambi"
echo "bambi removed"
