#include "socket_utils.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstring>

#include "logging.hpp"

namespace socket_utils {

ssize_t xread(int fd, void* buf, size_t count) {
    size_t read_sz = 0;
    ssize_t ret;
    do {
        ret = read(fd, (std::byte*) buf + read_sz, count - read_sz);
        if (ret < 0) {
            if (errno == EINTR) continue;
            PLOGE("read");
            return ret;
        }
        read_sz += ret;
    } while (read_sz != count && ret != 0);
    if (read_sz != count) {
        PLOGE("read (%zu != %zu)", count, read_sz);
    }
    return read_sz;
}

size_t xwrite(int fd, const void* buf, size_t count) {
    size_t write_sz = 0;
    ssize_t ret;
    do {
        ret = write(fd, (std::byte*) buf + write_sz, count - write_sz);
        if (ret < 0) {
            if (errno == EINTR) continue;
            PLOGE("write");
            return write_sz;
        }
        write_sz += ret;
    } while (write_sz != count && ret != 0);
    if (write_sz != count) {
        PLOGE("write (%zu != %zu)", count, write_sz);
    }
    return write_sz;
}

template <typename T>
inline T read_exact_or(int fd, T fail) {
    T res;
    return sizeof(T) == xread(fd, &res, sizeof(T)) ? res : fail;
}

template <typename T>
inline bool write_exact(int fd, T val) {
    return sizeof(T) == xwrite(fd, &val, sizeof(T));
}

uint8_t read_u8(int fd) { return read_exact_or<uint8_t>(fd, 0); }

uint32_t read_u32(int fd) { return read_exact_or<uint32_t>(fd, 0); }

size_t read_usize(int fd) { return read_exact_or<size_t>(fd, 0); }

bool write_usize(int fd, size_t val) { return write_exact<size_t>(fd, val); }

std::string read_string(int fd) {
    auto len = read_usize(fd);
    std::string result;
    if (len > result.max_size()) {
        LOGE("read_string: invalid length %zu", len);
        return {};
    }

    // Grow only as data arrives, rather than allocating a peer-supplied length up front.
    // The string owns its storage on every exit path, including a truncated payload.
    char buf[4096];
    while (len != 0) {
        const auto chunk = std::min(len, sizeof(buf));
        if (xread(fd, buf, chunk) != static_cast<ssize_t>(chunk)) return {};
        result.append(buf, chunk);
        len -= chunk;
    }
    return result;
}

bool write_u8(int fd, uint8_t val) { return write_exact<uint8_t>(fd, val); }

bool write_u32(int fd, uint32_t val) { return write_exact<uint32_t>(fd, val); }

bool write_string(int fd, std::string_view str) {
    return write_usize(fd, str.size()) && str.size() == xwrite(fd, str.data(), str.size());
}

int recv_fd(int sockfd) {
    alignas(cmsghdr) char cmsgbuf[CMSG_SPACE(sizeof(int))]{};
    // Rust's passfd sends one int of payload alongside the descriptor.
    int dummy_data;
    iovec iov{&dummy_data, sizeof(dummy_data)};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsgbuf;

    ssize_t rec;
    do {
        msg.msg_controllen = sizeof(cmsgbuf);
        msg.msg_flags = 0;
        rec = recvmsg(sockfd, &msg, MSG_WAITALL);
    } while (rec < 0 && errno == EINTR);
    if (rec < 0) {
        PLOGE("recvmsg");
        return -1;
    }

    bool valid = rec == sizeof(dummy_data) && !(msg.msg_flags & (MSG_CTRUNC | MSG_TRUNC));
    int result = -1;
    size_t count = 0;
    for (auto* cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        const auto remaining = msg.msg_controllen -
                               static_cast<size_t>(reinterpret_cast<char*>(cmsg) - cmsgbuf);
        if (cmsg->cmsg_len < CMSG_LEN(0) || cmsg->cmsg_len > remaining) {
            valid = false;
            break;
        }
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
            valid = false;
            continue;
        }

        const auto size = cmsg->cmsg_len - CMSG_LEN(0);
        if (size % sizeof(int) != 0) valid = false;
        for (size_t offset = 0; offset + sizeof(int) <= size; offset += sizeof(int)) {
            int received;
            memcpy(&received, CMSG_DATA(cmsg) + offset, sizeof(received));
            if (count++ == 0) {
                result = received;
            } else {
                // Alignment padding can fit a second descriptor even in this buffer.
                close(received);
            }
        }
    }
    if (valid && count == 1) return result;

    // Even truncated messages can install descriptors. Release every one we received.
    if (result >= 0) close(result);
    LOGE("recv_fd: invalid message (bytes=%zd, flags=%d, descriptors=%zu)", rec, msg.msg_flags,
         count);
    return -1;
}
}  // namespace socket_utils
