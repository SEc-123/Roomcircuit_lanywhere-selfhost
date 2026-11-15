#include "nodes_ui.h"
#include "../../common/node_lobby.h"
#include "app_socket.h"
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
#define NODE_COLOR_TEXT RGBA(242, 243, 247, 255)
#define NODE_COLOR_DIM RGBA(154, 160, 176, 255)
#define NODE_COLOR_ACCENT RGBA(139, 92, 246, 255)
#define NODE_COLOR_WARN RGBA(245, 165, 36, 255)
#define NODE_COLOR_PANEL RGBA(255, 255, 255, 16)

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
    if (id >= NODE_CATALOG_COUNT)
        return ui_text("Unknown node");
    return ui_chinese() ? LanywhereNodes[id].name_zh : LanywhereNodes[id].name_en;
}
const char *nodes_ui_region(uint8_t region)
{
    static const char *labels[] = {"Region unverified", "Asia", "Europe", "Americas", "Oceania", "Africa"};
    return ui_text(region < 6 ? labels[region] : "All regions");
}
static const char *country_name(const char *country)
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
    (void)ui_locale_save(UI_LOCALE_PATH, chinese);
}
static bool ask_friend_code(char code[8])
{
    SwkbdConfig keyboard;
    if (R_FAILED(swkbdCreate(&keyboard, 0)))
        return false;
    swkbdConfigMakePresetDefault(&keyboard);
    swkbdConfigSetHeaderText(&keyboard, ui_text("Shared test code"));
    swkbdConfigSetStringLenMin(&keyboard, 6);
    swkbdConfigSetStringLenMax(&keyboard, 6);
    const Result result = swkbdShow(&keyboard, code, 8);
    swkbdClose(&keyboard);
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
static void begin_screen(const char *title)
{
    gfx_begin();
    gfx_clear_gradient(RGBA(27, 34, 54, 255), RGBA(15, 18, 28, 255));
    gfx_text(56, 36, 40, NODE_COLOR_TEXT, "LANywhere");
    gfx_button(1068, 47, "RS", ui_chinese() ? "English" : "中文");
    gfx_text(56, 125, 30, NODE_COLOR_TEXT, ui_text(title));
}
static void draw_picker(const NodeView *copy, uint8_t region, const uint32_t *order, unsigned count,
                        unsigned selected, const bool *recommended)
{
    begin_screen("Choose hosting node");
    const unsigned first = selected / NODE_ROWS * NODE_ROWS;
    char text[180];
    gfx_button(56, 175, "R", nodes_ui_region(region));
    if (group_code[0]) {
        snprintf(text, sizeof(text), ui_text("Code %s · %u players"), group_code, group.participants);
        gfx_text(600, 181, 21, NODE_COLOR_TEXT, text);
    } else
        gfx_text(600, 181, 21, NODE_COLOR_DIM, ui_text("Recommended for your network"));
    for (unsigned i = first; i < count && i < first + NODE_ROWS; i++) {
        const uint32_t id = order[i];
        const LanywhereNode *node = &LanywhereNodes[id];
        const int y = 225 + (int)(i - first) * 58;
        const bool enabled = copy->catalog_ok && (copy->enabled & (1u << id));
        gfx_round_rect(56, y, 1168, 52, 12, i == selected ? RGBA(139, 92, 246, 65) : NODE_COLOR_PANEL);
        snprintf(text, sizeof(text), "%s%s", nodes_ui_name(id),
                 recommended[id] ? ui_text(" (Recommended)") : "");
        gfx_text_fit(74, y + 4, 22, enabled ? NODE_COLOR_TEXT : NODE_COLOR_DIM, text, 700);
        snprintf(text, sizeof(text), "%s%s%s · %s", nodes_ui_region(node->region),
                 node->country[0] ? " · " : "", country_name(node->country),
                 node->protocol == NodeProtocolLanPlay ? "UDP" : "TCP");
        gfx_text(74, y + 30, 15, NODE_COLOR_DIM, text);
        measurement_label(&copy->measurements[id], text);
        if (group_code[0] && copy->measurements[id].state == NodeMeasureReady && group.participants &&
            group.nodes[id].measured_members == group.participants) {
            if (group.nodes[id].reachable_members < group.participants)
                snprintf(text, sizeof(text), "%s", ui_text("Unstable for a player"));
            else
                snprintf(text, sizeof(text), ui_text("You %u / worst %u ms"),
                         copy->measurements[id].median_ms, group.nodes[id].worst_median_ms);
        } else if (copy->measurements[id].state == NodeMeasureReady &&
                   !node_measurement_recommended(&copy->measurements[id])) {
            const NodeMeasurement *measurement = &copy->measurements[id];
            snprintf(text, sizeof(text), "%u ms · %s · %u/%u", measurement->median_ms,
                     measurement->kind == NodeProbeUdp ? "UDP" : "TCP", measurement->received,
                     measurement->sent);
        }
        gfx_text_fit(910, y + 13, 21, enabled ? NODE_COLOR_TEXT : NODE_COLOR_DIM,
                     enabled ? text : ui_text("Unavailable"), 290);
    }
    if (!count)
        gfx_text(56, 260, 24, NODE_COLOR_DIM, ui_text("No nodes in this region."));
    if (copy->probe_start_failed)
        gfx_text_fit(56, 596, 21, NODE_COLOR_WARN, ui_text("Could not start node tests. Try again."), 1168);
    else if (notice)
        gfx_text_fit(56, 596, 21, NODE_COLOR_WARN, ui_text(notice), 1168);
    else if (group_code[0] && group.reported_participants < group.participants)
        gfx_text_fit(56, 596, 21, NODE_COLOR_DIM, ui_text("Waiting for other players' results."), 1168);
    else if (!copy->catalog_ok)
        gfx_text_fit(56, 596, 21, NODE_COLOR_WARN, ui_text("Could not load node availability. Refresh."),
                     1168);
    else {
        unsigned finished = 0;
        for (unsigned id = 0; id < NODE_CATALOG_COUNT; id++) {
            if (copy->measurements[id].state == NodeMeasureReady ||
                copy->measurements[id].state == NodeMeasureFailed)
                finished++;
        }
        if (finished < NODE_CATALOG_COUNT) {
            snprintf(text, sizeof(text), ui_text("Testing nodes: %u/%u"), finished, NODE_CATALOG_COUNT);
            gfx_text(56, 596, 21, NODE_COLOR_DIM, text);
        }
    }
    gfx_rect(0, 648, 1280, 1, RGBA(255, 255, 255, 28));
    gfx_button(56, 670, "A", ui_text("Select"));
    gfx_button(240, 670, "X", ui_text("Refresh"));
    gfx_button(465, 670, "L", ui_text("Test with friends"));
    gfx_button(980, 670, "B", ui_text("Back"));
    gfx_end();
}
static u64 picker_touch(const uint32_t rendered[NODE_ROWS], uint32_t *touched, bool *held)
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
    if (x >= 1056 && y >= 35 && y < 95)
        return HidNpadButton_StickR;
    if (y >= 165 && y < 210)
        return HidNpadButton_R;
    if (y >= 225 && y < 573) {
        const unsigned index = (unsigned)(y - 225) / 58;
        if (index < NODE_ROWS && rendered[index] != NODE_ID_INVALID) {
            *touched = rendered[index];
            return HidNpadButton_A;
        }
    }
    if (y >= 650) {
        if (x < 220)
            return HidNpadButton_A;
        if (x < 450)
            return HidNpadButton_X;
        if (x < 900)
            return HidNpadButton_L;
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
    gfx_text(56, 205, 24, NODE_COLOR_DIM, ui_text("Share a code, then compare everyone's connection."));
    if (group_code[0]) {
        char text[160];
        snprintf(text, sizeof(text), ui_text("Code %s · %u players"), group_code, group.participants);
        gfx_text(56, 290, 32, NODE_COLOR_TEXT, text);
    }
    if (notice)
        gfx_text_fit(56, 420, 22, NODE_COLOR_WARN, ui_text(notice), 1168);
    gfx_rect(0, 648, 1280, 1, RGBA(255, 255, 255, 28));
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
    bool dirty = true, touch_held = true;
    NodeProbeLoop probe_loop = probe_loop_start();
    while (appletMainLoop()) {
        padUpdate(pad);
        u64 down = padGetButtonsDown(pad);
        int touch_x, touch_y;
        if (modal_touch_start(&touch_held, &touch_x, &touch_y)) {
            if (touch_x >= 1056 && touch_y >= 35 && touch_y < 95)
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
bool nodes_ui_pick(PadState *pad, Service *service, uint32_t current, uint32_t *selected_node)
{
    if (!initialized)
        return false;
    start_probes((1u << NODE_CATALOG_COUNT) - 1, false);
    notice = NULL;
    unsigned selected = 0;
    uint8_t region = NODE_ALL_REGIONS;
    bool dirty = true, moved = false, touch_held = true;
    uint32_t selected_id = current, rendered_selected = NODE_ID_INVALID, rendered[NODE_ROWS];
    for (unsigned i = 0; i < NODE_ROWS; i++)
        rendered[i] = NODE_ID_INVALID;
    NodeProbeLoop probe_loop = probe_loop_start();
    while (appletMainLoop()) {
        collect_probes();
        maintain_probes(&probe_loop, (1u << NODE_CATALOG_COUNT) - 1);
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
        for (unsigned i = 0; i < count; i++)
            if (order[i] == selected_id) {
                selected = i;
                break;
            }
        if (selected >= count)
            selected = count ? count - 1 : 0;
        uint32_t touched = NODE_ID_INVALID;
        down |= picker_touch(rendered, &touched, &touch_held);
        if (touched != NODE_ID_INVALID) {
            selected_id = touched;
            moved = true;
        } else if ((down & HidNpadButton_A) && rendered_selected != NODE_ID_INVALID) {
            selected_id = rendered_selected;
            moved = true;
        } else if (!moved && count && recommended[order[0]])
            selected_id = order[0];
        for (unsigned i = 0; i < count; i++)
            if (order[i] == selected_id) {
                selected = i;
                break;
            }
        language_input(down);
        if (down & HidNpadButton_B) {
            cancel_probes();
            return false;
        }
        if ((down & HidNpadButton_AnyUp) && selected) {
            selected--;
            selected_id = order[selected];
            moved = true;
        }
        if ((down & HidNpadButton_AnyDown) && selected + 1 < count) {
            selected++;
            selected_id = order[selected];
            moved = true;
        }
        if (down & HidNpadButton_R) {
            region = region == NODE_ALL_REGIONS ? 0 : region == 5 ? NODE_ALL_REGIONS : (uint8_t)(region + 1);
            rank_nodes(&copy, region, order, &count, recommended);
            selected = 0;
            selected_id = count ? order[0] : NODE_ID_INVALID;
            rendered_selected = NODE_ID_INVALID;
            moved = false;
            down &= ~HidNpadButton_A;
        }
        if (down & HidNpadButton_X) {
            notice = NULL;
            start_probes((1u << NODE_CATALOG_COUNT) - 1, true);
            continue;
        }
        if (down & HidNpadButton_L) {
            friend_panel(pad);
            dirty = true;
        }
        if ((down & HidNpadButton_A) && count) {
            const uint32_t id = order[selected];
            const NodeMeasurement *m = &copy.measurements[id];
            if (!copy.catalog_ok || !(copy.enabled & (1u << id)) || m->state != NodeMeasureReady) {
                notice = "This node is not ready. Refresh or choose another.";
            } else if (R_SUCCEEDED(serviceDispatchIn(service, 36, id))) {
                *selected_node = id;
                cancel_probes();
                return true;
            } else
                notice = "Could not select this node. Try another.";
        }
        mutexLock(&view_lock);
        dirty |= view.changed;
        view.changed = false;
        mutexUnlock(&view_lock);
        if (dirty || down) {
            draw_picker(&copy, region, order, count, selected, recommended);
            rendered_selected = count ? order[selected] : NODE_ID_INVALID;
            const unsigned first = selected / NODE_ROWS * NODE_ROWS;
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
    gfx_text(
        56, 330, 23, NODE_COLOR_DIM,
        ui_text(gateway ? "Your connection to My server gateway" : "Your connection to this room's node"));
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
    gfx_rect(0, 648, 1280, 1, RGBA(255, 255, 255, 28));
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
            if (touch_x >= 1056 && touch_y >= 35 && touch_y < 95)
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
    SwkbdConfig keyboard;
    if (R_FAILED(swkbdCreate(&keyboard, 0)))
        return false;
    swkbdConfigMakePresetDefault(&keyboard);
    swkbdConfigSetHeaderText(&keyboard, ui_text("Search game"));
    swkbdConfigSetStringLenMax(&keyboard, 20);
    const Result result = swkbdShow(&keyboard, query, 64);
    swkbdClose(&keyboard);
    return R_SUCCEEDED(result);
}
static void draw_filters(const GameCatalog *catalog, const CatalogGameChoice *choices, size_t count,
                         int selected, uint64_t chosen, uint8_t chosen_region)
{
    begin_screen("Filter rooms");
    gfx_button(56, 175, "R", nodes_ui_region(chosen_region));
    const CatalogMatch selected_title = game_catalog_identify(catalog, 0, chosen);
    gfx_text_fit(400, 181, 21, NODE_COLOR_DIM,
                 (chosen && selected_title.kind) ? game_catalog_name(&selected_title, ui_chinese())
                                                 : ui_text("All games"),
                 810);
    gfx_round_rect(56, 225, 1168, 45, 12, selected < 0 ? RGBA(139, 92, 246, 65) : NODE_COLOR_PANEL);
    gfx_text(74, 234, 22, NODE_COLOR_TEXT, ui_text("Apply current filters"));
    for (size_t i = 0; i < count; i++) {
        const int y = 280 + (int)i * 48;
        gfx_round_rect(56, y, 1168, 43, 10, selected == (int)i ? RGBA(139, 92, 246, 65) : NODE_COLOR_PANEL);
        gfx_text_fit(74, y + 8, 22, NODE_COLOR_TEXT, game_catalog_name(&choices[i].title, ui_chinese()),
                     1130);
    }
    gfx_rect(0, 648, 1280, 1, RGBA(255, 255, 255, 28));
    gfx_button(56, 670, "A", ui_text("Apply"));
    gfx_button(250, 670, "X", ui_text("Search game"));
    gfx_button(505, 670, "Y", ui_text("All games"));
    gfx_button(740, 670, "ZR", ui_text("Next"));
    gfx_button(1060, 670, "B", ui_text("Back"));
    gfx_end();
}
bool nodes_ui_filters(PadState *pad, const GameCatalog *catalog, uint64_t *game_id, uint8_t *region)
{
    char query[64] = {0};
    CatalogGameChoice choices[6];
    size_t offset = 0, next = SIZE_MAX;
    int selected = -1;
    bool dirty = true, touch_held = true;
    uint64_t chosen = *game_id;
    uint8_t chosen_region = *region;
    size_t count = game_catalog_choices(catalog, query, offset, choices, 6, &next);
    while (appletMainLoop()) {
        padUpdate(pad);
        u64 down = padGetButtonsDown(pad);
        int touch_x, touch_y;
        if (modal_touch_start(&touch_held, &touch_x, &touch_y)) {
            if (touch_x >= 1056 && touch_y >= 35 && touch_y < 95)
                down |= HidNpadButton_StickR;
            else if (touch_y >= 165 && touch_y < 210 && touch_x < 350)
                down |= HidNpadButton_R;
            else if (touch_y >= 225 && touch_y < 270) {
                selected = -1;
                down |= HidNpadButton_A;
            } else if (touch_y >= 280 && touch_y < 568) {
                int row = (touch_y - 280) / 48;
                if (row < (int)count) {
                    selected = row;
                    down |= HidNpadButton_A;
                }
            } else if (touch_y >= 650) {
                if (touch_x < 230)
                    down |= HidNpadButton_A;
                else if (touch_x < 480)
                    down |= HidNpadButton_X;
                else if (touch_x < 720)
                    down |= HidNpadButton_Y;
                else if (touch_x < 1020)
                    down |= HidNpadButton_ZR;
                else
                    down |= HidNpadButton_B;
            }
        }
        language_input(down);
        if (down & HidNpadButton_B)
            return false;
        if (down & HidNpadButton_R) {
            chosen_region = chosen_region == NODE_ALL_REGIONS ? 0
                            : chosen_region == 5              ? NODE_ALL_REGIONS
                                                              : (uint8_t)(chosen_region + 1);
            dirty = true;
        }
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
        if ((down & HidNpadButton_AnyDown) && selected + 1 < (int)count) {
            selected++;
            dirty = true;
        }
        if ((down & HidNpadButton_AnyUp) && selected >= 0) {
            selected--;
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
            draw_filters(catalog, choices, count, selected, chosen, chosen_region);
            dirty = false;
        }
        svcSleepThread(33000000ULL);
    }
    return false;
}
