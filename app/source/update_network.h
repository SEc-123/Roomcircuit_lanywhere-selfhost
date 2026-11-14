#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

typedef enum {
    UpdateNetworkOk,
    UpdateNetworkCancelled,
    UpdateNetworkUnavailable,
    UpdateNetworkBusy,
    UpdateNetworkInvalid,
    UpdateNetworkIo
} UpdateNetworkResult;

typedef void (*UpdateNetworkProgress)(uint32_t received, uint32_t total, void *context);

UpdateNetworkResult update_network_manifest(const char *host, uint16_t port, uint8_t manifest[512],
                                            const atomic_bool *cancel);
UpdateNetworkResult update_network_download(const char *host, uint16_t port, const uint8_t hash[32],
                                            uint32_t size, const char *destination, const atomic_bool *cancel,
                                            UpdateNetworkProgress progress, void *context);
