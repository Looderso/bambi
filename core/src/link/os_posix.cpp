// SPDX-License-Identifier: GPL-3.0-or-later
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "os.hpp"

namespace bambi::detail {

bool mapShared(const std::string& name, std::size_t bytes, std::intptr_t& handle, void*& map, std::string& error) {
    map = nullptr;
    handle = -1;
    const int fd = ::shm_open(name.c_str(), O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        error = "shm_open failed: " + std::string(std::strerror(errno));
        return false;
    }

    /*  Size the object, tolerating the race: macOS permits ftruncate on a POSIX shared-memory object exactly once,
     *  so two instances opening the same object at the same moment both see size 0, and the one that loses gets
     *  EINVAL rather than a harmless no-op. Treating a failed ftruncate as fatal would fail to join a session about
     *  two thirds of the time two plugins load together, which is what loading a project does -- so the question
     *  is never "did ftruncate succeed" but "is the object big enough now"; only the second is fatal, and the
     *  brief retry covers the window where the creator has claimed the object but not yet sized it.
     *
     *  The retry is short on purpose: this runs on a message thread, where sleeping freezes every plugin window
     *  the host has open, and the race it covers is microseconds wide. A caller that still finds the object
     *  unsized retries on its own timer instead, trading a hiccup at load for 200 ms of frozen interface every two
     *  seconds otherwise. */
    bool sized = false;
    for (int attempt = 0; attempt < 40 && !sized; ++attempt) {
        struct stat st{};
        const bool known = ::fstat(fd, &st) == 0;
        if (known && static_cast<std::size_t>(st.st_size) >= bytes) {
            sized = true;
            break;
        }
        if (::ftruncate(fd, static_cast<off_t>(bytes)) == 0) {
            sized = true;
            break;
        }
        //  Sized already, and too small: a segment another build left, which no wait will grow. Only an
        //  unsized one is a creator mid-flight.
        if (known && st.st_size > 0) break;
        ::usleep(500);  // the creator is mid-flight; it will be sized in a moment
    }
    if (!sized) {
        struct stat st{};
        error = ::fstat(fd, &st) == 0 && st.st_size > 0
                    ? "shared segment is smaller than this build expects; it was probably created by a different "
                      "bambi version"
                    : "could not size the shared segment: " + std::string(std::strerror(errno));
        ::close(fd);
        return false;
    }

    //  Never map more than the object actually holds. Reading past the end of a shared
    //  memory object is a SIGBUS inside the host process, which as a plugin failure mode is
    //  as bad as it gets -- and an older build meeting a newer one's larger segment is a
    //  perfectly ordinary way to arrive here.
    {
        struct stat st{};
        if (::fstat(fd, &st) != 0 || static_cast<std::size_t>(st.st_size) < bytes) {
            error =
                "shared segment is smaller than this build expects; it was probably "
                "created by a different bambi version";
            ::close(fd);
            return false;
        }
    }

    void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        error = "mmap failed: " + std::string(std::strerror(errno));
        ::close(fd);
        return false;
    }
    map = p;
    handle = fd;
    return true;
}

void unmapShared(void*& map, std::size_t bytes, std::intptr_t& handle) {
    if (map != nullptr) ::munmap(map, bytes);
    if (handle >= 0) ::close(static_cast<int>(handle));
    map = nullptr;
    handle = -1;
}

void unlinkShared(const std::string& name) { ::shm_unlink(name.c_str()); }

std::uint64_t monotonicMicros() {
    //  CLOCK_MONOTONIC is system-wide on macOS and Linux, so two processes comparing these values agree.
    //  steady_clock is not guaranteed to be, and the wall clock can step backwards.
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000ull + static_cast<std::uint64_t>(ts.tv_nsec) / 1000ull;
}

}  // namespace bambi::detail
