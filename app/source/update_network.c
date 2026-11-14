#include "update_network.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define UpdateFrameLimit (32768u + 5u)
#define UpdateTransferLimit (16u * 1024u * 1024u)
#define UpdateIoTimeoutMs 6000u
#define UpdateTransferTimeoutMs (10u * 60u * 1000u)

static uint64_t monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static bool cancelled(const atomic_bool *cancel)
{
    return cancel && atomic_load(cancel);
}

static UpdateNetworkResult wait_socket(int socket, short events, uint64_t deadline, const atomic_bool *cancel)
{
    while (!cancelled(cancel)) {
        const uint64_t now = monotonic_ms();
        if (now >= deadline) return UpdateNetworkUnavailable;
        const int remaining = (int)(deadline - now);
        struct pollfd item = {socket, events, 0};
        const int count = poll(&item, 1, remaining < 100 ? remaining : 100);
        if (count < 0) {
            if (errno == EINTR) continue;
            return UpdateNetworkUnavailable;
        }
        if (item.revents & events) return UpdateNetworkOk;
        if (item.revents & (POLLERR | POLLNVAL | POLLHUP)) return UpdateNetworkUnavailable;
    }
    return UpdateNetworkCancelled;
}

static UpdateNetworkResult open_connection(const char *host, uint16_t port, const atomic_bool *cancel, int *out)
{
    *out = -1;
    struct sockaddr_in endpoint = {0};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(port);
    if (!port || inet_pton(AF_INET, host, &endpoint.sin_addr) != 1) return UpdateNetworkInvalid;
    const int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) return UpdateNetworkUnavailable;
#ifdef SO_NOSIGPIPE
    const int one = 1;
    (void)setsockopt(socket_fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    const int flags = fcntl(socket_fd, F_GETFL, 0);
    if (flags < 0 || fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        close(socket_fd);
        return UpdateNetworkUnavailable;
    }
    UpdateNetworkResult result = UpdateNetworkOk;
    if (connect(socket_fd, (const struct sockaddr *)&endpoint, sizeof(endpoint)) != 0) {
        if (errno != EINPROGRESS) result = UpdateNetworkUnavailable;
        else result = wait_socket(socket_fd, POLLOUT, monotonic_ms() + UpdateIoTimeoutMs, cancel);
    }
    int connection_error = 0;
    socklen_t error_size = sizeof(connection_error);
    if (result == UpdateNetworkOk && (getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &connection_error, &error_size) != 0 || connection_error))
        result = UpdateNetworkUnavailable;
    if (result != UpdateNetworkOk) close(socket_fd);
    else *out = socket_fd;
    return result;
}

static UpdateNetworkResult transfer(int socket_fd, uint8_t *data, size_t size, bool writing,
                                    const atomic_bool *cancel, uint64_t deadline)
{
    size_t offset = 0;
    while (offset < size) {
        UpdateNetworkResult result = wait_socket(socket_fd, writing ? POLLOUT : POLLIN, deadline, cancel);
        if (result != UpdateNetworkOk) return result;
#ifdef MSG_NOSIGNAL
        const int send_flags = MSG_NOSIGNAL;
#else
        const int send_flags = 0;
#endif
        const ssize_t count = writing ? send(socket_fd, data + offset, size - offset, send_flags) :
                                        recv(socket_fd, data + offset, size - offset, 0);
        if (count > 0) offset += (size_t)count;
        else if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) return UpdateNetworkUnavailable;
    }
    return UpdateNetworkOk;
}

static UpdateNetworkResult exchange(int socket_fd, uint8_t command, const uint8_t *request, size_t request_size,
                                    uint8_t expected, uint8_t *response, size_t capacity, size_t *received,
                                    const atomic_bool *cancel)
{
    if (request_size > 36) return UpdateNetworkInvalid;
    uint8_t frame[39] = {(uint8_t)(request_size + 1), 0, command};
    if (request_size) memcpy(frame + 3, request, request_size);
    const uint64_t deadline = monotonic_ms() + UpdateIoTimeoutMs;
    UpdateNetworkResult result = transfer(socket_fd, frame, request_size + 3, true, cancel, deadline);
    if (result != UpdateNetworkOk) return result;
    uint8_t header[3];
    result = transfer(socket_fd, header, sizeof(header), false, cancel, deadline);
    if (result != UpdateNetworkOk) return result;
    const size_t frame_size = (size_t)header[0] | (size_t)header[1] << 8;
    if (!frame_size || frame_size > UpdateFrameLimit || frame_size - 1 > capacity) return UpdateNetworkInvalid;
    result = transfer(socket_fd, response, frame_size - 1, false, cancel, deadline);
    if (result != UpdateNetworkOk) return result;
    if (header[2] == 0x85 && frame_size >= 2)
        return response[0] == 7 ? UpdateNetworkBusy : UpdateNetworkUnavailable;
    if (header[2] != expected) return UpdateNetworkInvalid;
    *received = frame_size - 1;
    return UpdateNetworkOk;
}

UpdateNetworkResult update_network_manifest(const char *host, uint16_t port, uint8_t manifest[512],
                                            const atomic_bool *cancel)
{
    int socket_fd;
    UpdateNetworkResult result = open_connection(host, port, cancel, &socket_fd);
    if (result != UpdateNetworkOk) return result;
    size_t received = 0;
    result = exchange(socket_fd, 0x30, NULL, 0, 0xb0, manifest, 512, &received, cancel);
    close(socket_fd);
    if (result == UpdateNetworkOk && received != 512) return UpdateNetworkInvalid;
    return result;
}

UpdateNetworkResult update_network_download(const char *host, uint16_t port, const uint8_t hash[32],
                                            uint32_t size, const char *destination, const atomic_bool *cancel,
                                            UpdateNetworkProgress progress, void *context)
{
    if (!size || size > UpdateTransferLimit) return UpdateNetworkInvalid;
    struct stat previous;
    if (lstat(destination, &previous) == 0 && !S_ISREG(previous.st_mode)) return UpdateNetworkIo;
    int socket_fd;
    UpdateNetworkResult result = open_connection(host, port, cancel, &socket_fd);
    if (result != UpdateNetworkOk) return result;
    int flags = O_WRONLY | O_CREAT | O_TRUNC;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int file_fd = open(destination, flags, 0600);
    FILE *file = file_fd >= 0 ? fdopen(file_fd, "wb") : NULL;
    if (!file) {
        if (file_fd >= 0) close(file_fd);
        close(socket_fd);
        return UpdateNetworkIo;
    }
    uint8_t request[36], response[32768 + 4];
    memcpy(request, hash, 32);
    uint32_t offset = 0;
    const uint64_t deadline = monotonic_ms() + UpdateTransferTimeoutMs;
    while (offset < size && result == UpdateNetworkOk) {
        if (monotonic_ms() >= deadline) { result = UpdateNetworkUnavailable; break; }
        for (unsigned index = 0; index < 4; ++index) request[32 + index] = (uint8_t)(offset >> (index * 8));
        size_t received = 0;
        result = exchange(socket_fd, 0x31, request, sizeof(request), 0xb1, response, sizeof(response), &received, cancel);
        if (result != UpdateNetworkOk) break;
        const size_t expected = size - offset < 32768 ? size - offset : 32768;
        if (received != expected + 4 || memcmp(response, request + 32, 4)) { result = UpdateNetworkInvalid; break; }
        if (fwrite(response + 4, 1, expected, file) != expected) { result = UpdateNetworkIo; break; }
        offset += (uint32_t)expected;
        if (progress) progress(offset, size, context);
    }
    if (result == UpdateNetworkOk && (fflush(file) != 0 || fsync(fileno(file)) != 0)) result = UpdateNetworkIo;
    if (fclose(file) != 0 && result == UpdateNetworkOk) result = UpdateNetworkIo;
    close(socket_fd);
    if (result != UpdateNetworkOk) unlink(destination);
    return result;
}
