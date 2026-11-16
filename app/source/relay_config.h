#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "../../common/relay_profile.h"
#include "../../common/roomcircuit_paths.h"
#include "roomcircuit_storage.h"

#define RELAY_CONFIG_DIR rc_storage_path(roomcircuit_storage_current(), RC_LEGACY_DIR, RC_CURRENT_DIR)
#define RELAY_CONFIG_PATH rc_storage_path(roomcircuit_storage_current(), RC_LEGACY_PATH("config.ini"), RC_CURRENT_PATH("config.ini"))
#define RELAY_DEFAULT_PORT 11460
#define RELAY_IPV4_SIZE 16

typedef enum {
    RelayConfigNotSet,
    RelayConfigReady,
    RelayConfigInvalid,
    RelayConfigFileError,
} RelayConfigState;

typedef struct {
    char server[RELAY_IPV4_SIZE];
    uint16_t port;
    bool public_groups;
    char provider[33];
} RelayConfig;

bool relay_config_ipv4_valid(const char *server);
bool relay_config_parse_port(const char *text, uint16_t *port);
RelayConfigState relay_config_load(const char *path, RelayConfig *config);
bool relay_config_save_endpoint(const char *path, const char *server, uint16_t port);
bool relay_config_save_public(const char *path, bool public_groups);
bool relay_config_save_provider(const char *path, const char *provider);
