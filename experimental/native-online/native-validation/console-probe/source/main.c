// SPDX-License-Identifier: AGPL-3.0-only
// Based on OpenPak tools/cert-probe at bf8f220f26a04e36a19b384ebfb101bde5be333d.
// Reads public certificate metadata through documented libnx interfaces.
#include <switch.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>
#include "startup.h"

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

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}

static void hash_line(const void *data, size_t size) {
    u8 digest[32];
    sha256CalculateHash(digest, data, size);
    char hex[65];
    for (unsigned i = 0; i < sizeof(digest); ++i)
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    line("sha256=%s\n", hex);
}

static void inspect_store(void) {
    Result rc = romfsMountFromDataArchive(0x0100000000000800ULL, NcmStorageId_BuiltInSystem, "certstore");
    line("CertStore mount result=%08lX\n", (unsigned long)rc);
    if (R_FAILED(rc)) return;
    FILE *input = fopen("certstore:/ssl_TrustedCerts.bdf", "rb");
    uint8_t *data = NULL;
    if (!input) {
        line("Certificate store file unavailable\n");
        goto done;
    }
    if (fseek(input, 0, SEEK_END)) goto done;
    long length = ftell(input);
    if (length < 8 || length > 2 * 1024 * 1024 || fseek(input, 0, SEEK_SET)) {
        line("Certificate store length out of bounds\n");
        goto done;
    }
    data = malloc((size_t)length);
    if (!data || fread(data, 1, (size_t)length, input) != (size_t)length) {
        line("Certificate store read incomplete\n");
        goto done;
    }
    line("Certificate store bytes=%ld ", length);
    hash_line(data, (size_t)length);
    const uint32_t magic = get32(data), count = get32(data + 4);
    line("Certificate store magic=%08lX entries=%lu\n", (unsigned long)magic, (unsigned long)count);
    if (magic != 0x546c7373 || count > 512 || count > ((size_t)length - 8) / 16) {
        line("Unrecognized certificate store layout\n");
        goto done;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *entry = data + 8 + 16 * i;
        const uint32_t id = get32(entry), status = get32(entry + 4), size = get32(entry + 8), off = get32(entry + 12);
        if (off > (size_t)length - 8 || size > (size_t)length - 8 - off || (size && off < count * 16)) {
            line("Invalid entry bounds at %lu\n", (unsigned long)i);
            break;
        }
        line("Entry %lu id=%lu status=%lu bytes=%lu offset=%lu ", (unsigned long)i,
             (unsigned long)id, (unsigned long)status, (unsigned long)size, (unsigned long)off);
        hash_line(data + 8 + off, size);
    }
done:
    free(data);
    if (input) fclose(input);
    romfsUnmount("certstore");
}

static void inspect_active(u32 id) {
    u32 size = 0, total = 0;
    Result rc = sslGetCertificateBufSize(&id, 1, &size);
    line("SSL ID %lu size-result=%08lX bytes=%lu\n", (unsigned long)id, (unsigned long)rc, (unsigned long)size);
    if (R_FAILED(rc) || size < sizeof(SslBuiltInCertificateInfo) || size > 1024 * 1024) return;
    void *buffer = calloc(1, size);
    if (!buffer) { line("Allocation failed\n"); return; }
    rc = sslGetCertificates(buffer, size, &id, 1, &total);
    line("  read-result=%08lX entries=%lu\n", (unsigned long)rc, (unsigned long)total);
    if (R_SUCCEEDED(rc) && total == 1) {
        SslBuiltInCertificateInfo *info = buffer;
        uintptr_t start = (uintptr_t)buffer, data = (uintptr_t)info->cert_data;
        if (data >= start && data - start <= size && info->cert_size <= size - (data - start)) {
            line("  id=%lu status=%lu DER-bytes=%llu ", (unsigned long)info->cert_id,
                 (unsigned long)info->status, (unsigned long long)info->cert_size);
            hash_line(info->cert_data, info->cert_size);
        } else line("Invalid active certificate buffer bounds\n");
    }
    free(buffer);
}

static void inspect_browser_bundle(const char *path) {
    FILE *input = fopen(path, "rb");
    if (!input) {
        line("BrowserDll %s unavailable errno=%d\n", path, errno);
        return;
    }

    if (fseek(input, 0, SEEK_END)) goto incomplete;
    const long size = ftell(input);
    if (size < 1 || size > 2 * 1024 * 1024 || fseek(input, 0, SEEK_SET)) goto incomplete;
    uint8_t *data = malloc((size_t)size);
    if (!data) goto incomplete;
    const bool read_complete = fread(data, 1, (size_t)size, input) == (size_t)size;
    if (read_complete) {
        line("BrowserDll %s bytes=%ld ", path, size);
        hash_line(data, (size_t)size);
    }
    free(data);
    if (read_complete) {
        fclose(input);
        return;
    }

incomplete:
    fclose(input);
    line("BrowserDll %s unreadable within bounds\n", path);
}

static void inspect_browser(void) {
    const Result rc = romfsMountFromDataArchive(0x0100000000000803ULL, NcmStorageId_BuiltInSystem, "browserdll");
    line("BrowserDll mount result=%08lX\n", (unsigned long)rc);
    if (R_FAILED(rc)) return;

    static const char *const paths[] = {
        "browserdll:/browser/RootCaEtc.pem",
        "browserdll:/browser/RootCaSdkAdditional.pem",
        "browserdll:/0/browser/RootCaEtc.pem",
        "browserdll:/0/browser/RootCaSdkAdditional.pem",
        "browserdll:/openssl_peer/cacerts.pem",
        "browserdll:/nro/netfront/openssl_peer/cacerts.pem",
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) inspect_browser_bundle(paths[i]);
    romfsUnmount("browserdll");
}

static void inspect_local_profiles(void) {
    Result rc = accountInitialize(AccountServiceType_System);
    if (R_FAILED(rc)) {
        line("Local profile count unavailable result=%08lX\n", (unsigned long)rc);
        return;
    }
    s32 count = 0;
    rc = accountGetUserCount(&count);
    accountExit();
    if (R_SUCCEEDED(rc)) line("Local profile count=%ld\n", (long)count);
    else line("Local profile count unavailable result=%08lX\n", (unsigned long)rc);
}

int main(void) {
    if (!rcEmummcDisplayReady()) return 0;
    consoleInit(NULL);
    line("RoomCircuit System Probe\n");
    if (!rcEmummcSdReady()) {
        line("The confirmed emuMMC could not mount the SD card.\n");
        goto exit_screen;
    }
    if (mkdir("sdmc:/switch/roomcircuit-native", 0755) != 0 && errno != EEXIST) {
        line("Could not create report directory errno=%d. Use the screen result.\n", errno);
    } else {
        report = fopen("sdmc:/switch/roomcircuit-native/probe.txt", "w");
        if (!report) line("SD report unavailable errno=%d. Use the screen result.\n", errno);
    }
    line("emuMMC type=1 HOS=%lu.%lu.%lu\n",
        (unsigned long)HOSVER_MAJOR(hosversionGet()), (unsigned long)HOSVER_MINOR(hosversionGet()),
        (unsigned long)HOSVER_MICRO(hosversionGet()));
    Result rc = setsysInitialize();
    if (R_SUCCEEDED(rc)) {
        SetSysFirmwareVersion firmware;
        rc = setsysGetFirmwareVersion(&firmware);
        if (R_SUCCEEDED(rc)) line("Firmware display=%.*s\n", (int)sizeof(firmware.display_version), firmware.display_version);
        setsysExit();
    }
    line("Firmware query result=%08lX\n", (unsigned long)rc);
    inspect_store();
    inspect_browser();
    inspect_local_profiles();
    rc = sslInitialize(1);
    line("sslInitialize result=%08lX\n", (unsigned long)rc);
    if (R_SUCCEEDED(rc)) {
        const u32 ids[] = {1, 2, 1000, 1011, 1012, 1013, 1033};
        for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) inspect_active(ids[i]);
        sslExit();
    }
    if (report) {
        if (!report_error && fsync(fileno(report)) != 0) report_error = errno ? errno : EIO;
        if (fclose(report) == EOF && !report_error) report_error = errno ? errno : EIO;
        report = NULL;
        if (report_error) {
            line("SD report incomplete: write failed errno=%d. Use the screen result.\n", report_error);
        } else {
            line("Report saved: /switch/roomcircuit-native/probe.txt\n");
        }
    }
exit_screen:
    line("Press + to exit.\n");
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(NULL);
    }
    consoleExit(NULL);
    return 0;
}
