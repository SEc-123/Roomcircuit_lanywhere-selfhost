#pragma once
#include "../../common/node_catalog.h"
#include "../../common/node_lobby.h"
#include "game_catalog.h"
#include "node_probe.h"
#include <stdbool.h>
#include <stdint.h>
#include <switch.h>

void nodes_ui_initialize(void);
void nodes_ui_shutdown(void);
void nodes_ui_network_changed(void);
const char *nodes_ui_name(uint32_t id);
const char *nodes_ui_provider(uint32_t id);
uint8_t nodes_ui_region_for(uint32_t id);
bool nodes_ui_valid(uint32_t id);
const char *nodes_ui_region(uint8_t region);
const char *nodes_ui_country(const char *country);
uint32_t nodes_ui_region_mask(uint8_t region);
bool nodes_ui_supported(Service *service, uint32_t *selected);
/* IPC42 reads a nonblocking snapshot and asks the module to refresh in background. */
bool nodes_ui_refresh_catalog(Service *service);
bool nodes_ui_pick(PadState *pad, Service *service, uint32_t current, uint32_t *selected);
bool nodes_ui_check_room(PadState *pad, uint32_t node_id, bool gateway, const NodeRoomLocator *locator);
bool nodes_ui_filters(PadState *pad, const GameCatalog *catalog, uint64_t *game_id, uint8_t *region);
