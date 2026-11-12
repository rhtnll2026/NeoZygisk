#include "socket_utils.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>

// Count C++ allocations as well as using sanitizers: LeakSanitizer cannot run in
// some sandboxed/ptraced environments, but repeated IPC must still not leak.
static std::atomic<size_t> outstanding_allocations{0};

void *operator new(size_t size) {
    if (void *ptr = std::malloc(size == 0 ? 1 : size)) {
        ++outstanding_allocations;
        return ptr;
    }
    throw std::bad_alloc();
}

void operator delete(void *ptr) noexcept {
    if (ptr) {
        --outstanding_allocations;
        std::free(ptr);
    }
}

void *operator new[](size_t size) { return ::operator new(size); }
void operator delete[](void *ptr) noexcept { ::operator delete(ptr); }
void operator delete(void *ptr, size_t) noexcept { ::operator delete(ptr); }
void operator delete[](void *ptr, size_t) noexcept { ::operator delete(ptr); }

namespace {

void check(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

class Fd {
public:
    explicit Fd(int value) : value_(value) {}
    ~Fd() {
        if (value_ >= 0) close(value_);
    }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
    operator int() const { return value_; }

private:
    int value_;
};

struct SocketPair {
    int fds[2];
    explicit SocketPair(int type = SOCK_STREAM) {
        check(socketpair(AF_UNIX, type, 0, fds) == 0, "socketpair failed");
    }
    ~SocketPair() {
        close(fds[0]);
        close(fds[1]);
    }
};

size_t fd_count() {
    DIR *dir = opendir("/proc/self/fd");
    check(dir != nullptr, "cannot inspect open descriptors");
    size_t count = 0;
    while (auto *entry = readdir(dir)) {
        if (entry->d_name[0] != '.') ++count;
    }
    closedir(dir);
    return count;
}

void send_rights(int socket, int fd, size_t count = 1, size_t payload_size = sizeof(int)) {
    check(count <= 3 && payload_size <= 8, "invalid test message");
    alignas(cmsghdr) char control[CMSG_SPACE(3 * sizeof(int))]{};
    char payload[8]{};
    iovec iov{payload, payload_size};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = CMSG_SPACE(count * sizeof(int));
    auto *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(count * sizeof(int));
    for (size_t i = 0; i < count; ++i) {
        std::memcpy(CMSG_DATA(cmsg) + i * sizeof(int), &fd, sizeof(fd));
    }
    check(sendmsg(socket, &msg, 0) == static_cast<ssize_t>(payload_size), "sendmsg failed");
}

void string_roundtrip() {
    for (const auto &expected : {std::string(), std::string("module-name"),
                                 std::string("a\0b", 3), std::string(12000, 'x')}) {
        SocketPair sockets;
        check(socket_utils::write_string(sockets.fds[0], expected), "write_string failed");
        check(socket_utils::read_string(sockets.fds[1]) == expected, "string did not round trip");
    }
}

void truncated_string() {
    SocketPair sockets;
    check(socket_utils::write_usize(sockets.fds[0], 8), "length write failed");
    check(socket_utils::xwrite(sockets.fds[0], "abc", 3) == 3, "payload write failed");
    shutdown(sockets.fds[0], SHUT_WR);
    check(socket_utils::read_string(sockets.fds[1]).empty(), "accepted a truncated string");
}

void truncated_length() {
    SocketPair sockets;
    size_t length = 8;
    check(write(sockets.fds[0], &length, sizeof(length) - 1) == sizeof(length) - 1,
          "length write failed");
    shutdown(sockets.fds[0], SHUT_WR);
    check(socket_utils::read_string(sockets.fds[1]).empty(), "accepted a truncated length");
}

void oversized_length() {
    SocketPair sockets;
    check(socket_utils::write_usize(sockets.fds[0], std::numeric_limits<size_t>::max()),
          "length write failed");
    shutdown(sockets.fds[0], SHUT_WR);
    check(socket_utils::read_string(sockets.fds[1]).empty(), "accepted an overflowing length");
}

void large_length_without_payload() {
    SocketPair sockets;
    check(socket_utils::write_usize(sockets.fds[0], 1ULL << 30), "length write failed");
    shutdown(sockets.fds[0], SHUT_WR);
    check(socket_utils::read_string(sockets.fds[1]).empty(), "accepted a missing large payload");
}

void fragmented_string() {
    SocketPair sockets;
    std::string expected(32000, 'x');
    check(socket_utils::write_usize(sockets.fds[0], expected.size()), "length write failed");
    std::thread sender([&] {
        for (size_t offset = 0; offset < expected.size();) {
            auto size = std::min(size_t{37}, expected.size() - offset);
            if (socket_utils::xwrite(sockets.fds[0], expected.data() + offset, size) != size) break;
            offset += size;
        }
        shutdown(sockets.fds[0], SHUT_WR);
    });
    auto actual = socket_utils::read_string(sockets.fds[1]);
    sender.join();
    check(actual == expected, "fragmented string did not round trip");
}

void repeated_strings() {
    SocketPair sockets;
    const std::string expected(100, 'm');
    const auto before = outstanding_allocations.load();
    for (int i = 0; i < 128; ++i) {
        check(socket_utils::write_string(sockets.fds[0], expected), "write_string failed");
        check(socket_utils::read_string(sockets.fds[1]) == expected, "read_string failed");
    }
    check(outstanding_allocations.load() == before, "reading strings leaked allocations");
}

void valid_fd() {
    SocketPair sockets;
    Fd original(open("/dev/null", O_RDONLY));
    check(original >= 0, "open failed");
    const auto before = fd_count();
    // Repeated frames must stay synchronized and ownership passes to the caller.
    for (int i = 0; i < 3; ++i) {
        send_rights(sockets.fds[0], original);
        Fd received(socket_utils::recv_fd(sockets.fds[1]));
        check(received >= 0 && received != original, "did not receive a new descriptor");
        struct stat source{}, destination{};
        check(fstat(original, &source) == 0 && fstat(received, &destination) == 0, "fstat failed");
        check(source.st_dev == destination.st_dev && source.st_ino == destination.st_ino,
              "descriptor points to the wrong file");
    }
    check(fd_count() == before, "valid descriptor transfer leaked");
}

void rejected_rights(size_t count, size_t payload_size = sizeof(int), int type = SOCK_STREAM) {
    SocketPair sockets(type);
    Fd original(open("/dev/null", O_RDONLY));
    check(original >= 0, "open failed");
    const auto before = fd_count();
    send_rights(sockets.fds[0], original, count, payload_size);
    shutdown(sockets.fds[0], SHUT_WR);
    int result;
    {
        Fd received(socket_utils::recv_fd(sockets.fds[1]));
        result = received;
    }
    check(fd_count() == before, "rejected message leaked descriptors");
    check(result == -1, "accepted an invalid descriptor message");
}

void short_fd_payload() { rejected_rights(1, 1); }
void extra_fds() { rejected_rights(2); }
void truncated_control() { rejected_rights(3); }
void truncated_datagram() { rejected_rights(1, 8, SOCK_DGRAM); }

void missing_fd() {
    SocketPair sockets;
    check(socket_utils::write_u32(sockets.fds[0], 0), "write failed");
    check(socket_utils::recv_fd(sockets.fds[1]) == -1, "accepted a message without descriptors");
}

void credentials_are_not_fds() {
    SocketPair sockets;
    int enabled = 1;
    check(setsockopt(sockets.fds[1], SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) == 0,
          "setsockopt failed");
    check(socket_utils::write_u32(sockets.fds[0], 0), "write failed");
    check(socket_utils::recv_fd(sockets.fds[1]) == -1, "interpreted credentials as descriptors");
}

void eof_fd() {
    SocketPair sockets;
    shutdown(sockets.fds[0], SHUT_WR);
    check(socket_utils::recv_fd(sockets.fds[1]) == -1, "accepted EOF as a descriptor");
}

void failed_recvmsg() {
    const auto before = fd_count();
    check(socket_utils::recv_fd(-1) == -1, "accepted a failed recvmsg");
    check(fd_count() == before, "failed recvmsg changed descriptors");
}

volatile sig_atomic_t signal_count = 0;
void handle_signal(int) { signal_count = 1; }

void interrupted_recvmsg() {
    SocketPair sockets;
    Fd original(open("/dev/null", O_RDONLY));
    check(original >= 0, "open failed");
    struct sigaction action{}, old_action{};
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    check(sigaction(SIGUSR1, &action, &old_action) == 0, "sigaction failed");
    signal_count = 0;
    const auto parent = getpid();
    const auto child = fork();
    check(child >= 0, "fork failed");
    if (child == 0) {
        usleep(50000);
        kill(parent, SIGUSR1);
        usleep(50000);
        send_rights(sockets.fds[0], original);
        _exit(0);
    }
    Fd received(socket_utils::recv_fd(sockets.fds[1]));
    int status = 0;
    while (waitpid(child, &status, 0) == -1 && errno == EINTR) {}
    sigaction(SIGUSR1, &old_action, nullptr);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "sender process failed");
    check(signal_count != 0, "signal was not delivered");
    check(received >= 0, "recvmsg did not recover from EINTR");
}

}  // namespace

int main(int argc, char **argv) {
    const std::pair<const char *, void (*)()> tests[] = {
        {"string_roundtrip", string_roundtrip},
        {"truncated_string", truncated_string},
        {"truncated_length", truncated_length},
        {"oversized_length", oversized_length},
        {"large_length_without_payload", large_length_without_payload},
        {"fragmented_string", fragmented_string},
        {"repeated_strings", repeated_strings},
        {"valid_fd", valid_fd},
        {"short_fd_payload", short_fd_payload},
        {"extra_fds", extra_fds},
        {"truncated_control", truncated_control},
        {"truncated_datagram", truncated_datagram},
        {"missing_fd", missing_fd},
        {"credentials_are_not_fds", credentials_are_not_fds},
        {"eof_fd", eof_fd},
        {"failed_recvmsg", failed_recvmsg},
        {"interrupted_recvmsg", interrupted_recvmsg},
    };
    int failures = 0;
    int selected = 0;
    for (const auto &[name, test] : tests) {
        if (argc > 1 && std::none_of(argv + 1, argv + argc, [&](const char *arg) {
                return std::strcmp(name, arg) == 0;
            })) continue;
        ++selected;
        try {
            test();
            std::printf("PASS %s\n", name);
        } catch (const std::exception &error) {
            std::fprintf(stderr, "FAIL %s: %s\n", name, error.what());
            ++failures;
        }
    }
    std::printf("%d/%d tests passed\n", selected - failures, selected);
    return selected == 0 || failures != 0;
}
