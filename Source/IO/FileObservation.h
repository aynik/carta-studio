/**
 * Point-in-time file identity observation for detecting concurrent external
 * modification or replacement during multi-step reads and project saves.
 */

#pragma once

#include <juce_core/juce_core.h>
#if JUCE_MAC || JUCE_LINUX
#include <sys/stat.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace carta {

/**
 * Point-in-time path observation.
 *
 * Captures device, inode, size, mode, and nanosecond modification/change
 * timestamps on POSIX platforms (or volume serial, NTFS file index, size, and
 * write timestamp on Windows) to detect ordinary replacement and modification
 * during a read or save. It cannot lock out concurrent or later external
 * changes.
 */
class FileObservation {
  public:
    /** Record the current identity of `file`, or mark invalid if inaccessible. */
    explicit FileObservation(const juce::File& file) {
#if JUCE_MAC || JUCE_LINUX
        valid = ::stat(file.getFullPathName().toRawUTF8(), &identity) == 0;
#elif defined(_WIN32)
        valid = queryWindowsIdentity(file, winIdentity);
#else
        valid = file.existsAsFile();
        size = file.getSize();
        modified = file.getLastModificationTime();
#endif
    }

    /** Return true if the initial stat/probe succeeded. */
    bool isValid() const noexcept { return valid; }

#if JUCE_MAC || JUCE_LINUX
    /** Return true if the observed target exists and is a regular file. */
    bool isRegularFile() const noexcept { return valid && S_ISREG(identity.st_mode); }

    /** Return true if an open file descriptor still matches the initial stat. */
    bool matchesDescriptor(int descriptor) const noexcept {
        struct stat current{};
        return valid && ::fstat(descriptor, &current) == 0 && sameIdentity(current);
    }
#endif

    /** Return true if `file` currently matches the initial observation. */
    bool matches(const juce::File& file) const {
        if (!valid) {
            return false;
        }
#if JUCE_MAC || JUCE_LINUX
        struct stat current{};
        return ::stat(file.getFullPathName().toRawUTF8(), &current) == 0 && sameIdentity(current);
#elif defined(_WIN32)
        BY_HANDLE_FILE_INFORMATION current{};
        if (!queryWindowsIdentity(file, current)) {
            return false;
        }
        return winIdentity.dwVolumeSerialNumber == current.dwVolumeSerialNumber &&
               winIdentity.nFileIndexHigh == current.nFileIndexHigh &&
               winIdentity.nFileIndexLow == current.nFileIndexLow &&
               winIdentity.nFileSizeHigh == current.nFileSizeHigh &&
               winIdentity.nFileSizeLow == current.nFileSizeLow &&
               winIdentity.ftLastWriteTime.dwHighDateTime ==
                   current.ftLastWriteTime.dwHighDateTime &&
               winIdentity.ftLastWriteTime.dwLowDateTime == current.ftLastWriteTime.dwLowDateTime;
#else
        return file.existsAsFile() && size == file.getSize() &&
               modified == file.getLastModificationTime();
#endif
    }

  private:
#if defined(_WIN32)
    static bool queryWindowsIdentity(const juce::File& file,
                                     BY_HANDLE_FILE_INFORMATION& out) noexcept {
        const auto wide = file.getFullPathName().toWideCharPointer();
        HANDLE h = CreateFileW(wide, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            return false;
        }
        const BOOL ok = GetFileInformationByHandle(h, &out);
        CloseHandle(h);
        return ok != FALSE && (out.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }
#endif

#if JUCE_MAC || JUCE_LINUX
    bool sameIdentity(const struct stat& current) const noexcept {
        if (identity.st_dev != current.st_dev || identity.st_ino != current.st_ino ||
            identity.st_size != current.st_size || identity.st_mode != current.st_mode) {
            return false;
        }
#if JUCE_MAC
        return identity.st_mtimespec.tv_sec == current.st_mtimespec.tv_sec &&
               identity.st_mtimespec.tv_nsec == current.st_mtimespec.tv_nsec &&
               identity.st_ctimespec.tv_sec == current.st_ctimespec.tv_sec &&
               identity.st_ctimespec.tv_nsec == current.st_ctimespec.tv_nsec;
#else
        return identity.st_mtim.tv_sec == current.st_mtim.tv_sec &&
               identity.st_mtim.tv_nsec == current.st_mtim.tv_nsec &&
               identity.st_ctim.tv_sec == current.st_ctim.tv_sec &&
               identity.st_ctim.tv_nsec == current.st_ctim.tv_nsec;
#endif
    }
#endif

    bool valid = false;
#if JUCE_MAC || JUCE_LINUX
    struct stat identity{};
#elif defined(_WIN32)
    BY_HANDLE_FILE_INFORMATION winIdentity{};
#else
    juce::int64 size = 0;
    juce::Time modified;
#endif
};

} // namespace carta
