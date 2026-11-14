#define _POSIX_C_SOURCE 200809L
#include "node_probe.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#ifdef __SWITCH__
#include <switch/runtime/resolver.h>
#endif

uint64_t node_probe_now_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

void node_probe_guard_poll(NodeProbeGuard *guard, bool cancel)
{
    if (!guard)
        return;
    const unsigned handle = atomic_load(&guard->handle);
    const uint64_t deadline = atomic_load(&guard->deadline);
    if (!handle || (!cancel && node_probe_now_ms() < deadline))
        return;
#ifdef __SWITCH__
    (void)resolverCancel(handle);
#else
    (void)deadline;
#endif
}

static bool wait_socket(int fd, short events, uint64_t deadline, const atomic_bool *cancel)
{
    while (!atomic_load(cancel)) {
        const uint64_t now = node_probe_now_ms();
        if (now >= deadline)
            return false;
        struct pollfd item = {fd, events, 0};
        const int remaining = (int)(deadline - now);
        const int ready = poll(&item, 1, remaining < 50 ? remaining : 50);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (item.revents & events)
            return true;
        if (item.revents & (POLLHUP | POLLERR | POLLNVAL))
            return false;
    }
    return false;
}

static bool transfer(int fd, unsigned char *buffer, size_t size, bool sending, uint64_t deadline,
                     const atomic_bool *cancel)
{
    size_t offset = 0;
    while (offset < size) {
        if (!wait_socket(fd, sending ? POLLOUT : POLLIN, deadline, cancel))
            return false;
        const ssize_t count = sending ? send(fd, buffer + offset, size - offset, 0)
                                      : recv(fd, buffer + offset, size - offset, 0);
        if (count > 0)
            offset += (size_t)count;
        else if (!count || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK))
            return false;
    }
    return true;
}

static bool usable_unicast(uint32_t network_ip)
{
    const uint32_t ip = ntohl(network_ip);
#ifdef NODE_PROBE_TEST_ENDPOINTS
    if ((ip >> 24) == 127)
        return true;
#endif
    return (ip >> 24) && (ip >> 24) < 224 && (ip >> 24) != 127 && (ip >> 24) != 10 && (ip >> 20) != 0xac1 &&
           (ip >> 16) != 0xc0a8 && (ip >> 16) != 0xa9fe && (ip >> 17) != 0x6309;
}

static int open_node(const char *host, uint16_t port, NodeProbeKind kind, uint64_t deadline,
                     const atomic_bool *cancel, NodeProbeGuard *guard)
{
    struct addrinfo hints = {0}, *addresses = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = kind == NodeProbeUdp ? SOCK_DGRAM : SOCK_STREAM;
    struct in_addr numeric;
    const bool numeric_host = inet_pton(AF_INET, host, &numeric) == 1;
    if (numeric_host)
        hints.ai_flags = AI_NUMERICHOST;
#ifdef __SWITCH__
    if (guard && !numeric_host) {
        const unsigned handle = resolverGetCancelHandle();
        if (!handle || atomic_load(cancel))
            return -1;
        atomic_store(&guard->deadline, deadline);
        atomic_store(&guard->handle, handle);
    }
#else
    (void)guard;
#endif
    const int resolved = getaddrinfo(host, NULL, &hints, &addresses);
    if (guard)
        atomic_store(&guard->handle, 0);
    if (resolved != 0)
        return -1;
    if (atomic_load(cancel) || node_probe_now_ms() >= deadline) {
        freeaddrinfo(addresses);
        return -1;
    }
    int fd = -1;
    for (struct addrinfo *item = addresses; item && fd < 0 && !atomic_load(cancel); item = item->ai_next) {
        if (item->ai_addrlen != sizeof(struct sockaddr_in))
            continue;
        struct sockaddr_in endpoint = *(struct sockaddr_in *)item->ai_addr;
        if (!usable_unicast(endpoint.sin_addr.s_addr))
            continue;
        endpoint.sin_port = htons(port);
        const int candidate = socket(AF_INET, hints.ai_socktype, 0);
        if (candidate < 0)
            continue;
        const int flags = fcntl(candidate, F_GETFL, 0);
        if (flags < 0 || fcntl(candidate, F_SETFL, flags | O_NONBLOCK) != 0) {
            close(candidate);
            continue;
        }
        const int connected = connect(candidate, (struct sockaddr *)&endpoint, sizeof(endpoint));
        if (connected != 0 && (errno != EINPROGRESS || !wait_socket(candidate, POLLOUT, deadline, cancel))) {
            close(candidate);
            continue;
        }
        int error = 0;
        socklen_t size = sizeof(error);
        if (getsockopt(candidate, SOL_SOCKET, SO_ERROR, &error, &size) != 0 || error) {
            close(candidate);
            continue;
        }
        fd = candidate;
    }
    freeaddrinfo(addresses);
    return fd;
}

static int compare_ms(const void *left, const void *right)
{
    const uint16_t a = *(const uint16_t *)left, b = *(const uint16_t *)right;
    return a < b ? -1 : a > b;
}

bool node_probe(const char *host, uint16_t port, NodeProbeKind kind, uint32_t node_id,
                const atomic_bool *cancel, NodeProbeGuard *guard, NodeMeasurement *result)
{
    if (!host || !*host || !port || !cancel || !result || (kind != NodeProbeTcp && kind != NodeProbeUdp))
        return false;
    *result = (NodeMeasurement){.node_id = node_id,
                                .median_ms = NODE_PROBE_UNKNOWN,
                                .jitter_ms = NODE_PROBE_UNKNOWN,
                                .kind = (uint8_t)kind,
                                .state = NodeMeasureMeasuring};
    uint16_t samples[NODE_PROBE_SAMPLES];
    const uint64_t overall = node_probe_now_ms() + 4000;
    const int fd = open_node(host, port, kind, overall, cancel, guard);
    if (fd < 0) {
        result->state = NodeMeasureFailed;
        result->measured_at_ms = node_probe_now_ms();
        return false;
    }
    const uint32_t seed = (uint32_t)node_probe_now_ms() ^ node_id * 0x9e3779b9u;
    uint64_t next_sample = 0;
    for (unsigned index = 0; index < NODE_PROBE_SAMPLES && !atomic_load(cancel); index++) {
        while (node_probe_now_ms() < next_sample && !atomic_load(cancel))
            poll(NULL, 0, 20);
        const uint64_t start = node_probe_now_ms();
        next_sample = start + 100;
        if (start >= overall)
            break;
        const uint64_t deadline = start + 700 < overall ? start + 700 : overall;
        result->sent++;
        bool ok = false;
        if (kind == NodeProbeTcp) {
            unsigned char request[3] = {1, 0, 5}, reply[3];
            ok = transfer(fd, request, sizeof(request), true, deadline, cancel) &&
                 transfer(fd, reply, sizeof(reply), false, deadline, cancel) && !memcmp(reply, "\1\0\206", 3);
            /* A timed-out ordered TCP stream cannot match later replies safely. */
            if (!ok)
                break;
        } else {
            const uint32_t nonce = seed + index * 0x01010101u;
            unsigned char request[5] = {2, (unsigned char)nonce, (unsigned char)(nonce >> 8),
                                        (unsigned char)(nonce >> 16), (unsigned char)(nonce >> 24)};
            if (send(fd, request, sizeof(request), 0) == sizeof(request)) {
                while (wait_socket(fd, POLLIN, deadline, cancel)) {
                    unsigned char reply[64];
                    const ssize_t length = recv(fd, reply, sizeof(reply), 0);
                    if ((length == 4 || length == 5) && !memcmp(reply, request, (size_t)length)) {
                        ok = true;
                        break;
                    }
                    if (length < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                        break;
                }
            }
        }
        if (ok) {
            const uint64_t elapsed = node_probe_now_ms() - start;
            samples[result->received++] = (uint16_t)(elapsed > 65534 ? 65534 : elapsed);
        }
    }
    close(fd);
    if (result->received) {
        qsort(samples, result->received, sizeof(samples[0]), compare_ms);
        result->median_ms = samples[result->received / 2];
        result->jitter_ms = samples[result->received - 1] - samples[0];
        result->state = NodeMeasureReady;
    } else
        result->state = NodeMeasureFailed;
    result->measured_at_ms = node_probe_now_ms();
    return result->received != 0;
}

bool node_measurement_recommended(const NodeMeasurement *result)
{
    return result && result->state == NodeMeasureReady && result->sent == NODE_PROBE_SAMPLES &&
           result->received >= 4;
}

bool node_measurement_weak(const NodeMeasurement *result)
{
    return !node_measurement_recommended(result) || result->median_ms >= 200 || result->jitter_ms >= 80;
}

int node_measurement_compare(const NodeMeasurement *a, const NodeMeasurement *b)
{
    const bool good_a = node_measurement_recommended(a), good_b = node_measurement_recommended(b);
    if (good_a != good_b)
        return good_a ? -1 : 1;
    if (a->received != b->received)
        return a->received > b->received ? -1 : 1;
    if (a->median_ms != b->median_ms)
        return a->median_ms < b->median_ms ? -1 : 1;
    if (a->jitter_ms != b->jitter_ms)
        return a->jitter_ms < b->jitter_ms ? -1 : 1;
    return a->node_id < b->node_id ? -1 : a->node_id > b->node_id;
}

bool node_protocol_exchange(const char *host, uint16_t port, uint8_t command, const void *request,
                            size_t request_size, uint8_t expected, void *response, size_t response_size,
                            const atomic_bool *cancel)
{
    if (request_size > NODE_FRIEND_REQUEST_SIZE || !response || !cancel)
        return false;
    const uint64_t deadline = node_probe_now_ms() + 4000;
    const int fd = open_node(host, port, NodeProbeTcp, deadline, cancel, NULL);
    if (fd < 0)
        return false;
    unsigned char frame[3 + NODE_FRIEND_REQUEST_SIZE] = {(unsigned char)(request_size + 1),
                                                         (unsigned char)((request_size + 1) >> 8), command};
    if (request_size)
        memcpy(frame + 3, request, request_size);
    unsigned char header[3];
    bool ok = transfer(fd, frame, request_size + 3, true, deadline, cancel) &&
              transfer(fd, header, sizeof(header), false, deadline, cancel);
    if (ok) {
        const size_t bytes = (size_t)header[0] | (size_t)header[1] << 8;
        ok = header[2] == expected && bytes == response_size + 1 &&
             transfer(fd, response, response_size, false, deadline, cancel);
    }
    close(fd);
    return ok;
}
