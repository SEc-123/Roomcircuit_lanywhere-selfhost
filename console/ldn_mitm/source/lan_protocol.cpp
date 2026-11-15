#include "lan_protocol.hpp"
#include "debug.hpp"
#include <poll.h>
#include <memory>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <cstring>
#include <unistd.h>
#include <array>
#include <cerrno>
#include <ctime>
#include <algorithm>
#include <stratosphere.hpp>

#define POLL_UNKNOWN (~(POLLIN | POLLPRI | POLLOUT))
// Only used when debuging
#define DISABLE_COMPRESS 0

int Pollable::Poll(Pollable *fds[], size_t nfds, int timeout) {
    if (nfds > 32) return -1;
    std::array<struct pollfd, 32> pfds{};
    for (size_t i = 0; i < nfds; i++) {
        pfds[i].fd = fds[i] ? fds[i]->getFd() : -1;
        pfds[i].events = POLLIN;
        pfds[i].revents = 0;
    }
    int rc = poll(pfds.data(), nfds, timeout);
    if (rc < 0) {
        LogFormat("Pollable::Poll failed %d", rc);
        return -1;
    }
    if (rc == 0) {
        return 0;
    }
    for (size_t i = 0; i < nfds; i++) {
        const struct pollfd &pfd = pfds[i];

        if (pfd.revents != 0) {
            if (pfd.revents & POLL_UNKNOWN) {
                LogFormat("Poll: %zu(%d) revents=0x%08X", i, pfd.fd, pfd.revents);
            }
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                LogFormat("Poll: (POLLERR | POLLHUP | POLLNVAL) %zu(%d) revents=0x%x", i, pfd.fd, pfd.revents);
                fds[i]->onClose();
            } else if (pfd.revents & (POLLIN | POLLPRI)) {
                int rc = fds[i]->onRead();
                if (rc != 0) {
                    LogFormat("Pollable::Poll close %d", rc);
                    fds[i]->onClose();
                }
            }
        }
    }

    return 0;
}

LanSocket::~LanSocket() {
    this->close();
}

void LanSocket::close() {
    if (this->fd != -1) {
        ::close(this->fd);
        this->fd = -1;
    }
}

void LanSocket::resetRecvSize() {
    this->recvSize = 0;
}

bool LanSocket::bufferedPacket() const {
    if (this->recvSize < sizeof(LANPacketHeader)) return false;
    LANPacketHeader header{};
    std::memcpy(&header, this->buffer, sizeof(header));
    return header.magic == LANMagic && header.length <= BufferSize - sizeof(header) &&
           this->recvSize >= sizeof(header) + header.length;
}

int LanSocket::recvPartPacket(u8 *output, size_t capacity, struct sockaddr_in *addr) {
    constexpr size_t HeaderSize = sizeof(LANPacketHeader);
    if (!this->isStream()) this->resetRecvSize();
    if (!this->bufferedPacket()) {
        const ssize_t count = this->recvfrom(this->buffer + this->recvSize, BufferSize - this->recvSize, addr);
        if (count <= 0) return count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
        this->recvSize += count;
    }
    if (this->recvSize < HeaderSize) {
        if (!this->isStream()) this->resetRecvSize();
        return 0;
    }
    LANPacketHeader header{};
    std::memcpy(&header, this->buffer, sizeof(header));
    const size_t total = HeaderSize + header.length;
    if (header.magic != LANMagic || total > capacity || total > BufferSize) {
        this->resetRecvSize();
        return -1;
    }
    if (this->recvSize < total) {
        if (!this->isStream()) this->resetRecvSize();
        return 0;
    }
    std::memcpy(output, this->buffer, total);
    this->recvSize -= total;
    std::memmove(this->buffer, this->buffer + total, this->recvSize);
    return static_cast<int>(total);
}

int LanSocket::recvPacket(MessageCallback callback) {
    constexpr size_t HeaderSize = sizeof(LANPacketHeader);
    u8 frame[BufferSize];
    u8 expanded[BufferSize];
    do {
        struct sockaddr_in address{};
        const int size = this->recvPartPacket(frame, sizeof(frame), &address);
        if (size <= 0) return size;
        LANPacketHeader header{};
        std::memcpy(&header, frame, sizeof(header));
        const u8 *body = frame + HeaderSize;
        size_t body_size = size - HeaderSize;
        if (header.compressed) {
            size_t output_size = sizeof(expanded);
            if (this->decompress(body, body_size, expanded, &output_size) != 0 ||
                output_size != header.decompress_length) return -1;
            body = expanded;
            body_size = output_size;
        }
        ReplyFunc reply = [&](LANPacketType type, const void *data, size_t count) {
            return this->sendPacket(type, data, count, &address);
        };
        const int result = callback(header.type, body, body_size, reply);
        if (result != 0) return result;
        // TCP may contain another complete frame already buffered, without
        // any more kernel bytes to wake the next poll call.
    } while (this->isStream() && this->bufferedPacket());
    return 0;
}
int LanSocket::sendPacket(LANPacketType type, const void *data, size_t size) {
    return this->sendPacket(type, data, size, nullptr);
}
int LanSocket::sendPacket(LANPacketType type, const void *data, size_t size, struct sockaddr_in *addr) {
    if (size > BufferSize - sizeof(LANPacketHeader) || (size && !data)) return -1;
    LANPacketHeader header{};
    this->prepareHeader(header, type);
    if (data == NULL) {
        size = 0;
    }
    header.length = size;
    u8 buf[BufferSize];
    if (size > 0) {
        u8 compressed[BufferSize];
        size_t outSize = size;
        if (this->compress(data, size, compressed, &outSize) == 0) {
            std::memcpy(buf + sizeof(header), compressed, outSize);
            header.decompress_length = header.length;
            header.length = outSize;
            header.compressed = true;
        } else {
            std::memcpy(buf + sizeof(header), data, size);
        }
    }
    std::memcpy(buf, &header, sizeof(header));

    return this->sendto(buf, header.length + sizeof(header), addr);
}

void LanSocket::prepareHeader(LANPacketHeader &header, LANPacketType type) {
    header.magic = LANMagic;
    header.type = type;
    header.compressed = false;
    header.length = 0;
    header.decompress_length = 0;
    header._reserved[0] = 0;
    header._reserved[1] = 0;
}

ssize_t TcpLanSocketBase::recvfrom(void *buf, size_t len, struct sockaddr_in *addr) {
	AMS_UNUSED(addr);
    auto rc = ::recvfrom(this->fd, buf, len, 0, nullptr, 0);
    if (rc == 0) {
        return -0xFD23;
    }
    return rc;
}
int TcpLanSocketBase::sendto(const void *buf, size_t len, struct sockaddr_in *addr) {
	AMS_UNUSED(addr);
    size_t sent = 0;
    const auto now = []() {
        timespec time{};
        clock_gettime(CLOCK_MONOTONIC, &time);
        return uint64_t(time.tv_sec) * 1000 + time.tv_nsec / 1000000;
    };
    const uint64_t deadline = now() + 500;
    while (sent < len && now() < deadline) {
        const ssize_t count = ::sendto(this->fd, static_cast<const u8 *>(buf) + sent, len - sent,
                                      MSG_DONTWAIT, nullptr, 0);
        if (count > 0) { sent += count; continue; }
        if (count == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) break;
        pollfd ready{this->fd, POLLOUT, 0};
        const uint64_t current = now();
        if (current >= deadline || ::poll(&ready, 1, static_cast<int>(deadline - current)) <= 0 ||
            !(ready.revents & POLLOUT)) break;
    }
    if (sent != len) {
        // A truncated frame cannot be resumed by a later protocol message.
        this->close();
        return -1;
    }
    return static_cast<int>(sent);
}

ssize_t UdpLanSocketBase::recvfrom(void *buf, size_t len, struct sockaddr_in *addr) {
    socklen_t addr_len = sizeof(*addr);
    return ::recvfrom(this->fd, buf, len, 0, (struct sockaddr *)addr, &addr_len);
}
int UdpLanSocketBase::sendto(const void *buf, size_t len, struct sockaddr_in *addr) {
    return ::sendto(this->fd, buf, len, 0, (struct sockaddr *)addr, sizeof(*addr));
}

int UdpLanSocketBase::sendBroadcast(LANPacketType type, const void *data, size_t size) {
    struct sockaddr_in addr{};

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(this->getBroadcast());
    addr.sin_port = htons(this->listenPort);

    return this->sendPacket(type, data, size, &addr);
}
int UdpLanSocketBase::sendBroadcast(LANPacketType type) {
    return this->sendBroadcast(type, nullptr, 0);
}

int LanSocket::compress(const void *input, size_t input_size, uint8_t *output, size_t *output_size) {
#if DISABLE_COMPRESS
    return -1;
#endif
    const uint8_t *in = (decltype(in))input;
    const uint8_t *in_end = in + input_size;
    uint8_t *out = output;
    uint8_t *out_end = output + *output_size;

    while (out < out_end && in < in_end) {
        uint8_t c = *in++;
        uint8_t count = 0;

        if (c == 0) {
            while (in < in_end && count < 0xFF && *in == 0) {
                count += 1;
                in++;
            }
        }

        if (c == 0x00) {
            *out++ = 0;

            if (out == out_end)
                return -1;
            *out++ = count;
        } else {
            *out++ = c;
        }
    }

    *output_size = out - output;

    return in == in_end ? 0 : -1;
}

int LanSocket::decompress(const void *input, size_t input_size, uint8_t *output, size_t *output_size) {
    const uint8_t *in = (decltype(in))input;
    const uint8_t *in_end = in + input_size;
    uint8_t *out = output;
    uint8_t *out_end = output + *output_size;

    while (in < in_end && out < out_end) {
        uint8_t c = *in++;

        *out++ = c;
        if (c == 0) {
            if (in == in_end) {
                return -1;
            }
            uint8_t count = *in++;
            if (static_cast<size_t>(out_end - out) < count) return -1;
            for (int i = 0; i < count; i++) {
                *out++ = c;
            }
        }
    }

    *output_size = out - output;

    return in == in_end ? 0 : -1;
}