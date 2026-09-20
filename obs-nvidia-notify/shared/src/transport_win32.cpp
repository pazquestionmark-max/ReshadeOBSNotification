// SPDX-License-Identifier: MIT
// Windows named-pipe transport — the shipping configuration.
//
// Security properties established here (see docs/architecture.md §6.5):
//   * explicit DACL: only the creating user's SID and SYSTEM get read/write; no Everyone ACE,
//     and never a NULL DACL (which would grant everyone full control);
//   * PIPE_REJECT_REMOTE_CLIENTS, so the pipe cannot be reached across the network;
//   * FIRST_PIPE_INSTANCE on the first instance, so another process cannot pre-create the pipe
//     name and impersonate the plugin before it starts.
#if defined(_WIN32)

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "obsn/transport.hpp"

namespace obsn {
namespace {

constexpr DWORD kPipeBufferBytes = 64 * 1024;

std::string last_error_string(DWORD code) {
    char* buffer = nullptr;
    const DWORD n = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<char*>(&buffer), 0, nullptr);
    std::string out = n != 0 && buffer != nullptr ? std::string(buffer, n) : std::string();
    if (buffer != nullptr) LocalFree(buffer);
    while (!out.empty() && (out.back() == '\r' || out.back() == '\n')) out.pop_back();
    if (out.empty()) out = "error " + std::to_string(code);
    return out;
}

/// Current user's SID as a string, used both in the pipe name and in its DACL.
bool current_user_sid(std::string& out) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (size == 0) {
        CloseHandle(token);
        return false;
    }
    std::vector<unsigned char> buffer(size);
    if (!GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
        CloseHandle(token);
        return false;
    }
    CloseHandle(token);
    const TOKEN_USER* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    char* sid_text = nullptr;
    if (!ConvertSidToStringSidA(user->User.Sid, &sid_text)) return false;
    out = sid_text;
    LocalFree(sid_text);
    return true;
}

/// Builds a self-relative security descriptor granting the current user and SYSTEM, nobody else.
class PipeSecurity {
public:
    bool build(std::string& error) {
        std::string sid;
        if (!current_user_sid(sid)) {
            error = "could not determine the current user's SID";
            return false;
        }
        // SDDL: D: (DACL) with two ACEs granting generic read+write, and no inheritance.
        const std::string sddl = "D:(A;;GRGW;;;" + sid + ")(A;;GRGW;;;SY)";
        PSECURITY_DESCRIPTOR raw = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(
                sddl.c_str(), SDDL_REVISION_1, &raw, nullptr)) {
            error = "could not build the pipe security descriptor: " +
                    last_error_string(GetLastError());
            return false;
        }
        descriptor_ = raw;
        attributes_.nLength = sizeof(SECURITY_ATTRIBUTES);
        attributes_.lpSecurityDescriptor = descriptor_;
        attributes_.bInheritHandle = FALSE;
        return true;
    }
    ~PipeSecurity() {
        if (descriptor_ != nullptr) LocalFree(descriptor_);
    }
    PipeSecurity() = default;
    PipeSecurity(const PipeSecurity&) = delete;
    PipeSecurity& operator=(const PipeSecurity&) = delete;
    // Movable so the descriptor can be handed to PipeServer without duplicating it; copying it
    // would double-free the LocalAlloc'd descriptor.
    PipeSecurity(PipeSecurity&& other) noexcept
        : descriptor_(other.descriptor_), attributes_(other.attributes_) {
        other.descriptor_ = nullptr;
        other.attributes_ = SECURITY_ATTRIBUTES{};
        if (descriptor_ != nullptr) attributes_.lpSecurityDescriptor = descriptor_;
    }
    PipeSecurity& operator=(PipeSecurity&& other) noexcept {
        if (this != &other) {
            if (descriptor_ != nullptr) LocalFree(descriptor_);
            descriptor_ = other.descriptor_;
            attributes_ = other.attributes_;
            other.descriptor_ = nullptr;
            other.attributes_ = SECURITY_ATTRIBUTES{};
            if (descriptor_ != nullptr) attributes_.lpSecurityDescriptor = descriptor_;
        }
        return *this;
    }

    SECURITY_ATTRIBUTES* attributes() { return &attributes_; }

private:
    PSECURITY_DESCRIPTOR descriptor_ = nullptr;
    SECURITY_ATTRIBUTES attributes_{};
};

/// Overlapped I/O plus a manual-reset stop event, so a blocked read can be cancelled from
/// another thread without closing a handle out from under it.
class PipeConnection final : public Connection {
public:
    explicit PipeConnection(HANDLE pipe) : pipe_(pipe) {
        read_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        write_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    ~PipeConnection() override {
        cancel();
        if (pipe_ != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(pipe_);
            DisconnectNamedPipe(pipe_);
            CloseHandle(pipe_);
        }
        if (read_event_ != nullptr) CloseHandle(read_event_);
        if (write_event_ != nullptr) CloseHandle(write_event_);
        if (stop_event_ != nullptr) CloseHandle(stop_event_);
    }

    int read(char* buffer, std::size_t size) override {
        if (!valid()) return -1;
        OVERLAPPED ov{};
        ov.hEvent = read_event_;
        ResetEvent(read_event_);
        DWORD got = 0;
        if (!ReadFile(pipe_, buffer, static_cast<DWORD>(size), &got, &ov)) {
            const DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED) return 0;
            if (err != ERROR_IO_PENDING) return -1;
            HANDLE waits[2] = {read_event_, stop_event_};
            const DWORD which = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (which != WAIT_OBJECT_0) {
                CancelIoEx(pipe_, &ov);
                // Reap the cancelled request so the OVERLAPPED is not still in use when it
                // leaves scope.
                DWORD discarded = 0;
                GetOverlappedResult(pipe_, &ov, &discarded, TRUE);
                return -1;
            }
            if (!GetOverlappedResult(pipe_, &ov, &got, FALSE)) {
                const DWORD e2 = GetLastError();
                return (e2 == ERROR_BROKEN_PIPE || e2 == ERROR_PIPE_NOT_CONNECTED) ? 0 : -1;
            }
        }
        return static_cast<int>(got);
    }

    bool write_all(const char* data, std::size_t size) override {
        std::size_t sent = 0;
        while (sent < size) {
            if (!valid()) return false;
            OVERLAPPED ov{};
            ov.hEvent = write_event_;
            ResetEvent(write_event_);
            DWORD written = 0;
            if (!WriteFile(pipe_, data + sent, static_cast<DWORD>(size - sent), &written, &ov)) {
                if (GetLastError() != ERROR_IO_PENDING) return false;
                HANDLE waits[2] = {write_event_, stop_event_};
                const DWORD which = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                if (which != WAIT_OBJECT_0) {
                    CancelIoEx(pipe_, &ov);
                    DWORD discarded = 0;
                    GetOverlappedResult(pipe_, &ov, &discarded, TRUE);
                    return false;
                }
                if (!GetOverlappedResult(pipe_, &ov, &written, FALSE)) return false;
            }
            if (written == 0) return false;
            sent += written;
        }
        return true;
    }

    void cancel() override {
        cancelled_.store(true, std::memory_order_release);
        if (stop_event_ != nullptr) SetEvent(stop_event_);
    }

    bool valid() const override {
        return pipe_ != INVALID_HANDLE_VALUE && !cancelled_.load(std::memory_order_acquire);
    }

    std::string peer_description() const override {
        ULONG pid = 0;
        if (GetNamedPipeClientProcessId(pipe_, &pid)) return "pid " + std::to_string(pid);
        return "local";
    }

private:
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    HANDLE read_event_ = nullptr;
    HANDLE write_event_ = nullptr;
    HANDLE stop_event_ = nullptr;
    std::atomic<bool> cancelled_{false};
};

class PipeServer final : public ServerTransport {
public:
    PipeServer(std::string name, PipeSecurity security)
        : name_(std::move(name)), security_(std::move(security)) {
        stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        connect_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    ~PipeServer() override {
        stop();
        if (pending_ != INVALID_HANDLE_VALUE) CloseHandle(pending_);
        if (stop_event_ != nullptr) CloseHandle(stop_event_);
        if (connect_event_ != nullptr) CloseHandle(connect_event_);
    }

    bool create_instance(bool first, std::string& error) {
        DWORD open_mode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
        if (first) open_mode |= FILE_FLAG_FIRST_PIPE_INSTANCE;
        const std::wstring wname = widen(name_);
        pending_ = CreateNamedPipeW(
            wname.c_str(), open_mode,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES, kPipeBufferBytes, kPipeBufferBytes, 0,
            security_.attributes());
        if (pending_ == INVALID_HANDLE_VALUE) {
            error = "CreateNamedPipe failed: " + last_error_string(GetLastError());
            return false;
        }
        return true;
    }

    std::unique_ptr<Connection> accept(int timeout_ms) override {
        if (stopped_.load(std::memory_order_acquire)) return nullptr;
        if (pending_ == INVALID_HANDLE_VALUE) {
            std::string error;
            if (!create_instance(false, error)) return nullptr;
        }

        OVERLAPPED ov{};
        ov.hEvent = connect_event_;
        ResetEvent(connect_event_);

        bool connected = false;
        if (ConnectNamedPipe(pending_, &ov)) {
            connected = true;
        } else {
            const DWORD err = GetLastError();
            if (err == ERROR_PIPE_CONNECTED) {
                // The client connected between CreateNamedPipe and ConnectNamedPipe.
                connected = true;
            } else if (err == ERROR_IO_PENDING) {
                HANDLE waits[2] = {connect_event_, stop_event_};
                const DWORD which = WaitForMultipleObjects(
                    2, waits, FALSE, timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms));
                if (which == WAIT_OBJECT_0) {
                    DWORD discarded = 0;
                    connected = GetOverlappedResult(pending_, &ov, &discarded, FALSE) != FALSE;
                } else {
                    CancelIoEx(pending_, &ov);
                    DWORD discarded = 0;
                    GetOverlappedResult(pending_, &ov, &discarded, TRUE);
                    return nullptr;  // timeout or stop; the instance is reused next call
                }
            } else {
                CloseHandle(pending_);
                pending_ = INVALID_HANDLE_VALUE;
                return nullptr;
            }
        }
        if (!connected) return nullptr;

        HANDLE accepted = pending_;
        pending_ = INVALID_HANDLE_VALUE;
        return std::make_unique<PipeConnection>(accepted);
    }

    void stop() override {
        stopped_.store(true, std::memory_order_release);
        if (stop_event_ != nullptr) SetEvent(stop_event_);
    }

    std::string endpoint() const override { return name_; }

    static std::wstring widen(const std::string& s) {
        if (s.empty()) return std::wstring();
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                          nullptr, 0);
        std::wstring out(static_cast<std::size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
        return out;
    }

private:
    std::string name_;
    PipeSecurity security_;
    HANDLE pending_ = INVALID_HANDLE_VALUE;
    HANDLE stop_event_ = nullptr;
    HANDLE connect_event_ = nullptr;
    std::atomic<bool> stopped_{false};
};

}  // namespace

std::string default_endpoint(const std::string& name_override) {
    if (name_override.rfind("\\\\", 0) == 0) return name_override;  // already a full pipe path
    std::string name = name_override;
    if (name.empty()) {
        std::string sid;
        // The SID keeps two users on one machine (fast user switching, a shared PC) from
        // colliding on a single pipe name.
        name = current_user_sid(sid) ? "obsn.v1." + sid : "obsn.v1";
    }
    return "\\\\.\\pipe\\" + name;
}

std::unique_ptr<ServerTransport> create_server(const std::string& endpoint, std::string& error) {
    PipeSecurity security;
    if (!security.build(error)) return nullptr;
    auto server = std::make_unique<PipeServer>(endpoint, std::move(security));
    if (!server->create_instance(true, error)) {
        if (error.find("already exists") != std::string::npos ||
            GetLastError() == ERROR_ACCESS_DENIED) {
            error += " (another instance of the OBS script may already be running)";
        }
        return nullptr;
    }
    return server;
}

std::unique_ptr<Connection> connect_client(const std::string& endpoint, int timeout_ms,
                                           std::string& error) {
    const std::wstring wname = PipeServer::widen(endpoint);
    for (int attempt = 0; attempt < 2; ++attempt) {
        const HANDLE pipe = CreateFileW(wname.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_BYTE;
            SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
            return std::make_unique<PipeConnection>(pipe);
        }
        const DWORD err = GetLastError();
        if (err != ERROR_PIPE_BUSY) {
            // ERROR_FILE_NOT_FOUND is the ordinary "OBS is not running" case, not a fault.
            error = last_error_string(err);
            return nullptr;
        }
        // Every instance is in use; wait briefly for one to free up, then retry once.
        if (!WaitNamedPipeW(wname.c_str(), timeout_ms > 0 ? static_cast<DWORD>(timeout_ms) : 1000)) {
            error = "all pipe instances are busy";
            return nullptr;
        }
    }
    error = "could not acquire a pipe instance";
    return nullptr;
}

}  // namespace obsn

#endif  // _WIN32
