#include "nodes_ui.h"
#include "../../common/node_lobby.h"
#include "app_socket.h"
#include "node_selection_trace.h"
#include "gfx.h"
#include "relay_config.h"
#include "ui_locale.h"
#include "ui_text.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NODE_ROWS 6
#define NODE_ALL_REGIONS 255u
#define NODE_COLOR_BG RGBA(0x00, 0x00, 0x00, 0xFF)
#define NODE_COLOR_PANEL RGBA(0x11, 0x11, 0x11, 0xFF)
#define NODE_COLOR_RAIL RGBA(0x08, 0x08, 0x08, 0xFF)
#define NODE_COLOR_TEXT RGBA(0xE8, 0xE8, 0xE8, 0xFF)
#define NODE_COLOR_DIM RGBA(0xAE, 0xAE, 0xAE, 0xFF)
#define NODE_COLOR_LINE RGBA(0x36, 0x36, 0x36, 0xFF)
#define NODE_COLOR_ACCENT RGBA(0xD8, 0xD8, 0xD8, 0xFF)
#define NODE_COLOR_SELECT RGBA(0x26, 0x26, 0x26, 0xFF)
#define NODE_COLOR_WARN RGBA(0xE5, 0xBA, 0x6E, 0xFF)
#define NODE_COLOR_FOOT RGBA(0x00, 0x00, 0x00, 0xFF)
#define NODE_PANEL_X 40
#define NODE_PANEL_Y 195
#define NODE_PANEL_W 1200
#define NODE_PANEL_H 412
#define NODE_TABLE_X 360
#define NODE_LIST_Y 275
#define NODE_ROW_HEIGHT 54
#define NODE_REGION_Y 244
#define NODE_REGION_HEIGHT 49
#define NODE_REGION_COUNT 7
#define NODE_NO_REGION_TOUCHED 254u
#define NODE_FOOTER_Y 650
#define NODE_FILTER_LIST_Y 291
#define NODE_FILTER_ROW_HEIGHT 50

typedef struct {
    NodeMeasurement measurements[NODE_CATALOG_COUNT];
    uint32_t enabled, available;
    bool catalog_ok, probe_start_failed, changed;
} NodeView;
static NodeView view;
static Mutex view_lock;
static Thread probe_threads[3], friend_thread;
static bool probe_created[3], friend_created, initialized, socket_ready;
static atomic_bool cancel_probe, cancel_friend, probe_done[3], friend_done;
static NodeProbeGuard resolver_guards[3];
static unsigned next_probe;
static uint32_t probe_mask;
static uint32_t network_address;
static Uuid network_uuid;
static bool nifm_ready, network_known, uuid_known;
static NodeFriendRequest friend_request;
static NodeFriendResponse friend_reply;
static bool friend_reply_ok;
static uint8_t participant[16];
static char group_code[8];
static NodeFriendResponse group;
static uint64_t next_friend_poll;
static const char *notice;
static bool language_save_failed;
static const char *keyboard_error;

static uint32_t read32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}
static NodeView snapshot(void)
{
    mutexLock(&view_lock);
    const NodeView copy = view;
    mutexUnlock(&view_lock);
    return copy;
}
const char *nodes_ui_name(uint32_t id)
{
    // The display label is independent of the server-verified catalog bytes.
    if (id == 0)
        return ui_text("Main server");
    if (id >= NODE_CATALOG_COUNT)
        return ui_text("Unknown node");
    return ui_chinese() ? LanywhereNodes[id].name_zh : LanywhereNodes[id].name_en;
}
const char *nodes_ui_region(uint8_t region)
{
    static const char *labels[] = {"Region unverified", "Asia", "Europe", "Americas", "Oceania", "Africa"};
    return ui_text(region < 6 ? labels[region] : "All regions");
}
const char *nodes_ui_country(const char *country)
{
    if (!strcmp(country, "CN"))
        return ui_text("China");
    if (!strcmp(country, "NL"))
        return ui_text("Netherlands");
    if (!strcmp(country, "DE"))
        return ui_text("Germany");
    if (!strcmp(country, "BR"))
        return ui_text("Brazil");
    if (!strcmp(country, "ID"))
        return ui_text("Indonesia");
    return country;
}
uint32_t nodes_ui_region_mask(uint8_t region)
{
    uint32_t mask = 0;
    for (unsigned i = 0; i < NODE_CATALOG_COUNT; i++)
        if (region == NODE_ALL_REGIONS || LanywhereNodes[i].region == region)
            mask |= 1u << i;
    return mask;
}
static bool fetch_catalog(uint32_t *enabled, uint32_t *available)
{
    uint8_t response[40];
    if (!node_protocol_exchange(RELAY_PROFILE_IPV4, RELAY_PROFILE_PORT, NODE_CATALOG_REQUEST, NULL, 0,
                                NODE_CATALOG_RESPONSE, response, sizeof(response), &cancel_probe) ||
        memcmp(response, LanywhereNodeCatalogHash, 32))
        return false;
    *enabled = read32(response + 32);
    *available = read32(response + 36);
    const uint32_t all = (1u << NODE_CATALOG_COUNT) - 1;
    return !((*enabled | *available) & ~all);
}
static void probe_worker(void *argument)
{
    const uintptr_t task = (uintptr_t)argument;
    const unsigned worker = (unsigned)(task >> 1);
    if (task & 1) {
        uint32_t enabled = 0, available = 0;
        const bool ok = fetch_catalog(&enabled, &available);
        mutexLock(&view_lock);
        view.catalog_ok = ok;
        view.enabled = enabled;
        view.available = available;
        view.changed = true;
        mutexUnlock(&view_lock);
    }
    while (!atomic_load(&cancel_probe)) {
        mutexLock(&view_lock);
        while (next_probe < NODE_CATALOG_COUNT && !(probe_mask & (1u << next_probe)))
            next_probe++;
        const unsigned id = next_probe++;
        if (id < NODE_CATALOG_COUNT) {
            view.measurements[id].state = NodeMeasureMeasuring;
            view.changed = true;
        }
        mutexUnlock(&view_lock);
        if (id >= NODE_CATALOG_COUNT)
            break;
        NodeMeasurement result;
        const LanywhereNode *node = &LanywhereNodes[id];
        const char *host = id == 0 ? RELAY_PROFILE_IPV4 : id == 1 ? RELAY_PROFILE_COMMUNITY_IPV4 : node->host;
        const uint16_t port = id == 0   ? RELAY_PROFILE_PORT
                              : id == 1 ? RELAY_PROFILE_COMMUNITY_PORT
                                        : node->port;
        node_probe(host, port, node->protocol == NodeProtocolLanPlay ? NodeProbeUdp : NodeProbeTcp, id,
                   &cancel_probe, &resolver_guards[worker], &result);
        if (atomic_load(&cancel_probe))
            result.measured_at_ms = 0;
        mutexLock(&view_lock);
        view.measurements[id] = result;
        view.changed = true;
        mutexUnlock(&view_lock);
    }
    atomic_store(&probe_done[worker], true);
}
static void cancel_probes(void)
{
    atomic_store(&cancel_probe, true);
    bool pending;
    do {
        pending = false;
        for (unsigned i = 0; i < 3; i++) {
            if (probe_created[i] && !atomic_load(&probe_done[i])) {
                node_probe_guard_poll(&resolver_guards[i], true);
                pending = true;
            }
        }
        if (pending)
            svcSleepThread(20000000ULL);
    } while (pending);
    for (unsigned i = 0; i < 3; i++)
        if (probe_created[i]) {
            threadWaitForExit(&probe_threads[i]);
            threadClose(&probe_threads[i]);
            probe_created[i] = false;
        }
}
static void collect_probes(void)
{
    for (unsigned i = 0; i < 3; i++) {
        node_probe_guard_poll(&resolver_guards[i], atomic_load(&cancel_probe));
        if (probe_created[i] && atomic_load(&probe_done[i])) {
            threadWaitForExit(&probe_threads[i]);
            threadClose(&probe_threads[i]);
            probe_created[i] = false;
        }
    }
}
static bool network_changed(void)
{
    uint32_t address = 0;
    NifmNetworkProfileData profile = {0};
    const bool known = nifm_ready && R_SUCCEEDED(nifmGetCurrentIpAddress(&address));
    const bool have_uuid = known && R_SUCCEEDED(nifmGetCurrentNetworkProfile(&profile));
    const bool changed = known != network_known || address != network_address || have_uuid != uuid_known ||
                         (have_uuid && memcmp(&profile.uuid, &network_uuid, sizeof(network_uuid)));
    network_known = known;
    network_address = address;
    uuid_known = have_uuid;
    if (have_uuid)
        network_uuid = profile.uuid;
    memset(&profile, 0, sizeof(profile));
    return changed;
}
static bool ensure_socket(void)
{
    if (!socket_ready)
        socket_ready = app_socket_acquire();
    return socket_ready;
}
static void start_probes(uint32_t mask, bool force)
{
    cancel_probes();
    atomic_store(&cancel_probe, false);
    if (network_changed()) {
        nodes_ui_network_changed();
        force = true;
    }
    const bool network_ready = ensure_socket();
    const uint64_t now = node_probe_now_ms();
    mutexLock(&view_lock);
    probe_mask = 0;
    next_probe = 0;
    for (unsigned i = 0; i < NODE_CATALOG_COUNT; i++) {
        if (!(mask & (1u << i)))
            continue;
        const NodeMeasurement *m = &view.measurements[i];
        if (force || !m->measured_at_ms || now - m->measured_at_ms >= 60000)
            probe_mask |= 1u << i;
    }
    // Keep workers from publishing results until their queued states are set.
    bool started = false;
    for (unsigned i = 0; network_ready && i < 3; i++) {
        atomic_store(&probe_done[i], false);
        // The first worker that starts fetches the catalog, even if an earlier slot failed.
        const uintptr_t task = ((uintptr_t)i << 1) | (started ? 0u : 1u);
        if (R_FAILED(
                threadCreate(&probe_threads[i], probe_worker, (void *)task, NULL, 0x10000, 0x2c, -2)))
            continue;
        probe_created[i] = true;
        if (R_FAILED(threadStart(&probe_threads[i]))) {
            threadClose(&probe_threads[i]);
            probe_created[i] = false;
        } else
            started = true;
    }
    for (unsigned i = 0; i < NODE_CATALOG_COUNT; i++) {
        if (!(probe_mask & (1u << i)))
            continue;
        NodeMeasurement *m = &view.measurements[i];
        if (started)
            m->state = NodeMeasurePending;
        else if (!m->measured_at_ms || now - m->measured_at_ms >= 60000 ||
                 m->state == NodeMeasurePending || m->state == NodeMeasureMeasuring)
            *m = (NodeMeasurement){.node_id = i, .median_ms = NODE_PROBE_UNKNOWN,
                                   .jitter_ms = NODE_PROBE_UNKNOWN, .state = NodeMeasureFailed};
    }
    view.probe_start_failed = !started;
    view.changed = true;
    mutexUnlock(&view_lock);
}
typedef struct {
    uint64_t last_frame_ms, next_check_ms;
    bool had_focus;
} NodeProbeLoop;
static NodeProbeLoop probe_loop_start(void)
{
    return (NodeProbeLoop){.last_frame_ms = node_probe_now_ms(),
                           .had_focus = appletGetFocusState() == AppletFocusState_InFocus};
}
static void maintain_probes(NodeProbeLoop *loop, uint32_t mask)
{
    const uint64_t now = node_probe_now_ms();
    const bool focused = appletGetFocusState() == AppletFocusState_InFocus;
    const bool resumed = focused && (!loop->had_focus || now - loop->last_frame_ms > 5000);
    loop->last_frame_ms = now;
    loop->had_focus = focused;
    if (!focused || (!resumed && now < loop->next_check_ms))
        return;
    loop->next_check_ms = now + 1000;
    if (resumed || network_changed()) {
        nodes_ui_network_changed();
        start_probes(mask, true);
        return;
    }
    if (probe_created[0] || probe_created[1] || probe_created[2])
        return;
    const NodeView previous = snapshot();
    for (unsigned i = 0; i < NODE_CATALOG_COUNT; i++) {
        if ((mask & (1u << i)) && previous.measurements[i].measured_at_ms &&
            now - previous.measurements[i].measured_at_ms >= 60000) {
            start_probes(mask, false);
            return;
        }
    }
}
void nodes_ui_network_changed(void)
{
    if (!initialized)
        return;
    mutexLock(&view_lock);
    for (unsigned i = 0; i < NODE_CATALOG_COUNT; i++)
        view.measurements[i].measured_at_ms = 0;
    mutexUnlock(&view_lock);
}
void nodes_ui_initialize(void)
{
    if (initialized)
        return;
    mutexInit(&view_lock);
    initialized = true;
    for (unsigned i = 0; i < NODE_CATALOG_COUNT; i++)
        view.measurements[i] =
            (NodeMeasurement){.node_id = i, .median_ms = NODE_PROBE_UNKNOWN, .jitter_ms = NODE_PROBE_UNKNOWN};
    (void)ensure_socket();
    nifm_ready = R_SUCCEEDED(nifmInitialize(NifmServiceType_User));
    do {
        randomGet(participant, sizeof(participant));
    } while (!memcmp(participant, (uint8_t[16]){0}, 16));
}
bool nodes_ui_supported(Service *service, uint32_t *selected)
{
    return RELAY_PROFILE_FIXED && R_SUCCEEDED(serviceDispatchOut(service, 35, *selected)) &&
           *selected < NODE_CATALOG_COUNT;
}
static void friend_worker(void *unused)
{
    (void)unused;
    NodeFriendRequest request;
    mutexLock(&view_lock);
    request = friend_request;
    mutexUnlock(&view_lock);
    NodeFriendResponse reply = {0};
    const bool ok =
        node_protocol_exchange(RELAY_PROFILE_IPV4, RELAY_PROFILE_PORT, NODE_FRIEND_REQUEST, &request,
                               sizeof(request), NODE_FRIEND_RESPONSE, &reply, sizeof(reply), &cancel_friend);
    bool valid = ok && reply.status <= NodeFriendBusy;
    if (valid && reply.status == NodeFriendOk) {
        valid = reply.code[6] == 0 && reply.code[7] == 0 && reply.participants <= NODE_FRIEND_MAX_PEOPLE &&
                reply.reported_participants <= reply.participants && reply.remaining_seconds <= 600 &&
                !memcmp(reply.participant, participant, 16);
        for (unsigned i = 0; i < NODE_CATALOG_COUNT; i++)
            if (reply.nodes[i].node_id != i || reply.nodes[i].recommendation_rank > 3 ||
                reply.nodes[i].reserved)
                valid = false;
    }
    mutexLock(&view_lock);
    friend_reply = reply;
    friend_reply_ok = valid;
    mutexUnlock(&view_lock);
    atomic_store(&friend_done, true);
}
static bool queue_friend(uint32_t operation, const char *code)
{
    if (friend_created)
        return false;
    if (!ensure_socket()) {
        notice = "Shared test is unavailable. Try again.";
        return false;
    }
    NodeFriendRequest request = {.operation = operation};
    memcpy(request.participant, participant, 16);
    if (code)
        snprintf(request.code, sizeof(request.code), "%s", code);
    if (operation == NodeFriendReport) {
        const NodeView copy = snapshot();
        request.count = 0;
        const uint64_t now = node_probe_now_ms();
        for (unsigned i = 0; i < NODE_CATALOG_COUNT; i++) {
            const NodeMeasurement *m = &copy.measurements[i];
            if (!m->measured_at_ms || now - m->measured_at_ms >= 60000 ||
                (m->state != NodeMeasureReady && m->state != NodeMeasureFailed))
                continue;
            NodeProbeSummary *out = &request.samples[request.count++];
            out->node_id = i;
            out->median_ms = m->median_ms;
            out->jitter_ms = m->jitter_ms;
            out->sent = m->sent;
            out->received = m->received;
            out->kind = m->kind;
            out->status = m->state == NodeMeasureReady    ? NodeProbeMeasured
                          : m->state == NodeMeasureFailed ? NodeProbeUnreachable
                                                          : NodeProbePending;
            out->age_seconds = m->measured_at_ms ? (uint16_t)((now - m->measured_at_ms) / 1000 > 65535
                                                                  ? 65535
                                                                  : (now - m->measured_at_ms) / 1000)
                                                 : 65535;
        }
    }
    mutexLock(&view_lock);
    friend_request = request;
    mutexUnlock(&view_lock);
    atomic_store(&cancel_friend, false);
    atomic_store(&friend_done, false);
    if (R_FAILED(threadCreate(&friend_thread, friend_worker, NULL, NULL, 0x10000, 0x2c, -2)))
        return false;
    friend_created = true;
    if (R_FAILED(threadStart(&friend_thread))) {
        threadClose(&friend_thread);
        friend_created = false;
        return false;
    }
    return true;
}
static bool collect_friend(void)
{
    if (!friend_created || !atomic_load(&friend_done))
        return false;
    threadWaitForExit(&friend_thread);
    threadClose(&friend_thread);
    friend_created = false;
    mutexLock(&view_lock);
    const NodeFriendResponse reply = friend_reply;
    const bool ok = friend_reply_ok;
    mutexUnlock(&view_lock);
    if (friend_request.operation == NodeFriendLeave) {
        group_code[0] = 0;
        memset(&group, 0, sizeof(group));
        return true;
    }
    if (!ok) {
        notice = "Could not refresh the shared test.";
        return true;
    }
    if (reply.status != NodeFriendOk) {
        notice = reply.status == NodeFriendExpired ? "Shared test expired. Create or join again."
                                                   : "Shared test is unavailable. Try again.";
        if (reply.status == NodeFriendExpired) {
            group_code[0] = 0;
            memset(&group, 0, sizeof(group));
        }
        return true;
    }
    group = reply;
    memcpy(group_code, reply.code, sizeof(group_code));
    notice = NULL;
    return true;
}
void nodes_ui_shutdown(void)
{
    if (!initialized)
        return;
    cancel_probes();
    atomic_store(&cancel_friend, true);
    if (friend_created) {
        threadWaitForExit(&friend_thread);
        threadClose(&friend_thread);
        friend_created = false;
    }
    if (socket_ready && group_code[0]) {
        NodeFriendRequest leave = {.operation = NodeFriendLeave};
        NodeFriendResponse ignored;
        memcpy(leave.code, group_code, sizeof(leave.code));
        memcpy(leave.participant, participant, 16);
        atomic_store(&cancel_friend, false);
        (void)node_protocol_exchange(RELAY_PROFILE_IPV4, RELAY_PROFILE_PORT, NODE_FRIEND_REQUEST, &leave,
                                     sizeof(leave), NODE_FRIEND_RESPONSE, &ignored, sizeof(ignored),
                                     &cancel_friend);
        group_code[0] = 0;
    }
    if (nifm_ready)
        nifmExit();
    if (socket_ready)
        app_socket_release();
    initialized = socket_ready = nifm_ready = false;
}
static void language_input(u64 down)
{
    if (!(down & HidNpadButton_StickR))
        return;
    const bool chinese = !ui_chinese();
    ui_set_language(chinese, false);
    language_save_failed = !ui_locale_save(UI_LOCALE_PATH, chinese);
}
static bool ask_friend_code(char code[8])
{
    keyboard_error = NULL;
    SwkbdConfig keyboard;
    if (R_FAILED(swkbdCreate(&keyboard, 0))) {
        keyboard_error = "Could not open the keyboard. Try again.";
        return false;
    }
    swkbdConfigMakePresetDefault(&keyboard);
    swkbdConfigSetHeaderText(&keyboard, ui_text("Shared test code"));
    swkbdConfigSetStringLenMin(&keyboard, 6);
    swkbdConfigSetStringLenMax(&keyboard, 6);
    const Result result = swkbdShow(&keyboard, code, 8);
    swkbdClose(&keyboard);
    if (R_FAILED(result) && result != MAKERESULT(Module_Libnx, LibnxError_LibAppletBadExit))
        keyboard_error = "Could not open the keyboard. Try again.";
    for (char *p = code; *p; p++)
        if (*p >= 'a' && *p <= 'z')
            *p -= 'a' - 'A';
    return R_SUCCEEDED(result) && strlen(code) == 6;
}
static void rank_nodes(const NodeView *copy, uint8_t region, uint32_t order[NODE_CATALOG_COUNT],
                       unsigned *count, bool recommended[NODE_CATALOG_COUNT])
{
    *count = 0;
    memset(recommended, 0, NODE_CATALOG_COUNT * sizeof(*recommended));
    for (unsigned i = 0; i < NODE_CATALOG_COUNT; i++)
        if (region == NODE_ALL_REGIONS || LanywhereNodes[i].region == region)
            order[(*count)++] = i;
    for (unsigned i = 1; i < *count; i++) {
        const uint32_t id = order[i];
        unsigned j = i;
        while (j) {
            const uint32_t previous = order[j - 1];
            const bool enabled = (copy->enabled & (1u << id)) != 0,
                       previous_enabled = (copy->enabled & (1u << previous)) != 0;
            bool first =
                enabled != previous_enabled
                    ? enabled
                    : node_measurement_compare(&copy->measurements[id], &copy->measurements[previous]) < 0;
            if (enabled == previous_enabled && group_code[0]) {
                const NodeFriendSummary *a = &group.nodes[id], *b = &group.nodes[previous];
                const bool ready_a = group.participants && a->reachable_members == group.participants &&
                                     a->measured_members == group.participants;
                const bool ready_b = group.participants && b->reachable_members == group.participants &&
                                     b->measured_members == group.participants;
                if (ready_a != ready_b)
                    first = ready_a;
                else if (a->minimum_received != b->minimum_received)
                    first = a->minimum_received > b->minimum_received;
                else if (a->worst_median_ms != b->worst_median_ms)
                    first = a->worst_median_ms < b->worst_median_ms;
                else if (a->worst_jitter_ms != b->worst_jitter_ms)
                    first = a->worst_jitter_ms < b->worst_jitter_ms;
            }
            if (!first)
                break;
            order[j] = previous;
            j--;
        }
        order[j] = id;
    }
    unsigned top = 0;
    for (unsigned i = 0; i < *count && top < 3; i++) {
        const uint32_t id = order[i];
        if (!(copy->enabled & (1u << id)) || !node_measurement_recommended(&copy->measurements[id]))
            continue;
        bool duplicate = false;
        for (unsigned j = 0; j < i; j++)
            if (recommended[order[j]] && !strcmp(LanywhereNodes[order[j]].host, LanywhereNodes[id].host))
                duplicate = true;
        if (duplicate)
            continue;
        if (group_code[0]) {
            const NodeFriendSummary *shared = &group.nodes[id];
            if (!group.participants || shared->reachable_members != group.participants ||
                shared->measured_members != group.participants || shared->minimum_sent < 5 ||
                shared->minimum_received < 4)
                continue;
        }
        recommended[id] = true;
        top++;
    }
}
static void measurement_label(const NodeMeasurement *m, char text[80])
{
    if (m->state == NodeMeasureMeasuring || m->state == NodeMeasurePending)
        snprintf(text, 80, "%s", ui_text("Testing…"));
    else if (m->state != NodeMeasureReady)
        snprintf(text, 80, "%s", m->measured_at_ms ? ui_text("Unreachable") : "—");
    else
        snprintf(text, 80, "%u ms · %s", m->median_ms, m->kind == NodeProbeUdp ? "UDP" : "TCP");
}
static uint8_t region_for_row(unsigned row)
{
    return row ? (uint8_t)(row - 1) : NODE_ALL_REGIONS;
}

static unsigned region_row(uint8_t region)
{
    return region < NODE_REGION_COUNT - 1 ? (unsigned)region + 1 : 0;
}

static void begin_screen(const char *title)
{
    gfx_begin();
    gfx_clear_gradient(NODE_COLOR_BG, NODE_COLOR_BG);
    gfx_rect(0, 0, GFX_W, 83, NODE_COLOR_PANEL);
    gfx_rect(0, 82, GFX_W, 1, NODE_COLOR_LINE);
    gfx_round_rect(40, 30, 22, 18, 5, NODE_COLOR_ACCENT);
    gfx_round_rect(45, 34, 12, 10, 3, NODE_COLOR_PANEL);
    gfx_round_rect(54, 39, 22, 18, 5, NODE_COLOR_ACCENT);
    gfx_round_rect(59, 43, 12, 10, 3, NODE_COLOR_PANEL);
    gfx_text(90, 24, 36, NODE_COLOR_TEXT, "RoomCircuit");
    const char *language = ui_chinese() ? "English" : "中文";
    const int language_width = gfx_text_width(18, language) + 50;
    gfx_round_rect(1240 - language_width, 22, language_width, 38, 8, NODE_COLOR_SELECT);
    gfx_text(1250 - language_width, 32, 16, NODE_COLOR_ACCENT, "RS");
    gfx_text(1275 - language_width, 31, 18, NODE_COLOR_TEXT, language);
    if (language_save_failed)
        gfx_text(1068, 91, 18, NODE_COLOR_WARN, ui_text("Not saved"));
    gfx_text(40, 109, 35, NODE_COLOR_TEXT, ui_text(title));
}

static void node_footer(void)
{
    gfx_rect(0, NODE_FOOTER_Y, GFX_W, GFX_H - NODE_FOOTER_Y, NODE_COLOR_FOOT);
    gfx_rect(0, NODE_FOOTER_Y, GFX_W, 1, NODE_COLOR_LINE);
}

static void node_panel(void)
{
    gfx_round_rect(NODE_PANEL_X, NODE_PANEL_Y, NODE_PANEL_W, NODE_PANEL_H, 11, NODE_COLOR_LINE);
    gfx_round_rect(NODE_PANEL_X + 1, NODE_PANEL_Y + 1, NODE_PANEL_W - 2, NODE_PANEL_H - 2, 10,
                   NODE_COLOR_PANEL);
}

static void draw_regions(uint8_t region, bool focus, bool draw_counts)
{
    gfx_round_rect(41, 196, NODE_TABLE_X - 40, NODE_PANEL_H - 2, 10, NODE_COLOR_RAIL);
    gfx_rect(NODE_TABLE_X - 1, 196, 1, NODE_PANEL_H - 2, NODE_COLOR_LINE);
    gfx_text(58, 207, 21, NODE_COLOR_TEXT, ui_text("Regions"));
    gfx_rect(41, 241, NODE_TABLE_X - 41, 1, NODE_COLOR_LINE);
    for (unsigned i = 0; i < NODE_REGION_COUNT; i++) {
        const uint8_t id = region_for_row(i);
        const bool current = id == region;
        const int y = NODE_REGION_Y + (int)i * NODE_REGION_HEIGHT;
        if (current) {
            gfx_round_rect(48, y + 4, 301, 42, 6, NODE_COLOR_SELECT);
            if (focus)
                gfx_round_rect(48, y + 9, 4, 32, 2, NODE_COLOR_ACCENT);
        }
        gfx_round_rect(62, y + 20, 9, 9, 5, current ? NODE_COLOR_ACCENT : NODE_COLOR_LINE);
        gfx_text_fit(84, y + 12, 20, current ? NODE_COLOR_ACCENT : NODE_COLOR_TEXT, nodes_ui_region(id), 214);
        if (draw_counts) {
            unsigned total = 0;
            for (unsigned j = 0; j < NODE_CATALOG_COUNT; j++)
                if (id == NODE_ALL_REGIONS || LanywhereNodes[j].region == id)
                    total++;
            char label[8];
            snprintf(label, sizeof(label), "%u", total);
            gfx_text(331 - gfx_text_width(18, label), y + 14, 18, NODE_COLOR_DIM, label);
        }
    }
}

static void node_location(const LanywhereNode *node, char location[96])
{
    const char *place = node->country[0] ? nodes_ui_country(node->country) :
                        node->region == NodeRegionUnknown ? ui_text("Unverified") : nodes_ui_region(node->region);
    snprintf(location, 96, "%s", place);
}

static void draw_picker(const NodeView *copy, uint8_t region, const uint32_t *order, unsigned count,
                        unsigned selected, uint32_t current, bool region_focus, const bool *recommended)
{
    begin_screen("Choose hosting node");
    const unsigned first = selected / NODE_ROWS * NODE_ROWS;
    char text[180];
    if (group_code[0]) {
        snprintf(text, sizeof(text), ui_text("Code %s · %u players"), group_code, group.participants);
        gfx_text_fit(490, 119, 21, NODE_COLOR_TEXT, text, 490);
    }
    if (first)
        gfx_button(1000, 114, "ZL", ui_text("Prev"));
    if (first + NODE_ROWS < count)
        gfx_button(1120, 114, "ZR", ui_text("Next"));
    node_panel();
    draw_regions(region, region_focus, true);
    gfx_text_fit(379, 207, 21, NODE_COLOR_TEXT, nodes_ui_region(region), 825);
    gfx_rect(NODE_TABLE_X, 242, 879, 32, NODE_COLOR_RAIL);
    gfx_rect(NODE_TABLE_X, 274, 879, 1, NODE_COLOR_LINE);
    gfx_text(379, 249, 17, NODE_COLOR_DIM, ui_text("Node"));
    gfx_text(704, 249, 17, NODE_COLOR_DIM, ui_text("Region"));
    gfx_text(864, 249, 17, NODE_COLOR_DIM, ui_text("Latency"));
    gfx_text(1090, 249, 17, NODE_COLOR_DIM, ui_text("Status"));
    for (unsigned i = first; i < count && i < first + NODE_ROWS; i++) {
        const uint32_t id = order[i];
        const LanywhereNode *node = &LanywhereNodes[id];
        const NodeMeasurement *m = &copy->measurements[id];
        const bool enabled = copy->catalog_ok && (copy->enabled & (1u << id));
        const bool available = copy->catalog_ok && (copy->available & (1u << id));
        const int y = NODE_LIST_Y + (int)(i - first) * NODE_ROW_HEIGHT;
        if (i == selected) {
            gfx_rect(NODE_TABLE_X, y, 879, NODE_ROW_HEIGHT - 1, NODE_COLOR_SELECT);
            if (!region_focus)
                gfx_round_rect(360, y + 7, 4, NODE_ROW_HEIGHT - 15, 2, NODE_COLOR_ACCENT);
        }
        gfx_rect(NODE_TABLE_X, y + NODE_ROW_HEIGHT - 1, 879, 1, NODE_COLOR_LINE);
        gfx_text_fit(379, y + (recommended[id] ? 7 : 16), 19, enabled ? NODE_COLOR_TEXT : NODE_COLOR_DIM,
                     nodes_ui_name(id), 305);
        if (recommended[id]) {
            const char *recommendation = group_code[0] ? " (Recommended)" : " (Recommended for you)";
            gfx_text_fit(379, y + 31, 15, NODE_COLOR_ACCENT, ui_text(recommendation), 305);
        }
        char location[96];
        node_location(node, location);
        gfx_text_fit(704, y + 16, 18, NODE_COLOR_DIM, location, 145);
        const char *protocol = node->protocol == NodeProtocolLanPlay ? "UDP" : "TCP";
        if (m->state == NodeMeasureReady && group_code[0] && group.participants &&
            group.nodes[id].measured_members == group.participants) {
            if (group.nodes[id].reachable_members < group.participants)
                snprintf(text, sizeof(text), "%s", ui_text("Unstable for a player"));
            else
                snprintf(text, sizeof(text), ui_text("You %u / worst %u ms"),
                         m->median_ms, group.nodes[id].worst_median_ms);
        } else if (m->state == NodeMeasureReady && !node_measurement_recommended(m)) {
            snprintf(text, sizeof(text), "%u ms · %s · %u/%u", m->median_ms, protocol, m->received, m->sent);
        } else if (m->state == NodeMeasureReady) {
            measurement_label(m, text);
        } else {
            snprintf(text, sizeof(text), "%s", protocol);
        }
        gfx_text_fit(864, y + 16, 18, NODE_COLOR_TEXT, text, 211);
        const char *status = "Not measured";
        u32 color = NODE_COLOR_DIM;
        if (!copy->catalog_ok)
            status = "Unavailable";
        else if (!enabled)
            status = "Disabled";
        else if (m->state == NodeMeasurePending || m->state == NodeMeasureMeasuring)
            status = "Testing…";
        else if (m->state == NodeMeasureFailed && m->measured_at_ms) {
            status = "Unreachable";
            color = NODE_COLOR_WARN;
        } else if (m->state == NodeMeasureReady) {
            status = id == current ? "Current" : available ? "Online" : "Unlisted";
            color = id == current ? NODE_COLOR_ACCENT : NODE_COLOR_DIM;
        }
        gfx_text_fit(1090, y + 16, 17, color, ui_text(status), 134);
    }
    if (!count)
        gfx_text(379, 306, 23, NODE_COLOR_DIM, ui_text("No nodes in this region."));
    if (copy->probe_start_failed)
        gfx_text_fit(40, 618, 20, NODE_COLOR_WARN, ui_text("Could not start node tests. Try again."), 1200);
    else if (notice)
        gfx_text_fit(40, 618, 20, NODE_COLOR_WARN, ui_text(notice), 1200);
    else if (group_code[0] && group.reported_participants < group.participants)
        gfx_text_fit(40, 618, 20, NODE_COLOR_DIM, ui_text("Waiting for other players' results."), 1200);
    else if (!copy->catalog_ok)
        gfx_text_fit(40, 618, 20, NODE_COLOR_WARN, ui_text("Could not load node availability. Refresh."), 1200);
    else {
        unsigned finished = 0;
        for (unsigned id = 0; id < NODE_CATALOG_COUNT; id++)
            if (copy->measurements[id].state == NodeMeasureReady || copy->measurements[id].state == NodeMeasureFailed)
                finished++;
        if (finished < NODE_CATALOG_COUNT) {
            snprintf(text, sizeof(text), ui_text("Testing nodes: %u/%u"), finished, NODE_CATALOG_COUNT);
            gfx_text(40, 618, 20, NODE_COLOR_DIM, text);
        }
    }
    node_footer();
    if (current < NODE_CATALOG_COUNT) {
        snprintf(text, sizeof(text), "%s · %s", ui_text("Current"), nodes_ui_name(current));
        gfx_text_fit(40, 673, 19, NODE_COLOR_ACCENT, text, 352);
    }
    gfx_button(430, 669, "A", ui_text("Select"));
    gfx_button(570, 669, "R", ui_text("Region"));
    gfx_button(730, 669, "X", ui_text("Refresh"));
    gfx_button(885, 669, "L", ui_text("Test with friends"));
    gfx_button(1140, 669, "B", ui_text("Back"));
    gfx_end();
}

static u64 picker_touch(const uint32_t rendered[NODE_ROWS], uint32_t *touched, uint8_t *touched_region,
                        bool *held, bool prev, bool next)
{
    HidTouchScreenState touch = {0};
    const bool down = hidGetTouchScreenStates(&touch, 1) && touch.count;
    if (!down) {
        *held = false;
        return 0;
    }
    if (*held)
        return 0;
    *held = true;
    const int x = touch.touches[0].x, y = touch.touches[0].y;
    if (x >= 1056 && y >= 15 && y < 76)
        return HidNpadButton_StickR;
    if (y >= 105 && y < 151) {
        if (x >= 1000 && x < 1120 && prev)
            return HidNpadButton_ZL;
        if (x >= 1120 && x < 1240 && next)
            return HidNpadButton_ZR;
    }
    if (x >= 40 && x < NODE_TABLE_X && y >= NODE_REGION_Y &&
        y < NODE_REGION_Y + NODE_REGION_COUNT * NODE_REGION_HEIGHT) {
        *touched_region = region_for_row((unsigned)(y - NODE_REGION_Y) / NODE_REGION_HEIGHT);
        return 0;
    }
    if (x >= NODE_TABLE_X && x < 1240 && y >= NODE_LIST_Y &&
        y < NODE_LIST_Y + NODE_ROWS * NODE_ROW_HEIGHT) {
        const unsigned index = (unsigned)(y - NODE_LIST_Y) / NODE_ROW_HEIGHT;
        if (rendered[index] != NODE_ID_INVALID) {
            *touched = rendered[index];
            return HidNpadButton_A;
        }
    }
    if (y >= NODE_FOOTER_Y) {
        if (x >= 430 && x < 570)
            return HidNpadButton_A;
        if (x >= 570 && x < 730)
            return HidNpadButton_R;
        if (x >= 730 && x < 885)
            return HidNpadButton_X;
        if (x >= 885 && x < 1140)
            return HidNpadButton_L;
        if (x >= 1140)
            return HidNpadButton_B;
    }
    return 0;
}
static bool modal_touch_start(bool *held, int *x, int *y)
{
    HidTouchScreenState touch = {0};
    const bool down = hidGetTouchScreenStates(&touch, 1) && touch.count;
    if (!down) {
        *held = false;
        return false;
    }
    if (*held)
        return false;
    *held = true;
    *x = touch.touches[0].x;
    *y = touch.touches[0].y;
    return true;
}

static void draw_friend_panel(void)
{
    begin_screen("Test with friends");
    if (group_code[0]) {
        char text[160];
        snprintf(text, sizeof(text), ui_text("Code %s · %u players"), group_code, group.participants);
        gfx_text(56, 205, 32, NODE_COLOR_TEXT, text);
    }
    if (keyboard_error)
        gfx_text_fit(56, 460, 21, NODE_COLOR_WARN, ui_text(keyboard_error), 1168);
    if (notice)
        gfx_text_fit(56, 420, 22, NODE_COLOR_WARN, ui_text(notice), 1168);
    node_footer();
    if (!group_code[0]) {
        gfx_button(56, 670, "A", ui_text("Create code"));
        gfx_button(330, 670, "X", ui_text("Join code"));
    } else
        gfx_button(600, 670, "Y", ui_text("Leave test"));
    gfx_button(980, 670, "B", ui_text("Back"));
    gfx_end();
}
static void friend_panel(PadState *pad)
{
    keyboard_error = NULL;
    bool dirty = true, touch_held = true;
    NodeProbeLoop probe_loop = probe_loop_start();
    while (appletMainLoop()) {
        padUpdate(pad);
        u64 down = padGetButtonsDown(pad);
        int touch_x, touch_y;
        if (modal_touch_start(&touch_held, &touch_x, &touch_y)) {
            if (touch_x >= 1056 && touch_y >= 15 && touch_y < 76)
                down |= HidNpadButton_StickR;
            else if (touch_y >= 650) {
                if (touch_x < 300)
                    down |= HidNpadButton_A;
                else if (touch_x < 590)
                    down |= HidNpadButton_X;
                else if (touch_x < 900)
                    down |= HidNpadButton_Y;
                else
                    down |= HidNpadButton_B;
            }
        }
        collect_probes();
        maintain_probes(&probe_loop, (1u << NODE_CATALOG_COUNT) - 1);
        if (collect_friend())
            dirty = true;
        language_input(down);
        if (down & HidNpadButton_B)
            break;
        if ((down & HidNpadButton_A) && !group_code[0]) {
            if (queue_friend(NodeFriendCreate, NULL))
                notice = NULL;
            dirty = true;
        }
        if ((down & HidNpadButton_X) && !group_code[0]) {
            char code[8] = {0};
            if (ask_friend_code(code) && queue_friend(NodeFriendJoin, code))
                notice = NULL;
            dirty = true;
        }
        if ((down & HidNpadButton_Y) && group_code[0]) {
            queue_friend(NodeFriendLeave, group_code);
            dirty = true;
        }
        const uint64_t now = node_probe_now_ms();
        if (group_code[0] && !friend_created && now >= next_friend_poll) {
            queue_friend(NodeFriendReport, group_code);
            next_friend_poll = now + 3000;
        }
        if (dirty || down) {
            draw_friend_panel();
            dirty = false;
        }
        svcSleepThread(33000000ULL);
    }
}
static unsigned picker_index(const uint32_t *order, unsigned count, uint32_t id)
{
    for (unsigned i = 0; i < count; i++)
        if (order[i] == id)
            return i;
    return count;
}

bool nodes_ui_pick(PadState *pad, Service *service, uint32_t current, uint32_t *selected_node)
{
    if (!initialized)
        return false;
    const uint32_t all = (1u << NODE_CATALOG_COUNT) - 1;
    start_probes(all, false);
    notice = NULL;
    unsigned selected = 0;
    uint8_t region = NODE_ALL_REGIONS;
    bool region_focus = false;
    bool dirty = true, moved = false, touch_held = true;
    bool rendered_prev = false, rendered_next = false;
    uint32_t selected_id = current, rendered_selected = NODE_ID_INVALID, rendered[NODE_ROWS];
    for (unsigned i = 0; i < NODE_ROWS; i++)
        rendered[i] = NODE_ID_INVALID;
    NodeProbeLoop probe_loop = probe_loop_start();
    while (appletMainLoop()) {
        collect_probes();
        maintain_probes(&probe_loop, all);
        if (collect_friend())
            dirty = true;
        const uint64_t now = node_probe_now_ms();
        if (group_code[0] && !friend_created && now >= next_friend_poll) {
            queue_friend(NodeFriendReport, group_code);
            next_friend_poll = now + 3000;
        }
        padUpdate(pad);
        u64 down = padGetButtonsDown(pad);
        const NodeView copy = snapshot();
        uint32_t order[NODE_CATALOG_COUNT];
        unsigned count;
        bool recommended[NODE_CATALOG_COUNT];
        rank_nodes(&copy, region, order, &count, recommended);

        uint32_t touched = NODE_ID_INVALID;
        uint8_t touched_region = NODE_NO_REGION_TOUCHED;
        down |= picker_touch(rendered, &touched, &touched_region, &touch_held, rendered_prev, rendered_next);
        language_input(down);
        if (down & HidNpadButton_B) {
            cancel_probes();
            return false;
        }
        if (down & HidNpadButton_X) {
            notice = NULL;
            start_probes(all, true);
            dirty = true;
            continue;
        }
        if (down & HidNpadButton_L) {
            friend_panel(pad);
            dirty = true;
            touch_held = true;
            continue;
        }

        if (down & HidNpadButton_AnyLeft) {
            region_focus = true;
            moved = true;
        }
        if (down & HidNpadButton_AnyRight) {
            region_focus = false;
            moved = true;
        }
        if (touched != NODE_ID_INVALID)
            region_focus = false;
        const unsigned old_region_row = region_row(region);
        unsigned new_region_row = old_region_row;
        if (touched_region != NODE_NO_REGION_TOUCHED) {
            new_region_row = region_row(touched_region);
            region_focus = true;
            dirty = true;
        } else if (down & HidNpadButton_R) {
            new_region_row = (old_region_row + 1) % NODE_REGION_COUNT;
        } else if (region_focus) {
            if ((down & HidNpadButton_AnyUp) && new_region_row)
                new_region_row--;
            if ((down & HidNpadButton_AnyDown) && new_region_row + 1 < NODE_REGION_COUNT)
                new_region_row++;
        }
        if (new_region_row != old_region_row) {
            region = region_for_row(new_region_row);
            rank_nodes(&copy, region, order, &count, recommended);
            selected_id = count ? order[0] : NODE_ID_INVALID;
            rendered_selected = NODE_ID_INVALID;
            notice = NULL;
            moved = true;
            down &= ~HidNpadButton_A;
        }

        if (!moved && count && recommended[order[0]])
            selected_id = order[0];
        selected = picker_index(order, count, selected_id);
        if (selected == count)
            selected = 0;
        if (!region_focus && count) {
            if ((down & HidNpadButton_AnyUp) && selected) {
                selected--;
                moved = true;
            }
            if ((down & HidNpadButton_AnyDown) && selected + 1 < count) {
                selected++;
                moved = true;
            }
            if (down & HidNpadButton_ZL) {
                const unsigned first = selected / NODE_ROWS * NODE_ROWS;
                if (first) {
                    selected = first - NODE_ROWS;
                    moved = true;
                }
            }
            if (down & HidNpadButton_ZR) {
                const unsigned next = (selected / NODE_ROWS + 1) * NODE_ROWS;
                if (next < count) {
                    selected = next;
                    moved = true;
                }
            }
        }
        selected_id = count ? order[selected] : NODE_ID_INVALID;
        if (down & HidNpadButton_A) {
            if (region_focus) {
                region_focus = false;
            } else {
                const uint32_t id = touched != NODE_ID_INVALID ? touched : rendered_selected;
                const unsigned confirmed = picker_index(order, count, id);
                if (confirmed < count) {
                    selected = confirmed;
                    selected_id = id;
                    moved = true;
                    const NodeMeasurement *m = &copy.measurements[id];
                    if (!copy.catalog_ok || !(copy.enabled & (1u << id)) || m->state != NodeMeasureReady) {
                        notice = "This node is not ready. Refresh or choose another.";
                    } else {
                        node_selection_trace_begin(id);
                        node_selection_trace_phase(NodeSelectionBeforeIpc, 0);
                        const uint32_t result = serviceDispatchIn(service, 36, id);
                        node_selection_trace_phase(NodeSelectionAfterIpc, result);
                        if (R_SUCCEEDED(result)) {
                            *selected_node = id;
                            node_selection_trace_phase(NodeSelectionBeforeCancelProbes, 0);
                            cancel_probes();
                            node_selection_trace_phase(NodeSelectionAfterCancelProbes, 0);
                            return true;
                        }
                        notice = "Could not select this node. Try another.";
                    }
                }
            }
        }
        mutexLock(&view_lock);
        dirty |= view.changed;
        view.changed = false;
        mutexUnlock(&view_lock);
        if (dirty || down) {
            draw_picker(&copy, region, order, count, selected, current, region_focus, recommended);
            rendered_selected = selected_id;
            const unsigned first = selected / NODE_ROWS * NODE_ROWS;
            rendered_prev = first != 0;
            rendered_next = first + NODE_ROWS < count;
            for (unsigned i = 0; i < NODE_ROWS; i++)
                rendered[i] = first + i < count ? order[first + i] : NODE_ID_INVALID;
            dirty = false;
        }
        svcSleepThread(33000000ULL);
    }
    cancel_probes();
    return false;
}
static void draw_room_check(const NodeMeasurement *m, uint32_t node_id, bool gateway, bool probe_start_failed)
{
    begin_screen("Check room connection");
    gfx_text(56, 205, 28, NODE_COLOR_TEXT, nodes_ui_name(node_id));
    char value[80];
    measurement_label(m, value);
    gfx_text(56, 267, 32, NODE_COLOR_TEXT, value);
    if (gateway)
        gfx_text(56, 330, 23, NODE_COLOR_DIM, ui_text("Main server gateway"));
    if (m->state == NodeMeasureReady) {
        snprintf(value, sizeof(value), ui_text("Replies %u/%u · variation %u ms"), m->received, m->sent,
                 m->jitter_ms);
        gfx_text(56, 380, 22, NODE_COLOR_DIM, value);
        if (node_measurement_weak(m))
            gfx_text(56, 450, 24, NODE_COLOR_WARN, ui_text("This connection may be slow or unstable."));
        if (gateway)
            gfx_text_fit(56, 500, 21, NODE_COLOR_DIM,
                         ui_text("External forwarding adds latency beyond this measurement."), 1168);
    }
    if (probe_start_failed)
        gfx_text_fit(56, 550, 21, NODE_COLOR_WARN, ui_text("Could not start node tests. Try again."), 1168);
    node_footer();
    if (m->state == NodeMeasureReady)
        gfx_button(56, 670, "A", ui_text("Continue"));
    if (probe_start_failed)
        gfx_button(420, 670, "X", ui_text("Refresh"));
    gfx_button(980, 670, "B", ui_text("Back"));
    gfx_end();
}
bool nodes_ui_check_room(PadState *pad, uint32_t node_id, bool gateway)
{
    if (node_id >= NODE_CATALOG_COUNT)
        return false;
    start_probes(1u << node_id, false);
    bool dirty = true, touch_held = true;
    NodeProbeLoop probe_loop = probe_loop_start();
    while (appletMainLoop()) {
        collect_probes();
        maintain_probes(&probe_loop, 1u << node_id);
        padUpdate(pad);
        u64 down = padGetButtonsDown(pad);
        const NodeView copy = snapshot();
        int touch_x, touch_y;
        if (modal_touch_start(&touch_held, &touch_x, &touch_y)) {
            if (touch_x >= 1056 && touch_y >= 15 && touch_y < 76)
                down |= HidNpadButton_StickR;
            else if (touch_y >= 650) {
                if (copy.probe_start_failed && touch_x >= 340 && touch_x < 760)
                    down |= HidNpadButton_X;
                else
                    down |= touch_x < 600 ? HidNpadButton_A : HidNpadButton_B;
            }
        }
        language_input(down);
        const NodeMeasurement *m = &copy.measurements[node_id];
        if (down & HidNpadButton_B) {
            cancel_probes();
            return false;
        }
        if ((down & HidNpadButton_X) && copy.probe_start_failed) {
            start_probes(1u << node_id, true);
            continue;
        }
        if ((down & HidNpadButton_A) && m->state == NodeMeasureReady) {
            cancel_probes();
            return true;
        }
        mutexLock(&view_lock);
        dirty |= view.changed;
        view.changed = false;
        mutexUnlock(&view_lock);
        if (dirty || down) {
            draw_room_check(m, node_id, gateway, copy.probe_start_failed);
            dirty = false;
        }
        svcSleepThread(33000000ULL);
    }
    cancel_probes();
    return false;
}
static bool ask_game_search(char query[64])
{
    keyboard_error = NULL;
    SwkbdConfig keyboard;
    if (R_FAILED(swkbdCreate(&keyboard, 0))) {
        keyboard_error = "Could not open the keyboard. Try again.";
        return false;
    }
    swkbdConfigMakePresetDefault(&keyboard);
    swkbdConfigSetHeaderText(&keyboard, ui_text("Search game"));
    swkbdConfigSetStringLenMax(&keyboard, 20);
    const Result result = swkbdShow(&keyboard, query, 64);
    swkbdClose(&keyboard);
    if (R_FAILED(result) && result != MAKERESULT(Module_Libnx, LibnxError_LibAppletBadExit))
        keyboard_error = "Could not open the keyboard. Try again.";
    return R_SUCCEEDED(result);
}
static void draw_filters(const GameCatalog *catalog, const CatalogGameChoice *choices, size_t count,
                         int selected, uint64_t chosen, uint8_t chosen_region, bool region_focus)
{
    begin_screen("Filter rooms");
    node_panel();
    draw_regions(chosen_region, region_focus, false);
    gfx_text(379, 207, 21, NODE_COLOR_TEXT, ui_text("Game"));
    gfx_rect(NODE_TABLE_X, 241, 879, 1, NODE_COLOR_LINE);
    const CatalogMatch selected_title = game_catalog_identify(catalog, 0, chosen);
    const char *active = (chosen && selected_title.kind) ? game_catalog_name(&selected_title, ui_chinese())
                                                          : ui_text("All games");
    for (int i = -1; i < (int)count; i++) {
        const int y = NODE_FILTER_LIST_Y + i * NODE_FILTER_ROW_HEIGHT;
        const bool highlighted = i == selected;
        if (highlighted) {
            gfx_rect(NODE_TABLE_X, y, 879, NODE_FILTER_ROW_HEIGHT - 1, NODE_COLOR_SELECT);
            if (!region_focus)
                gfx_round_rect(NODE_TABLE_X, y + 7, 4, NODE_FILTER_ROW_HEIGHT - 15, 2, NODE_COLOR_ACCENT);
        }
        gfx_rect(NODE_TABLE_X, y + NODE_FILTER_ROW_HEIGHT - 1, 879, 1, NODE_COLOR_LINE);
        const char *name = i < 0 ? active : game_catalog_name(&choices[i].title, ui_chinese());
        gfx_text_fit(379, y + 14, 21, NODE_COLOR_TEXT, name, i < 0 ? 650 : 830);
        if (i < 0)
            gfx_text_fit(1090, y + 16, 17, NODE_COLOR_DIM, ui_text("Current"), 134);
    }
    if (keyboard_error)
        gfx_text_fit(40, 618, 20, NODE_COLOR_WARN, ui_text(keyboard_error), 1200);
    node_footer();
    gfx_button(40, 669, "A", ui_text("Apply"));
    gfx_button(216, 669, "R", ui_text("Region"));
    gfx_button(390, 669, "X", ui_text("Search game"));
    gfx_button(620, 669, "Y", ui_text("All games"));
    gfx_button(880, 669, "ZR", ui_text("Next"));
    gfx_button(1140, 669, "B", ui_text("Back"));
    gfx_end();
}
bool nodes_ui_filters(PadState *pad, const GameCatalog *catalog, uint64_t *game_id, uint8_t *region)
{
    keyboard_error = NULL;
    char query[64] = {0};
    CatalogGameChoice choices[6];
    size_t offset = 0, next = SIZE_MAX;
    int selected = -1;
    bool dirty = true, touch_held = true, region_focus = false;
    uint64_t chosen = *game_id;
    uint8_t chosen_region = *region;
    size_t count = game_catalog_choices(catalog, query, offset, choices, 6, &next);
    while (appletMainLoop()) {
        padUpdate(pad);
        u64 down = padGetButtonsDown(pad);
        int touch_x, touch_y;
        if (modal_touch_start(&touch_held, &touch_x, &touch_y)) {
            if (touch_x >= 1056 && touch_y >= 15 && touch_y < 76)
                down |= HidNpadButton_StickR;
            else if (touch_x >= NODE_PANEL_X && touch_x < NODE_TABLE_X && touch_y >= NODE_REGION_Y &&
                     touch_y < NODE_REGION_Y + NODE_REGION_COUNT * NODE_REGION_HEIGHT) {
                chosen_region = region_for_row((unsigned)(touch_y - NODE_REGION_Y) / NODE_REGION_HEIGHT);
                region_focus = true;
                dirty = true;
                down &= ~HidNpadButton_A;
            } else if (touch_x >= NODE_TABLE_X && touch_x < NODE_PANEL_X + NODE_PANEL_W &&
                       touch_y >= NODE_FILTER_LIST_Y - NODE_FILTER_ROW_HEIGHT && touch_y <
                       NODE_FILTER_LIST_Y + NODE_FILTER_ROW_HEIGHT * (int)count) {
                selected = (touch_y - NODE_FILTER_LIST_Y + NODE_FILTER_ROW_HEIGHT) / NODE_FILTER_ROW_HEIGHT - 1;
                region_focus = false;
                down |= HidNpadButton_A;
            } else if (touch_y >= NODE_FOOTER_Y) {
                if (touch_x < 216)
                    down |= HidNpadButton_A;
                else if (touch_x < 390)
                    down |= HidNpadButton_R;
                else if (touch_x < 620)
                    down |= HidNpadButton_X;
                else if (touch_x < 880)
                    down |= HidNpadButton_Y;
                else if (touch_x < 1140)
                    down |= HidNpadButton_ZR;
                else
                    down |= HidNpadButton_B;
            }
        }
        language_input(down);
        if (down & HidNpadButton_B)
            return false;
        if (down & HidNpadButton_AnyLeft)
            region_focus = true;
        if (down & HidNpadButton_AnyRight)
            region_focus = false;
        unsigned row = region_row(chosen_region);
        if (down & HidNpadButton_R)
            row = (row + 1) % NODE_REGION_COUNT;
        else if (region_focus) {
            if ((down & HidNpadButton_AnyUp) && row)
                row--;
            if ((down & HidNpadButton_AnyDown) && row + 1 < NODE_REGION_COUNT)
                row++;
        }
        chosen_region = region_for_row(row);
        if (down & HidNpadButton_X) {
            if (ask_game_search(query)) {
                offset = 0;
                selected = -1;
            }
            dirty = true;
        }
        if (down & HidNpadButton_Y) {
            chosen = 0;
            query[0] = 0;
            offset = 0;
            selected = -1;
            dirty = true;
        }
        if ((down & HidNpadButton_ZR) && next != SIZE_MAX) {
            offset = next;
            selected = 0;
            dirty = true;
        }
        if (down & HidNpadButton_ZL) {
            offset = 0;
            selected = -1;
            dirty = true;
        }
        if (down & (HidNpadButton_X | HidNpadButton_Y | HidNpadButton_ZL | HidNpadButton_ZR)) {
            count = game_catalog_choices(catalog, query, offset, choices, 6, &next);
            down &= ~HidNpadButton_A;
        }
        if (!region_focus && (down & HidNpadButton_AnyDown) && selected + 1 < (int)count) {
            selected++;
            dirty = true;
        }
        if (!region_focus && (down & HidNpadButton_AnyUp) && selected >= 0) {
            selected--;
            dirty = true;
        }
        if ((down & HidNpadButton_A) && region_focus) {
            region_focus = false;
            down &= ~HidNpadButton_A;
            dirty = true;
        }
        if (down & HidNpadButton_A) {
            if (selected >= 0 && (size_t)selected < count)
                chosen = choices[selected].game_id;
            *game_id = chosen;
            *region = chosen_region;
            return true;
        }
        if (dirty || down) {
            draw_filters(catalog, choices, count, selected, chosen, chosen_region, region_focus);
            dirty = false;
        }
        svcSleepThread(33000000ULL);
    }
    return false;
}
