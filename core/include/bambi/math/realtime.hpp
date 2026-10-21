// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/*  Marks a function as real-time: in a build with -fsanitize=realtime, everything it calls that may allocate, free,
 *  lock or make a system call stops the program with a report naming the call. Elsewhere it is empty. Put it on each
 *  audio entry point, declaration and definition alike, never on message-thread work.
 */
#if defined(__has_feature)
#if __has_feature(realtime_sanitizer)
#define BAMBI_REALTIME [[clang::nonblocking]]
#endif
#endif
#ifndef BAMBI_REALTIME
#define BAMBI_REALTIME
#endif
