/**
 * Checked output stream primitives for Carta Studio I/O.
 *
 * Provides a partial-write completion helper and a JUCE OutputStream adapter
 * that latches seek and write failures for writers that do not retain stream
 * error state across header rewrites and flushes.
 */

#pragma once

#include <juce_core/juce_core.h>

namespace carta {

/**
 * Complete a write across partial progress reported by a byte-sink callback.
 *
 * The write callback receives `(cursor, remainingBytes)` and must return a
 * positive byte count on progress, or a nonpositive value on error. Returns
 * false if the callback errors or reports more bytes than requested.
 */
template <typename Write> bool writeAllBytes(const void* data, size_t bytes, Write&& write) {
    const auto* cursor = static_cast<const unsigned char*>(data);
    while (bytes > 0) {
        const auto count = write(cursor, bytes);
        if (count <= 0 || static_cast<size_t>(count) > bytes) {
            return false;
        }
        cursor += static_cast<size_t>(count);
        bytes -= static_cast<size_t>(count);
    }
    return true;
}

/**
 * OutputStream adapter that latches seek and write errors into a caller flag.
 *
 * The audio writer owns this adapter, while the caller must keep both the
 * underlying destination stream and the failure flag alive until after the
 * writer destructor completes its final header rewrite.
 */
class CheckedOutputStream final : public juce::OutputStream {
  public:
    /** Bind an output destination and a caller-owned failure flag. */
    CheckedOutputStream(juce::OutputStream& destination, bool& failed)
        : target(destination), failure(failed) {}

    /** Forward flush requests to the underlying stream. */
    void flush() override { target.flush(); }

    /** Return the current byte offset of the underlying stream. */
    juce::int64 getPosition() override { return target.getPosition(); }

    /** Seek the underlying stream and latch any failure. */
    bool setPosition(juce::int64 position) override {
        const bool ok = target.setPosition(position);
        failure |= !ok;
        return ok;
    }

    /** Write bytes to the underlying stream and latch any failure. */
    bool write(const void* data, size_t bytes) override {
        const bool ok = target.write(data, bytes);
        failure |= !ok;
        return ok;
    }

  private:
    juce::OutputStream& target;
    bool& failure;
};

} // namespace carta
