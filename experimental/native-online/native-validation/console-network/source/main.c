// SPDX-License-Identifier: AGPL-3.0-only
#include <switch.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include "startup.h"

#include <rc_build_config.h>
#define PROBE_HOST RC_PROBE_HOST
#define PROBE_PATH "/native/compatibility"
#define REPORT_DIR "sdmc:/switch/roomcircuit-native"
#define REPORT_FILE REPORT_DIR "/network-probe.txt"
#define SSL_RESULT_MODULE 123
#define SSL_IO_WOULD_BLOCK MAKERESULT(SSL_RESULT_MODULE, 204)
/* SwitchBrew's SSL result table lists these distinct verification failures. */
#define SSL_INVALID_CERT_DOMAIN MAKERESULT(SSL_RESULT_MODULE, 301)
#define SSL_UNKNOWN_CA MAKERESULT(SSL_RESULT_MODULE, 308)

enum { ConnectTimeoutMs = 4500, HandshakeTimeoutMs = 7500, HttpTimeoutMs = 4500 };
extern const unsigned char _binary_probe_ca_der_start[], _binary_probe_ca_der_end[];

static FILE *report;
static int report_error;

static void line(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    if (report && !report_error) {
        va_start(args, format);
        const int written = vfprintf(report, format, args);
        va_end(args);
        if (written < 0 || fflush(report) == EOF) report_error = errno ? errno : EIO;
    }
    consoleUpdate(NULL);
}

static u64 elapsed_ms(void) {
    return armTicksToNs(armGetSystemTick()) / 1000000;
}

static int open_socket(void) {
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;
    struct sockaddr_in destination = {0};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(443);
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) goto failed;
    if (inet_pton(AF_INET, PROBE_HOST, &destination.sin_addr) != 1) {
        errno = EINVAL;
        goto failed;
    }
    if (connect(fd, (const struct sockaddr *)&destination, sizeof(destination)) == 0) return fd;
    if (errno != EINPROGRESS && errno != EWOULDBLOCK) goto failed;

    const u64 deadline = elapsed_ms() + ConnectTimeoutMs;
    for (;;) {
        const u64 now = elapsed_ms();
        if (now >= deadline) {
            errno = ETIMEDOUT;
            goto failed;
        }
        struct pollfd probe = {fd, POLLOUT, 0};
        const int rc = poll(&probe, 1, (int)(deadline - now));
        if (rc < 0 && errno == EINTR) continue;
        if (rc < 0) goto failed;
        if (!rc) {
            errno = ETIMEDOUT;
            goto failed;
        }
        int error = 0;
        socklen_t len = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0) goto failed;
        if (error || !(probe.revents & POLLOUT)) {
            errno = error ? error : EIO;
            goto failed;
        }
        return fd;
    }
failed: {
        const int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
}

static Result handshake(SslConnection *connection, bool *timed_out) {
    const u64 deadline = elapsed_ms() + HandshakeTimeoutMs;
    for (;;) {
        const Result rc = sslConnectionDoHandshake(connection, NULL, NULL, NULL, 0);
        if (rc != SSL_IO_WOULD_BLOCK) return rc;
        if (elapsed_ms() >= deadline) {
            *timed_out = true;
            return rc;
        }
        svcSleepThread(20000000LL);
    }
}

static bool check_http(SslConnection *connection) {
    static const char request[] = "GET " PROBE_PATH " HTTP/1.1\r\nHost: " PROBE_HOST
                                  "\r\nConnection: close\r\nAccept: application/json\r\n\r\n";
    const u64 deadline = elapsed_ms() + HttpTimeoutMs;
    u32 offset = 0;
    while (offset < sizeof(request) - 1 && elapsed_ms() < deadline) {
        u32 written = 0;
        const u32 remaining = (u32)(sizeof(request) - 1) - offset;
        const Result rc = sslConnectionWrite(connection, request + offset, remaining, &written);
        if (rc == SSL_IO_WOULD_BLOCK) {
            svcSleepThread(20000000LL);
            continue;
        }
        if (R_FAILED(rc) || !written || written > remaining) {
            line("HTTP write failed result=%08lX bytes=%lu\n", (unsigned long)rc, (unsigned long)written);
            return false;
        }
        offset += written;
    }
    if (offset != sizeof(request) - 1) {
        line("HTTP write timeout\n");
        return false;
    }

    char header[192];
    offset = 0;
    while (offset < sizeof(header) - 1 && elapsed_ms() < deadline) {
        u32 received = 0;
        const u32 remaining = (u32)(sizeof(header) - 1) - offset;
        const Result rc = sslConnectionRead(connection, header + offset, remaining, &received);
        if (rc == SSL_IO_WOULD_BLOCK) {
            svcSleepThread(20000000LL);
            continue;
        }
        if (R_FAILED(rc) || !received || received > remaining) {
            line("HTTP read failed result=%08lX bytes=%lu\n", (unsigned long)rc, (unsigned long)received);
            return false;
        }
        char *end = memchr(header + offset, '\n', received);
        if (end) {
            *end = '\0';
            unsigned status = 0;
            if (sscanf(header, "HTTP/%*u.%*u %3u", &status) != 1 || status < 100 || status > 599) {
                line("Invalid HTTP status line\n");
                return false;
            }
            line("HTTP status=%u\n", status);
            return status == 200;
        }
        offset += received;
    }
    line("HTTP status line unavailable within bounds\n");
    return false;
}

static bool run_probe(const char *name, const char *hostname, bool with_ca, Result expected_cert_error) {
    line("\n[%s]\n", name);
    SslContext context = {0};
    SslConnection connection = {0};
    int fd = -1, ssl_fd = -1;
    const char *stage = "SSL context";
    const size_t ca_size = (size_t)(_binary_probe_ca_der_end - _binary_probe_ca_der_start);
    bool passed = false;
    Result rc = sslCreateContext(&context, SslVersion_TlsV12 | SslVersion_TlsV13);
    if (R_FAILED(rc)) goto result_error;
    if (with_ca) {
        stage = "own root import";
        u64 root_id = 0;
        rc = sslContextImportServerPki(&context, _binary_probe_ca_der_start, (u32)ca_size,
                                       SslCertificateFormat_Der, &root_id);
        if (R_FAILED(rc)) goto result_error;
        line("Own CA imported into this context\n");
    }
    stage = "SSL connection";
    rc = sslContextCreateConnection(&context, &connection);
    if (R_FAILED(rc)) goto result_error;
    stage = "SSL socket ownership";
    rc = sslConnectionSetOption(&connection, SslOptionType_DoNotCloseSocket, true);
    if (R_FAILED(rc)) goto result_error;
    fd = open_socket();
    if (fd < 0) {
        line("TCP connection failed errno=%d\n", errno);
        goto cleanup;
    }
    line("TCP connected\n");
    ssl_fd = socketSslConnectionSetSocketDescriptor(&connection, fd);
    if (ssl_fd < 0 && errno != ENOENT) {
        line("SSL socket binding failed errno=%d result=%08lX\n", errno, (unsigned long)socketGetLastResult());
        goto cleanup;
    }
    stage = "SSL nonblocking mode";
    rc = sslConnectionSetIoMode(&connection, SslIoMode_NonBlocking);
    if (R_FAILED(rc)) goto result_error;
    stage = "SSL hostname";
    rc = sslConnectionSetHostName(&connection, hostname, (u32)strlen(hostname) + 1);
    if (R_FAILED(rc)) goto result_error;
    stage = "SSL verification flags";
    const u32 verify_flags = SslVerifyOption_PeerCa | SslVerifyOption_HostName | SslVerifyOption_DateCheck;
    rc = sslConnectionSetVerifyOption(&connection, verify_flags);
    if (R_FAILED(rc)) goto result_error;
    u32 active_flags = 0;
    rc = sslConnectionGetVerifyOption(&connection, &active_flags);
    if (R_FAILED(rc)) goto result_error;
    if ((active_flags & verify_flags) != verify_flags) {
        line("Required verification flags not active: %08lX\n", (unsigned long)active_flags);
        goto cleanup;
    }
    stage = "SSL session cache";
    rc = sslConnectionSetSessionCacheMode(&connection, SslSessionCacheMode_None);
    if (R_FAILED(rc)) goto result_error;

    bool timed_out = false;
    rc = handshake(&connection, &timed_out);
    if (R_FAILED(rc)) {
        const Result certificate = sslConnectionGetVerifyCertError(&connection);
        line("TLS handshake result=%08lX certificate-result=%08lX timeout=%d\n",
             (unsigned long)rc, (unsigned long)certificate, timed_out);
        if (expected_cert_error && !timed_out) {
            passed = certificate == expected_cert_error;
            if (!passed) line("TLS rejected; expected certificate-result=%08lX unconfirmed\n",
                              (unsigned long)expected_cert_error);
        }
    } else {
        line("TLS handshake verified\n");
        passed = !expected_cert_error && check_http(&connection);
    }
    goto cleanup;

result_error:
    line("%s result=%08lX\n", stage, (unsigned long)rc);

cleanup:
    if (ssl_fd >= 0 && ssl_fd != fd) close(ssl_fd);
    sslConnectionClose(&connection);
    sslContextClose(&context);
    if (fd >= 0) close(fd);
    line("%s %s\n", passed ? "PASS" : "FAIL / INCONCLUSIVE", name);
    return passed;
}

static void inspect_virtual_client_identity(void) {
    /* Query only the active emuMMC SSL service. No connection, certificate or key export. */
    SslContext context = {0};
    Result rc = sslCreateContext(&context, SslVersion_TlsV12);
    if (R_SUCCEEDED(rc)) {
        rc = sslContextRegisterInternalPki(&context, SslInternalPki_DeviceClientCertDefault, NULL);
    }
    line("Virtual SSL client identity registration result=%08lX\n", (unsigned long)rc);
    line("Registration alone does not prove device authentication. Nothing was transmitted.\n");
    sslContextClose(&context);
}

int main(void) {
    if (!rcEmummcDisplayReady()) return 0;
    consoleInit(NULL);
    line("RoomCircuit Native Network Probe\n");
    if (!rcEmummcSdReady()) {
        line("The confirmed emuMMC could not mount the SD card.\n");
        goto exit_screen;
    }
    if (mkdir(REPORT_DIR, 0755) && errno != EEXIST) {
        line("Could not open report directory errno=%d\n", errno);
        goto exit_screen;
    }
    report = fopen(REPORT_FILE, "w");
    if (!report) {
        line("Could not write network report errno=%d\n", errno);
        goto exit_screen;
    }
    line("HOS 19.0.1, emuMMC type 1\nEndpoint https://" PROBE_HOST PROBE_PATH "\n");
    const size_t ca_size = (size_t)(_binary_probe_ca_der_end - _binary_probe_ca_der_start);
    if (!ca_size || ca_size > 8192) {
        line("Own CA resource size invalid\n");
        goto report_end;
    }
    unsigned char digest[32];
    char hex[65];
    sha256CalculateHash(digest, _binary_probe_ca_der_start, ca_size);
    for (size_t i = 0; i < sizeof(digest); ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    line("Own CA DER SHA256=%s\n", hex);
    SocketInitConfig sockets = *socketGetDefaultInitConfig();
    sockets.tcp_tx_buf_size = 0x4000;
    sockets.tcp_rx_buf_size = 0x4000;
    sockets.tcp_tx_buf_max_size = 0;
    sockets.tcp_rx_buf_max_size = 0;
    sockets.num_bsd_sessions = 2;
    sockets.sb_efficiency = 2;
    Result rc = socketInitialize(&sockets);
    if (R_FAILED(rc)) {
        line("Socket initialization failed result=%08lX\n", (unsigned long)rc);
        goto report_end;
    }
    rc = sslInitialize(SslServiceType_Default);
    if (R_FAILED(rc)) {
        line("SSL initialization failed result=%08lX\n", (unsigned long)rc);
        socketExit();
        goto report_end;
    }
    const bool positive = run_probe("Expected hostname and own CA", PROBE_HOST, true, 0);
    const bool wrong_name = run_probe("Wrong hostname with own CA", "wrong.roomcircuit.invalid", true,
                                      SSL_INVALID_CERT_DOMAIN);
    const bool no_ca = run_probe("Expected hostname without own CA", PROBE_HOST, false, SSL_UNKNOWN_CA);
    line("\nNative TLS probe %s\n", positive && wrong_name && no_ca ? "PASSED" : "NOT VERIFIED");
    inspect_virtual_client_identity();
    sslExit();
    socketExit();
report_end:
    if (!report_error && fsync(fileno(report)) != 0) report_error = errno ? errno : EIO;
    if (fclose(report) == EOF && !report_error) report_error = errno ? errno : EIO;
    report = NULL;
    if (report_error) {
        line("SD report incomplete: write failed errno=%d. Use the screen result.\n", report_error);
    } else {
        line("Report saved: /switch/roomcircuit-native/network-probe.txt\n");
    }
exit_screen:
    line("Press + to exit.\n");
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(NULL);
    }
    consoleExit(NULL);
    return 0;
}
