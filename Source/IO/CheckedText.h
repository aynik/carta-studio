/**
 * Durable temporary staging and atomic commit helpers for text and byte files.
 *
 * Implements the write-fsync-close -> atomic rename -> parent-directory fsync
 * contract so partial writes or crashes never corrupt an existing target file.
 * Post-commit directory sync outcome is reported separately from replacement.
 */

#pragma once

#include <juce_core/juce_core.h>
#include "IO/CheckedOutput.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <limits>
#include <utility>
#if JUCE_MAC || JUCE_LINUX
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace carta {

/**
 * Write all bytes, fsync, and close even after a write or sync failure.
 *
 * The caller must not commit the staged file unless every step succeeds.
 */
template <typename Write, typename Sync, typename Close>
bool writeSyncAndClose(const void* data, size_t bytes, Write&& write, Sync&& sync, Close&& close) {
    const bool written = writeAllBytes(data, bytes, std::forward<Write>(write));
    const bool synced = written && sync();
    const bool closed = close();
    return written && synced && closed;
}

/**
 * Result of synchronizing a destination's parent directory after rename.
 *
 * A successful rename has already changed the destination, so directory sync
 * status is reported separately to avoid misclassifying post-commit failures
 * as failed replacements.
 */
enum class DirectorySyncStatus { confirmed, unconfirmed, unsupported };

/** Execute directory sync followed by close, always invoking close. */
template <typename Sync, typename Close> bool syncAndCloseDirectory(Sync&& sync, Close&& close) {
    const bool synced = sync();
    const bool closed = close();
    return synced && closed;
}

#if defined(CARTA_STUDIO_COMPONENT_TEST) && CARTA_STUDIO_COMPONENT_TEST
/** Test-only fault injection; the save still performs its real rename first. */
inline std::atomic<bool> forceDirectorySyncFailure{false};
#endif

/** Flush directory metadata for `target`'s parent directory after commit. */
inline DirectorySyncStatus syncParentDirectory(const juce::File& target) {
#if defined(CARTA_STUDIO_COMPONENT_TEST) && CARTA_STUDIO_COMPONENT_TEST
    if (forceDirectorySyncFailure.load(std::memory_order_relaxed)) {
        return DirectorySyncStatus::unconfirmed;
    }
#endif
#if JUCE_MAC || JUCE_LINUX
    int flags = O_RDONLY | O_CLOEXEC;
#if JUCE_LINUX
    flags |= O_DIRECTORY;
#endif
    int fd;
    do {
        fd = ::open(target.getParentDirectory().getFullPathName().toRawUTF8(), flags);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        return DirectorySyncStatus::unconfirmed;
    }
    const bool ok = syncAndCloseDirectory(
        [fd] {
            int result;
            do {
                result = ::fsync(fd);
            } while (result < 0 && errno == EINTR);
            return result == 0;
        },
        [fd] { return ::close(fd) == 0; });
    return ok ? DirectorySyncStatus::confirmed : DirectorySyncStatus::unconfirmed;
#else
    juce::ignoreUnused(target);
    return DirectorySyncStatus::unsupported;
#endif
}

/**
 * Atomically replace `target` with a same-directory temporary file.
 *
 * Never falls back to copying over the destination if rename fails, because a
 * partial copy could damage the existing file.
 */
inline bool replaceTemporaryAtomically(juce::TemporaryFile& temp, const juce::File& target) {
#if JUCE_MAC || JUCE_LINUX
    return ::rename(temp.getFile().getFullPathName().toRawUTF8(),
                    target.getFullPathName().toRawUTF8()) == 0;
#else
    juce::ignoreUnused(target);
    return temp.overwriteTargetFileWithTemporary();
#endif
}

/**
 * Stage, flush, and close a complete temporary file in `target`'s directory.
 *
 * The returned object owns only a file created by this call and may be
 * committed after any final cancellation check. Failures clean up and return
 * nullptr.
 */
inline std::unique_ptr<juce::TemporaryFile> prepareCheckedBytes(const juce::File& target,
                                                                const void* data, size_t size) {
#if JUCE_MAC || JUCE_LINUX
    juce::File path;
    int fd = -1;
    for (int attempt = 0; attempt < 16 && fd < 0; ++attempt) {
        path = target.getParentDirectory().getChildFile(".carta-text-" + juce::Uuid().toString());
        fd = ::open(path.getFullPathName().toRawUTF8(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                    0666);
        if (fd < 0 && errno != EEXIST) {
            break;
        }
    }
    if (fd < 0) {
        return {};
    }

    std::unique_ptr<juce::TemporaryFile> temp;
    try {
        temp = std::make_unique<juce::TemporaryFile>(target, path);
    } catch (...) {
        ::close(fd);
        path.deleteFile();
        throw;
    }

    const auto write = [fd](const void* chunk, size_t count) {
        ssize_t result;
        do {
            result =
                ::write(fd, chunk,
                        std::min(count, static_cast<size_t>(std::numeric_limits<ssize_t>::max())));
        } while (result < 0 && errno == EINTR);
        return result;
    };
    const auto sync = [fd] {
        int result;
        do {
            result = ::fsync(fd);
        } while (result < 0 && errno == EINTR);
        return result == 0;
    };
    const auto close = [fd] { return ::close(fd) == 0; };
    if (!writeSyncAndClose(data, size, write, sync, close)) {
        return {};
    }
    return temp;
#else
    auto temp = std::make_unique<juce::TemporaryFile>(target);
    auto stream = temp->getFile().createOutputStream();
    if (!stream || stream->getStatus().failed() || !stream->setPosition(0) ||
        stream->truncate().failed()) {
        return {};
    }
    if (!stream->write(data, size)) {
        return {};
    }
    stream->flush();
    if (stream->getStatus().failed()) {
        return {};
    }
    stream.reset();
    if (temp->getFile().getSize() != static_cast<juce::int64>(size)) {
        return {};
    }
    return temp;
#endif
}

/** Stage a UTF-8 string into a durable temporary file in `target`'s directory. */
inline std::unique_ptr<juce::TemporaryFile> prepareCheckedText(const juce::File& target,
                                                               const juce::String& text) {
    return prepareCheckedBytes(target, text.toRawUTF8(), text.getNumBytesAsUTF8());
}

} // namespace carta
