/* SPDX-License-Identifier: GPL-2.0-only */
#include <arpa/inet.h>
#include <enet/enet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* IPC, stdin/stdout only: operation, big-endian peer ID, big-endian size, payload. */
enum { READY = 1, CONNECT = 2, RECEIVE = 3, DISCONNECT = 4, SEND = 5, DROP = 6 };
enum { HEADER_SIZE = 9, MAX_PAYLOAD = 65535, MAX_PEERS = 128, MAX_QUEUED = 128 };

typedef struct {
    uint32_t id;
    ENetPeer *peer;
    unsigned queued;
} Connection;

static Connection connections[MAX_PEERS];
static uint32_t generation;

static uint32_t read_u32(const uint8_t *bytes) {
    uint32_t raw;
    memcpy(&raw, bytes, sizeof(raw));
    return ntohl(raw);
}

static void write_u32(uint8_t *bytes, uint32_t value) {
    uint32_t raw = htonl(value);
    memcpy(bytes, &raw, sizeof(raw));
}

static bool write_all(const void *bytes, size_t length) {
    const uint8_t *cursor = bytes;
    while (length) {
        ssize_t consumed = write(STDOUT_FILENO, cursor, length);
        if (consumed < 0 && errno == EINTR)
            continue;
        if (consumed <= 0)
            return false;
        cursor += consumed;
        length -= (size_t)consumed;
    }
    return true;
}

static bool emit(uint8_t type, uint32_t id, const void *payload, uint32_t length) {
    uint8_t header[HEADER_SIZE] = { type };
    write_u32(header + 1, id);
    write_u32(header + 5, length);
    return write_all(header, sizeof(header)) && write_all(payload, length);
}

static Connection *find_connection(uint32_t id) {
    if (!id)
        return NULL;
    for (size_t i = 0; i < MAX_PEERS; ++i)
        if (connections[i].id == id)
            return &connections[i];
    return NULL;
}

static void release_packet(ENetPacket *packet) {
    Connection *connection = packet->userData;
    if (connection->queued)
        --connection->queued;
}

static bool handle_command(const uint8_t *record, uint32_t length) {
    uint8_t type = record[0];
    Connection *connection = find_connection(read_u32(record + 1));
    if (!connection)
        return true;
    ENetPeer *peer = connection->peer;
    if (type == DROP && !length) {
        enet_peer_disconnect_later(peer, 0);
        return true;
    }
    if (type != SEND || !length)
        return false;
    if (peer->state != ENET_PEER_STATE_CONNECTED)
        return true;
    if (connection->queued >= MAX_QUEUED) {
        uint32_t id = connection->id;
        /* ENet drops all pending packets on reset, including their free callbacks. */
        enet_peer_disconnect_now(peer, 0);
        memset(connection, 0, sizeof(*connection));
        return emit(DISCONNECT, id, NULL, 0);
    }
    ENetPacket *packet = enet_packet_create(record + HEADER_SIZE, length, ENET_PACKET_FLAG_RELIABLE);
    if (!packet)
        return false;
    packet->userData = connection;
    packet->freeCallback = release_packet;
    ++connection->queued;
    if (enet_peer_send(peer, 0, packet) < 0)
        enet_packet_destroy(packet);
    return true;
}

static bool read_commands(void) {
    static uint8_t record[HEADER_SIZE + MAX_PAYLOAD];
    static size_t received;
    /* Leave CPU time for ENet acknowledgements even during heavy IPC traffic. */
    for (unsigned frame = 0; frame < 64; ++frame) {
        size_t required = HEADER_SIZE;
        uint32_t length = 0;
        if (received >= HEADER_SIZE) {
            length = read_u32(record + 5);
            if (length > MAX_PAYLOAD)
                return false;
            required += length;
        }
        if (received < required) {
            ssize_t count = read(STDIN_FILENO, record + received, required - received);
            if (count < 0)
                return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
            if (!count)
                return false;
            received += (size_t)count;
            if (received < required || (required == HEADER_SIZE && read_u32(record + 5)))
                continue;
        }
        if (!handle_command(record, length))
            return false;
        received = 0;
    }
    return true;
}

static bool handle_event(ENetHost *host, ENetEvent *event) {
    Connection *connection = &connections[event->peer - host->peers];
    switch (event->type) {
    case ENET_EVENT_TYPE_CONNECT: {
        /* A monotonic generation prevents a delayed command reaching a reused ENet peer slot. */
        if (!++generation)
            ++generation;
        *connection = (Connection){ .id = generation, .peer = event->peer };
        enet_peer_timeout(event->peer, 16, 5000, 15000);
        return emit(CONNECT, connection->id, &event->peer->address.host, sizeof(uint32_t));
    }
    case ENET_EVENT_TYPE_RECEIVE: {
        bool ok = true;
        if (connection->id && event->packet->dataLength && event->packet->dataLength <= MAX_PAYLOAD)
            ok = emit(RECEIVE, connection->id, event->packet->data, (uint32_t)event->packet->dataLength);
        enet_packet_destroy(event->packet);
        return ok;
    }
    case ENET_EVENT_TYPE_DISCONNECT: {
        uint32_t id = connection->id;
        memset(connection, 0, sizeof(*connection));
        return !id || emit(DISCONNECT, id, NULL, 0);
    }
    default:
        return true;
    }
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fputs("Usage: enet-host ipv4 port\n", stderr);
        return 2;
    }
    char *end;
    errno = 0;
    unsigned long port = strtoul(argv[2], &end, 10);
    ENetAddress address = { .port = (uint16_t)port };
    if (errno || end == argv[2] || *end || port > UINT16_MAX ||
        inet_pton(AF_INET, argv[1], &address.host) != 1 ||
        enet_initialize() < 0 || fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK) < 0)
        return 2;

    signal(SIGPIPE, SIG_IGN);
#ifdef ENET_TEST_CLIENT
    ENetHost *host = enet_host_create(NULL, 1, 1, 0, 0);
#else
    ENetHost *host = enet_host_create(&address, MAX_PEERS, 1, 0, 0);
#endif
    if (!host) {
        fputs("Could not bind ENet room listener\n", stderr);
        enet_deinitialize();
        return 1;
    }
#ifdef ENET_TEST_CLIENT
    if (!enet_host_connect(host, &address, 1, 0)) {
        enet_host_destroy(host);
        enet_deinitialize();
        return 1;
    }
#endif
    host->maximumPacketSize = MAX_PAYLOAD;
    host->maximumWaitingData = 256 * 1024;
    uint16_t actual_port = htons(host->address.port);
    bool running = emit(READY, 0, &actual_port, sizeof(actual_port));

    while (running) {
        if (!read_commands()) {
            /* Deliver already-queued room closure and disconnect notices before exiting. */
            enet_host_flush(host);
            break;
        }
        ENetEvent event;
        int status = enet_host_service(host, &event, 5);
        if (status < 0)
            break;
        for (unsigned i = 0; status > 0 && i < 64; ++i) {
            if (!handle_event(host, &event)) {
                running = false;
                break;
            }
            if (i != 63)
                status = enet_host_check_events(host, &event);
        }
        enet_host_flush(host);
    }

    enet_host_destroy(host);
    enet_deinitialize();
    return 0;
}
