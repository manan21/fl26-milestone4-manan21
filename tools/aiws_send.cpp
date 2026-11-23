#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <fcntl.h>
#include <iostream>
#include <limits.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace {

using Deadline = std::chrono::steady_clock::time_point;

constexpr int kRequestTimeoutMs = 1000;

std::string default_socket() {
    const char* user = getenv("USER");
    if (user != nullptr && user[0] != '\0') {
        return "/tmp/aiws-" + std::string(user) + ".sock";
    }
    return "/tmp/aiws-" + std::to_string(geteuid()) + ".sock";
}

bool set_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
}

bool wait_for_event(int fd, short events, Deadline deadline) {
    while (true) {
        const auto remaining = std::chrono::duration_cast<
            std::chrono::milliseconds>(deadline -
                                       std::chrono::steady_clock::now())
                                   .count();
        if (remaining <= 0) {
            return false;
        }

        pollfd poll_fd{fd, events, 0};
        const int ready = poll(&poll_fd, 1, static_cast<int>(remaining));
        if (ready > 0) {
            return (poll_fd.revents & events) != 0;
        }
        if (ready == 0 || errno != EINTR) {
            return false;
        }
    }
}

bool send_all(int fd, const char* data, std::size_t length,
              Deadline deadline) {
    while (length > 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        const ssize_t sent = send(fd, data, length, MSG_NOSIGNAL);
        if (sent > 0) {
            data += sent;
            length -= static_cast<std::size_t>(sent);
        } else if (sent < 0 && (errno == EINTR)) {
            continue;
        } else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!wait_for_event(fd, POLLOUT, deadline)) {
                return false;
            }
        } else {
            return false;
        }
    }
    return true;
}

bool receive_exact(int fd, char* data, std::size_t length,
                   Deadline deadline) {
    while (length > 0) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        const ssize_t received = recv(fd, data, length, 0);
        if (received > 0) {
            data += received;
            length -= static_cast<std::size_t>(received);
        } else if (received == 0) {
            return false;
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (!wait_for_event(fd, POLLIN, deadline)) {
                return false;
            }
        } else {
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string socket_path = default_socket();
    if (argc >= 3 && std::string(argv[1]) == "--socket") {
        socket_path = argv[2];
        argc -= 2;
        argv += 2;
    }
    if (argc != 3 || std::string(argv[1]) != "add") {
        std::cerr << "usage: aiws_send [--socket PATH] add FILE\n";
        return 1;
    }

    char resolved_path[PATH_MAX];
    if (realpath(argv[2], resolved_path) == nullptr) {
        std::cerr << "unreadable file: " << argv[2] << "\n";
        return 1;
    }

    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return 2;
    if (!set_nonblocking(fd)) {
        close(fd);
        return 2;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(address.sun_path)) {
        close(fd);
        return 2;
    }
    std::memcpy(address.sun_path, socket_path.data(), socket_path.size());
    const Deadline deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(kRequestTimeoutMs);
    const int connected =
        connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    if (connected < 0 && errno != EINPROGRESS) {
        close(fd);
        return 2;
    }
    if (connected < 0) {
        if (!wait_for_event(fd, POLLOUT, deadline)) {
            close(fd);
            return 2;
        }
        int socket_error = 0;
        socklen_t socket_error_size = sizeof(socket_error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error,
                       &socket_error_size) < 0 ||
            socket_error != 0) {
            close(fd);
            return 2;
        }
    }

    std::string payload = std::string("ADD ") + resolved_path;
    std::uint32_t length = htonl(static_cast<std::uint32_t>(payload.size()));
    if (!send_all(fd, reinterpret_cast<const char*>(&length), 4, deadline) ||
        !send_all(fd, payload.data(), payload.size(), deadline)) {
        close(fd);
        return 2;
    }

    char response_header[2] = {};
    if (!receive_exact(fd, response_header, 2, deadline)) {
        close(fd);
        return 2;
    }
    close(fd);
    if (std::memcmp(response_header, "OK", 2) == 0) return 0;

    std::cerr << "application rejected request\n";
    return 1;
}
