// SPDX-License-Identifier: MIT
// Transport abstraction over the local IPC channel.
//
// Windows uses a named pipe with an explicit DACL (the shipping configuration). POSIX uses an
// AF_UNIX stream socket, which exists so the identical IpcServer / OverlayClient logic runs in
// CI on Linux against a real kernel transport instead of a mock. It is not a shipping target.
#ifndef OBSN_TRANSPORT_HPP
#define OBSN_TRANSPORT_HPP

#include <cstddef>
#include <memory>
#include <string>

namespace obsn {

/// One duplex byte stream. Every method is safe to call from a single owning thread; `cancel()`
/// is the exception and may be called from any thread to unblock a pending read or write.
class Connection {
public:
    virtual ~Connection() = default;

    /// Blocks until at least one byte arrives. Returns the byte count, 0 on a clean peer
    /// shutdown, or -1 on error or cancellation.
    virtual int read(char* buffer, std::size_t size) = 0;

    /// Writes the whole buffer. Returns true on success, false on error or cancellation.
    virtual bool write_all(const char* data, std::size_t size) = 0;

    /// Unblocks a pending read/write from another thread and marks the connection dead.
    virtual void cancel() = 0;

    virtual bool valid() const = 0;
    virtual std::string peer_description() const { return "local"; }
};

class ServerTransport {
public:
    virtual ~ServerTransport() = default;

    /// Waits for an incoming connection. Returns nullptr on timeout, on stop(), or on error.
    /// `timeout_ms` < 0 waits indefinitely.
    virtual std::unique_ptr<Connection> accept(int timeout_ms) = 0;

    /// Unblocks accept() from another thread and refuses further connections.
    virtual void stop() = 0;

    virtual std::string endpoint() const = 0;
};

/// Platform-appropriate endpoint for `name`. On Windows: `\\.\pipe\<name>`, with the current
/// user's SID appended when `name` is empty. On POSIX: a path under the user's runtime dir.
std::string default_endpoint(const std::string& name_override);

/// Creates the listening endpoint. On Windows the pipe is created with a security descriptor
/// granting only the creating user and SYSTEM, and with PIPE_REJECT_REMOTE_CLIENTS set.
/// Returns nullptr and fills `error` on failure.
std::unique_ptr<ServerTransport> create_server(const std::string& endpoint, std::string& error);

/// Connects to an existing endpoint. Returns nullptr and fills `error` when unavailable; a
/// missing endpoint (OBS not running, or the script not loaded) is the normal case and is
/// reported as an ordinary error, not a fault.
std::unique_ptr<Connection> connect_client(const std::string& endpoint, int timeout_ms,
                                           std::string& error);

}  // namespace obsn

#endif  // OBSN_TRANSPORT_HPP
