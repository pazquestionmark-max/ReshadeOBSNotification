// SPDX-License-Identifier: MIT
// AF_UNIX transport. Present so the shared IPC logic is exercised end-to-end by the test suite
// on Linux; the shipping build on Windows uses transport_win32.cpp instead.
#if !defined(_WIN32)

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "obsn/transport.hpp"

namespace obsn {
namespace {

/// Wakes a blocked poll() from another thread without any race on the fd's lifetime.
class StopPipe {
public:
    StopPipe() {
        if (::pipe(fds_) != 0) {
            fds_[0] = fds_[1] = -1;
        } else {
            ::fcntl(fds_[0], F_SETFL, O_NONBLOCK);
            ::fcntl(fds_[1], F_SETFL, O_NONBLOCK);
        }
    }
    ~StopPipe() {
        if (fds_[0] >= 0) ::close(fds_[0]);
        if (fds_[1] >= 0) ::close(fds_[1]);
    }
    StopPipe(const StopPipe&) = delete;
    StopPipe& operator=(const StopPipe&) = delete;

    void signal() {
        if (fds_[1] >= 0) {
            const char b = 1;
            ssize_t ignored = ::write(fds_[1], &b, 1);
            (void)ignored;
        }
    }
    int read_fd() const { return fds_[0]; }

private:
    int fds_[2]{-1, -1};
};

class PosixConnection final : public Connection {
public:
    explicit PosixConnection(int fd) : fd_(fd) {}
    ~PosixConnection() override {
        if (fd_ >= 0) ::close(fd_);
    }

    int read(char* buffer, std::size_t size) override {
        while (true) {
            if (fd_ < 0 || cancelled_) return -1;
            struct pollfd fds[2];
            fds[0].fd = fd_;
            fds[0].events = POLLIN;
            fds[0].revents = 0;
            fds[1].fd = stop_.read_fd();
            fds[1].events = POLLIN;
            fds[1].revents = 0;
            const int rc = ::poll(fds, 2, -1);
            if (rc < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            if (cancelled_ || (fds[1].revents & POLLIN) != 0) return -1;
            if ((fds[0].revents & (POLLERR | POLLNVAL)) != 0) return -1;
            if ((fds[0].revents & (POLLIN | POLLHUP)) == 0) continue;

            const ssize_t n = ::recv(fd_, buffer, size, 0);
            if (n > 0) return static_cast<int>(n);
            if (n == 0) return 0;  // clean peer shutdown
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return -1;
        }
    }

    bool write_all(const char* data, std::size_t size) override {
        std::size_t sent = 0;
        while (sent < size) {
            if (fd_ < 0 || cancelled_) return false;
            // MSG_NOSIGNAL: a peer that vanished mid-write must surface as EPIPE, not as a
            // SIGPIPE that would take down the host process (OBS or the game).
            const ssize_t n = ::send(fd_, data + sent, size - sent,
#if defined(MSG_NOSIGNAL)
                                     MSG_NOSIGNAL
#else
                                     0
#endif
            );
            if (n > 0) {
                sent += static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0 && (errno == EINTR)) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                struct pollfd p;
                p.fd = fd_;
                p.events = POLLOUT;
                p.revents = 0;
                if (::poll(&p, 1, 1000) <= 0 && errno != EINTR) return false;
                continue;
            }
            return false;
        }
        return true;
    }

    void cancel() override {
        cancelled_ = true;
        stop_.signal();
        if (fd_ >= 0) ::shutdown(fd_, SHUT_RDWR);
    }

    bool valid() const override { return fd_ >= 0 && !cancelled_; }

private:
    int fd_ = -1;
    volatile bool cancelled_ = false;
    StopPipe stop_;
};

class PosixServer final : public ServerTransport {
public:
    PosixServer(int fd, std::string path) : fd_(fd), path_(std::move(path)) {}
    ~PosixServer() override {
        if (fd_ >= 0) ::close(fd_);
        if (!path_.empty()) ::unlink(path_.c_str());
    }

    std::unique_ptr<Connection> accept(int timeout_ms) override {
        while (true) {
            if (fd_ < 0 || stopped_) return nullptr;
            struct pollfd fds[2];
            fds[0].fd = fd_;
            fds[0].events = POLLIN;
            fds[0].revents = 0;
            fds[1].fd = stop_.read_fd();
            fds[1].events = POLLIN;
            fds[1].revents = 0;
            const int rc = ::poll(fds, 2, timeout_ms);
            if (rc == 0) return nullptr;
            if (rc < 0) {
                if (errno == EINTR) continue;
                return nullptr;
            }
            if (stopped_ || (fds[1].revents & POLLIN) != 0) return nullptr;
            if ((fds[0].revents & POLLIN) == 0) continue;
            const int c = ::accept(fd_, nullptr, nullptr);
            if (c < 0) {
                if (errno == EINTR || errno == EAGAIN) continue;
                return nullptr;
            }
            return std::make_unique<PosixConnection>(c);
        }
    }

    void stop() override {
        stopped_ = true;
        stop_.signal();
    }

    std::string endpoint() const override { return path_; }

private:
    int fd_ = -1;
    std::string path_;
    volatile bool stopped_ = false;
    StopPipe stop_;
};

}  // namespace

std::string default_endpoint(const std::string& name_override) {
    if (!name_override.empty() && name_override.front() == '/') return name_override;
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    const char* tmp = std::getenv("TMPDIR");
    std::string dir = runtime != nullptr ? runtime : (tmp != nullptr ? tmp : "/tmp");
    const std::string name = name_override.empty() ? std::string("obsn.v1") : name_override;
    return dir + "/" + name + ".sock";
}

std::unique_ptr<ServerTransport> create_server(const std::string& endpoint, std::string& error) {
    if (endpoint.size() >= sizeof(sockaddr_un::sun_path)) {
        error = "endpoint path is too long for an AF_UNIX socket";
        return nullptr;
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        error = std::string("socket() failed: ") + std::strerror(errno);
        return nullptr;
    }
    ::unlink(endpoint.c_str());  // a stale socket file from a crashed run must not block startup

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, endpoint.c_str(), endpoint.size());

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        error = std::string("bind() failed: ") + std::strerror(errno);
        ::close(fd);
        return nullptr;
    }
    // Owner-only, matching the Windows DACL: no other user may connect.
    ::chmod(endpoint.c_str(), S_IRUSR | S_IWUSR);
    if (::listen(fd, 8) != 0) {
        error = std::string("listen() failed: ") + std::strerror(errno);
        ::close(fd);
        ::unlink(endpoint.c_str());
        return nullptr;
    }
    return std::make_unique<PosixServer>(fd, endpoint);
}

std::unique_ptr<Connection> connect_client(const std::string& endpoint, int timeout_ms,
                                           std::string& error) {
    (void)timeout_ms;
    if (endpoint.size() >= sizeof(sockaddr_un::sun_path)) {
        error = "endpoint path is too long for an AF_UNIX socket";
        return nullptr;
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        error = std::string("socket() failed: ") + std::strerror(errno);
        return nullptr;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, endpoint.c_str(), endpoint.size());
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        error = std::string("connect() failed: ") + std::strerror(errno);
        ::close(fd);
        return nullptr;
    }
    return std::make_unique<PosixConnection>(fd);
}

}  // namespace obsn

#endif  // !_WIN32
