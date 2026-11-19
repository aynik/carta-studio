/**
 * Local Unix domain socket IPC server, CLI client, and headless daemon control.
 *
 * `IpcServer` binds a private single-instance endpoint (`control.sock` guarded
 * by a `flock` lockfile) and dispatches newline-delimited JSON requests onto the
 * JUCE message thread via `IpcRequestGate`. `runIpcClient` handles `--help`,
 * `daemon start|stop|status`, and `ipc [--auto-start]` CLI invocations.
 */

#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <functional>
#include <memory>

namespace carta {

/**
 * Lock-free synchronization gate resolving the race between asynchronous
 * message-thread command dispatch and the IPC worker's timeout.
 */
class IpcRequestGate {
  public:
    /** Transition from `queued` to `running`; returns false if already cancelled. */
    bool tryStart() noexcept {
        State expected = State::queued;
        return state.compare_exchange_strong(expected, State::running);
    }

    /** Transition from `queued` to `cancelled`; returns false if already started. */
    bool cancelIfQueued() noexcept {
        State expected = State::queued;
        return state.compare_exchange_strong(expected, State::cancelled);
    }

  private:
    enum class State { queued, running, cancelled };
    std::atomic<State> state{State::queued};
};

/**
 * Local single-instance automation endpoint over a Unix domain stream socket.
 *
 * Request callbacks execute synchronously on the JUCE message thread, and
 * JSON responses are serialized before releasing the message thread.
 */
class IpcServer {
  public:
    /**
     * Start listening in `socketDirectory` (or the default per-user endpoint).
     *
     * Check `error()` after construction; bind/lock failures do not throw.
     */
    explicit IpcServer(std::function<juce::var(const juce::StringArray&)> handler,
                       juce::File socketDirectory = {});
    ~IpcServer();

    /** Return an empty string when active, or the initialization error message. */
    juce::String error() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

/**
 * Handle CLI IPC, daemon lifecycle, or `--help` arguments.
 *
 * Returns `-1` when arguments do not invoke a CLI subcommand and should instead
 * start the graphical application.
 */
int runIpcClient(int argc, char* argv[]);

} // namespace carta
