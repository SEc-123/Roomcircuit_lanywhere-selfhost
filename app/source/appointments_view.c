/* Appointments keep their own presentation and use the lobby's existing IPC worker. */
#include "appointments_view.h"
#include "roomcircuit_storage.h"
#include "../../common/roomcircuit_paths.h"
#include "appointments_model.h"
#include "gfx.h"
#include "title_inventory.h"
#include "ui_text.h"
#include "update_ui.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

enum { ListScreen, GameScreen, TimeScreen };
enum { AnimateListRow, AnimateRail, AnimateGamePick, AnimateTimeTitle };
enum { ListX = 56, ListY = 174, ListW = 760, RowH = 58, StepH = 69, SideX = 848, FooterY = 648 };
enum { PanelY = 164, PanelBottom = 609, TableX = 365, Right = GFX_W - ListX,
       RailY = PanelY + 58, RailH = 53, RowsY = PanelY + 94, TableRowH = 56,
       FilterTouchBase = 128 };
#define PICK_GAME (1ULL << 59)
#define TIME_FIELD (1ULL << 58)
#define REFRESH_MS 10000
#define SECOND 1000
#define WEEK_SECONDS (7 * 24 * 60 * 60)
#define TEXT RGBA(0xE8, 0xE8, 0xE8, 0xFF)
#define MUTED RGBA(0xAE, 0xAE, 0xAE, 0xFF)
#define PAPER RGBA(0x11, 0x11, 0x11, 0xFF)
#define PANEL RGBA(0x08, 0x08, 0x08, 0xFF)
#define ACCENT RGBA(0xD8, 0xD8, 0xD8, 0xFF)
#define WARN RGBA(0xE5, 0xBA, 0x6E, 0xFF)
#define BG RGBA(0x00, 0x00, 0x00, 0xFF)
#define LINE RGBA(0x36, 0x36, 0x36, 0xFF)
#define SELECTED RGBA(0x26, 0x26, 0x26, 0xFF)

typedef struct { u64 id; bool application; } GameFilter;
typedef struct { GameFilter key; u8 representative; u8 count; } GameGroup;

static UEvent *g_wake;
static atomic_bool g_active;
static Mutex g_lock;
static bool g_pending, g_completed;
typedef struct {
    AppointmentRequest exchange;
    AppointmentWatchRequest subscription;
} PendingRequest;
typedef struct {
    AppointmentResponse exchange;
    AppointmentSubscriptions subscriptions;
    u32 subscription_status, alert_status;
    bool subscriptions_ok;
} PendingReply;

static PendingRequest g_request;
static PendingReply g_reply;
static unsigned g_epoch, g_request_epoch;

/* These values belong to the UI thread. Only the request/reply cross the lock. */
static bool g_worker, g_busy, g_list_failed;
static AppointmentSubscriptions g_subscriptions;
static bool g_subscriptions_ok;
static u32 g_alert_status = UINT32_MAX;
static AppointmentResponse g_page;
static u32 g_offset;
static u64 g_selected, g_touched, g_rendered[APPOINTMENT_PAGE_SIZE];
static bool g_filter_focus;
static GameFilter g_filter, g_touched_filter, g_rendered_filters[APPOINTMENT_PAGE_SIZE + 1];
static size_t g_rendered_filter_count;
static bool g_selection_expired;
static u64 g_clock, g_clock_anchor, g_next_refresh, g_tick_now;
static const char *g_notice;
static int g_screen;
static GameCacheRecord g_games[APPOINTMENT_PAGE_SIZE];
static int g_game_count, g_game_selected;
static u32 g_game_offset;
static bool g_games_loaded, g_games_more;
static GameCacheRecord g_game;
static int g_day, g_hour, g_minute, g_field;
static AppointmentDate g_today;
static u64 g_repeat_next, g_repeat_key;
static bool g_title_animates, g_animation_initialized, g_animation_chinese;
static unsigned g_animation_field;
static GameFilter g_animation_key;
static u64 g_animation_start;

static GameFilter row_game(const AppointmentRow *row)
{
    return row->app_id ? (GameFilter){row->app_id, true} : (GameFilter){row->game_id, false};
}

static bool same_game(GameFilter left, GameFilter right)
{
    return left.id == right.id && left.application == right.application;
}

static size_t appointment_games(GameGroup groups[APPOINTMENT_PAGE_SIZE + 1])
{
    groups[0] = (GameGroup){.count = (u8)g_page.count};
    size_t count = 1;
    for (size_t i = 0; i < g_page.count; i++) {
        const GameFilter key = row_game(&g_page.rows[i]);
        size_t pos = 1;
        while (pos < count && !same_game(key, groups[pos].key)) pos++;
        if (pos == count) groups[count++] = (GameGroup){.key = key, .representative = (u8)i};
        groups[pos].count++;
    }
    return count;
}

static int game_index(const GameGroup *groups, size_t count, GameFilter key)
{
    for (size_t i = 0; i < count; i++)
        if (same_game(groups[i].key, key)) return (int)i;
    return -1;
}

static size_t visible_rows(u8 rows[APPOINTMENT_PAGE_SIZE])
{
    size_t count = 0;
    for (size_t i = 0; i < g_page.count; i++)
        if (!g_filter.id || same_game(g_filter, row_game(&g_page.rows[i]))) rows[count++] = (u8)i;
    return count;
}

static int selected_row(const u8 *rows, size_t count)
{
    for (size_t i = 0; i < count; i++)
        if (g_page.rows[rows[i]].id == g_selected) return (int)i;
    return count ? 0 : -1;
}

void appointments_init(UEvent *wake)
{
    g_wake = wake;
    mutexInit(&g_lock);
}

bool appointments_active(void)
{
    return atomic_load(&g_active);
}

bool appointments_title_animates(void)
{
    return g_title_animates && appointments_active();
}

bool appointments_list_active(void)
{
    return appointments_active() && g_screen == ListScreen;
}

static void awake(void)
{
    if (g_wake) ueventSignal(g_wake);
}

void appointments_open(bool worker_available, bool resume)
{
    g_worker = worker_available;
    g_epoch++;
    g_screen = ListScreen;
    g_notice = NULL;
    g_list_failed = false;
    g_next_refresh = 0;
    g_selection_expired = false;
    g_filter_focus = false;
    g_animation_initialized = false;
    g_title_animates = false;
    if (!resume) {
        g_offset = 0;
        g_selected = 0;
        g_filter = (GameFilter){0};
        g_clock = 0;
        g_page = (AppointmentResponse){.next = APPOINTMENT_END};
        g_subscriptions = (AppointmentSubscriptions){0};
        g_subscriptions_ok = false;
        g_alert_status = UINT32_MAX;
    }
    atomic_store(&g_active, true);
    awake();
}

void appointments_close(void)
{
    g_epoch++;
    atomic_store(&g_active, false);
    awake();
}

static void submit(PendingRequest data)
{
    if (g_busy || !g_worker) return;
    mutexLock(&g_lock);
    g_request = data;
    g_request_epoch = g_epoch;
    g_pending = true;
    mutexUnlock(&g_lock);
    g_busy = true;
    awake();
}

static void request(AppointmentRequest data)
{
    submit((PendingRequest){.exchange = data});
}

static bool subscriptions_valid(const AppointmentSubscriptions *subscriptions)
{
    if (subscriptions->count > APPOINTMENT_SUBSCRIPTION_LIMIT || subscriptions->reserved) return false;
    for (size_t i = 0; i < subscriptions->count; i++) {
        if (!subscriptions->ids[i]) return false;
        for (size_t j = 0; j < i; j++)
            if (subscriptions->ids[j] == subscriptions->ids[i]) return false;
    }
    return true;
}

static bool joined(u64 id)
{
    if (!g_subscriptions_ok) return false;
    for (size_t i = 0; i < g_subscriptions.count; i++)
        if (g_subscriptions.ids[i] == id) return true;
    return false;
}

bool appointments_worker_exchange(Service *service)
{
    PendingRequest req;
    mutexLock(&g_lock);
    bool pending = g_pending;
    if (pending) {
        req = g_request;
        g_pending = false;
    }
    mutexUnlock(&g_lock);
    if (!pending) return false;

    PendingReply reply = {.exchange = {.status = AppointmentUnavailable, .next = APPOINTMENT_END}};
    if (req.subscription.id) {
        u32 status = AppointmentUnavailable;
        Result rc = serviceDispatchInOut(service, APPOINTMENT_IPC_SUBSCRIBE, req.subscription, status);
        reply.subscription_status = R_SUCCEEDED(rc) ? status : AppointmentUnavailable;
    } else {
        Result rc = serviceDispatch(service, APPOINTMENT_IPC_EXCHANGE,
            .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In,
                              SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
            .buffers = { { &req.exchange, sizeof(req.exchange) }, { &reply.exchange, sizeof(reply.exchange) } });
        u32 offset = req.exchange.op == AppointmentList ? req.exchange.offset : 0;
        if (R_FAILED(rc) || !appointment_response_valid(&reply.exchange, offset))
            reply.exchange = (AppointmentResponse){.status = AppointmentUnavailable, .next = APPOINTMENT_END};
    }
    const Result subscriptions_result = serviceDispatch(service, APPOINTMENT_IPC_SUBSCRIPTIONS,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { &reply.subscriptions, sizeof(reply.subscriptions) } });
    reply.subscriptions_ok = R_SUCCEEDED(subscriptions_result) && subscriptions_valid(&reply.subscriptions);
    u32 alert_status = UINT32_MAX;
    const Result alert_result = serviceDispatchOut(service, APPOINTMENT_IPC_ALERT_STATE, alert_status);
    reply.alert_status = R_SUCCEEDED(alert_result) && alert_status <= AppointmentAlertLegacy ? alert_status : UINT32_MAX;
    mutexLock(&g_lock);
    g_reply = reply;
    g_completed = true;
    mutexUnlock(&g_lock);
    return true;
}

static const char *status_text(u32 status)
{
    switch (status) {
    case AppointmentInvalid: return "Check the game and appointment time.";
    case AppointmentLimit: return "Appointment limit reached. Try again later.";
    case AppointmentNotFound: return "This appointment has ended.";
    case AppointmentStorage: return "The server could not save this change.";
    default: return "Appointments are unavailable. Try again.";
    }
}

static u64 server_now(void)
{
    return g_clock && g_tick_now >= g_clock_anchor ? g_clock + (g_tick_now - g_clock_anchor) / SECOND : g_clock;
}

static bool hide_expired_rows(u64 now)
{
    u32 count = 0;
    for (u32 i = 0; i < g_page.count; i++) {
        const AppointmentRow *row = &g_page.rows[i];
        if (appointment_expired(row->starts_at, now)) {
            if (row->id == g_selected) g_selection_expired = true;
        } else {
            g_page.rows[count++] = *row;
        }
    }
    if (count == g_page.count) return false;
    g_page.count = count;
    if (appointment_find_row(&g_page, g_selected) < 0)
        g_selected = count ? g_page.rows[0].id : 0;
    return true;
}

bool appointments_tick(u64 monotonic_ms)
{
    g_tick_now = monotonic_ms;
    bool dirty = false;
    PendingReply result;
    PendingRequest operation;
    unsigned epoch = 0;
    mutexLock(&g_lock);
    bool complete = g_completed;
    if (complete) {
        result = g_reply;
        operation = g_request;
        epoch = g_request_epoch;
        g_completed = false;
    }
    mutexUnlock(&g_lock);
    if (complete) {
        g_busy = false;
        if (epoch == g_epoch && appointments_active()) {
            g_subscriptions_ok = result.subscriptions_ok;
            g_alert_status = result.alert_status;
            if (result.subscriptions_ok) g_subscriptions = result.subscriptions;
            if (operation.subscription.id) {
                if (result.subscription_status == AppointmentOk)
                    g_notice = operation.subscription.enabled ? "Appointment joined." : "Appointment left.";
                else if (result.subscription_status == AppointmentLimit)
                    g_notice = "You can join up to 24 appointments.";
                else if (result.subscription_status == AppointmentStorage)
                    g_notice = "Could not save joined appointments.";
                else
                    g_notice = status_text(result.subscription_status);
            } else {
                AppointmentResponse reply = result.exchange;
                AppointmentRequest req = operation.exchange;
                if (req.op == AppointmentList) g_list_failed = reply.status != AppointmentOk;
                if (reply.now) {
                    const u64 estimated_now = server_now();
                    g_clock = reply.now > estimated_now ? reply.now : estimated_now;
                    g_clock_anchor = monotonic_ms;
                }
                if (reply.status == AppointmentOk) {
                    /* Discard any controller action aimed at a row that just expired. */
                    hide_expired_rows(server_now());
                    if (req.op == AppointmentCreate || (req.op == AppointmentList && req.offset != g_offset)) {
                        g_filter = (GameFilter){0};
                        g_filter_focus = false;
                    }
                    g_page = reply;
                    g_offset = req.op == AppointmentList ? req.offset : 0;
                    if (req.op == AppointmentCreate) {
                        g_screen = ListScreen;
                        for (size_t i = 0; i < g_page.count; i++) {
                            const AppointmentRow *r = &g_page.rows[i];
                            if (r->game_id == req.game_id && r->app_id == req.app_id &&
                                r->starts_at == req.starts_at) {
                                g_selected = r->id;
                                break;
                            }
                        }
                    }
                    if (appointment_find_row(&g_page, g_selected) < 0)
                        g_selected = g_page.count ? g_page.rows[0].id : 0;
                    g_notice = NULL;
                    if (req.op == AppointmentCreate) g_notice = "Appointment created.";
                } else {
                    g_notice = status_text(reply.status);
                }
                g_next_refresh = monotonic_ms + REFRESH_MS;
            }
            dirty = true;
        }
    }
    if (!appointments_active()) return dirty;
    if (hide_expired_rows(server_now())) dirty = true;
    if (!g_worker) {
        if (!g_notice) { g_notice = status_text(AppointmentUnavailable); return true; }
        return dirty;
    }
    if (g_screen == ListScreen && !g_busy && monotonic_ms >= g_next_refresh) {
        if (g_offset && g_page.count == 0) g_offset = 0;
        request((AppointmentRequest){.op = AppointmentList, .offset = g_offset});
        dirty = true;
    }
    return dirty;
}

static void load_games(bool loading, bool failed)
{
    g_games_loaded = !loading;
    g_game_count = 0;
    g_game_selected = 0;
    g_games_more = false;
    if (loading) {
        g_notice = "Reading game titles…";
        return;
    }
    if (!failed)
        g_game_count = title_inventory_appointments_at(rc_storage_path_gen(roomcircuit_storage_generation(), GAME_CACHE_PATH, RC_V1_PATH("game-titles.bin"), RC_CURRENT_PATH("game-titles.bin")), g_game_offset,
            g_games, APPOINTMENT_PAGE_SIZE, &g_games_more);
    if (failed || g_game_count < 0) g_notice = "Could not read game titles. Reopen the app.";
    else g_notice = g_game_count ? NULL : "No installed games with Local Wireless found.";
}

static bool begin_time(const GameCacheRecord *game)
{
    TimeCalendarTime now, soon;
    TimeCalendarAdditionalInfo info;
    const u64 utc = server_now();
    if (!utc || R_FAILED(timeToCalendarTimeWithMyRule(utc, &now, &info)) ||
        R_FAILED(timeToCalendarTimeWithMyRule((utc / 60 + 61) * 60, &soon, &info))) {
        g_notice = "Could not read your local time.";
        return false;
    }
    g_today = (AppointmentDate){now.year, now.month, now.day};
    g_game = *game;
    g_day = now.day != soon.day;
    g_hour = soon.hour;
    g_minute = soon.minute;
    g_field = 1;
    g_repeat_key = 0;
    g_screen = TimeScreen;
    g_notice = NULL;
    return true;
}

static void create_appointment(void)
{
    AppointmentDate day = appointment_date_after(g_today, (unsigned)g_day);
    TimeCalendarTime local = {.year = day.year, .month = day.month, .day = day.day,
                              .hour = g_hour, .minute = g_minute};
    u64 utc[2] = {0};
    s32 count = 0;
    if (R_FAILED(timeToPosixTimeWithMyRule(&local, utc, 2, &count)) || count < 1 || count > 2) {
        g_notice = "This local time does not exist. Choose another minute.";
        return;
    }
    const u64 now = server_now();
    u64 first = 0;
    for (s32 i = 0; i < count; i++)
        if (utc[i] > now && (!first || utc[i] < first)) first = utc[i];
    if (!first || !now || first - now > WEEK_SECONDS) {
        g_notice = "Choose a future time within seven days.";
        return;
    }
    u64 game_id = 0;
    for (size_t i = 0; i < GAME_CACHE_LCID_COUNT; i++)
        if (g_game.local_communication_ids[i] && g_game.local_communication_ids[i] != UINT64_MAX) {
            game_id = g_game.local_communication_ids[i];
            break;
        }
    if (!game_id) return;
    request((AppointmentRequest){.op = AppointmentCreate, .starts_at = first,
                                 .game_id = game_id, .app_id = g_game.application_id});
}

void appointments_capture_touch(u64 action)
{
    size_t index = action & 0xff;
    if (!(action & (APPOINTMENT_TOUCH_ROW | APPOINTMENT_TOUCH_JOIN))) return;
    if (index < APPOINTMENT_PAGE_SIZE) g_touched = g_rendered[index];
    else if (index >= FilterTouchBase && index - FilterTouchBase < g_rendered_filter_count)
        g_touched_filter = g_rendered_filters[index - FilterTouchBase];
}

static void adjust_time(int delta)
{
    if (g_field == 0) g_day = (g_day + delta + 8) % 8;
    else if (g_field == 1) g_hour = (g_hour + delta + 24) % 24;
    else g_minute = (g_minute + delta + 60) % 60;
    g_notice = NULL;
}

AppointmentAction appointments_input(u64 down, u64 held, u64 monotonic_ms,
                                     bool inventory_loading, bool inventory_failed, u64 *game_id)
{
    /* Touch payloads use the low byte for row or field indices. */
    const bool synthetic = (down & (APPOINTMENT_TOUCH_ROW | APPOINTMENT_TOUCH_JOIN | PICK_GAME | TIME_FIELD)) != 0;
    u64 buttons = synthetic ? down & ~(u64)0xff : down;
    if (buttons & HidNpadButton_B) {
        if (g_busy && g_screen == TimeScreen) return AppointmentStay;
        if (g_screen == TimeScreen) g_screen = GameScreen;
        else if (g_screen == GameScreen) g_screen = ListScreen;
        else return AppointmentBack;
        g_notice = NULL;
        return AppointmentStay;
    }
    if (g_screen == GameScreen) {
        if (!g_games_loaded && !inventory_loading) load_games(false, inventory_failed);
        if (buttons & HidNpadButton_ZL && g_game_offset) {
            g_game_offset -= APPOINTMENT_PAGE_SIZE;
            load_games(inventory_loading, inventory_failed);
        }
        if (buttons & HidNpadButton_ZR && g_games_more) {
            g_game_offset += APPOINTMENT_PAGE_SIZE;
            load_games(inventory_loading, inventory_failed);
        }
        if (g_game_count <= 0) return AppointmentStay;
        if (buttons & HidNpadButton_AnyDown && g_game_selected + 1 < g_game_count) g_game_selected++;
        if (buttons & HidNpadButton_AnyUp && g_game_selected > 0) g_game_selected--;
        if ((down & PICK_GAME) && (int)(down & 0xff) < g_game_count) {
            g_game_selected = (int)(down & 0xff);
            buttons |= HidNpadButton_A;
        }
        if (buttons & HidNpadButton_A) begin_time(&g_games[g_game_selected]);
        return AppointmentStay;
    }
    if (g_screen == TimeScreen) {
        if (g_busy) return AppointmentStay;
        if (buttons & HidNpadButton_AnyLeft) g_field = (g_field + 2) % 3;
        if (buttons & HidNpadButton_AnyRight) g_field = (g_field + 1) % 3;
        if ((down & TIME_FIELD) && (down & 3) < 3) {
            g_field = (int)(down & 3);
            adjust_time(down & 4 ? -1 : 1);
        }
        u64 direction = held & (HidNpadButton_AnyUp | HidNpadButton_AnyDown);
        if (!direction) g_repeat_key = 0;
        else if (direction != g_repeat_key || monotonic_ms >= g_repeat_next) {
            adjust_time(direction & HidNpadButton_AnyUp ? 1 : -1);
            g_repeat_next = monotonic_ms + (direction == g_repeat_key ? 80 : 350);
            g_repeat_key = direction;
        }
        if (buttons & HidNpadButton_A) create_appointment();
        return AppointmentStay;
    }
    if (buttons & HidNpadButton_R && !g_busy) {
        g_next_refresh = 0;
        request((AppointmentRequest){.op = AppointmentList, .offset = g_offset});
        return AppointmentStay;
    }
    if (g_busy || !server_now()) return AppointmentStay;
    if (buttons & HidNpadButton_L) {
        g_screen = GameScreen;
        g_game_offset = 0;
        load_games(inventory_loading, inventory_failed);
        return AppointmentStay;
    }
    if (buttons & HidNpadButton_ZL && g_offset) {
        request((AppointmentRequest){.op = AppointmentList, .offset = g_offset - APPOINTMENT_PAGE_SIZE});
        return AppointmentStay;
    }
    if (buttons & HidNpadButton_ZR && g_page.next != APPOINTMENT_END) {
        request((AppointmentRequest){.op = AppointmentList, .offset = g_page.next});
        return AppointmentStay;
    }
    if (!g_page.count) return AppointmentStay;
    GameGroup groups[APPOINTMENT_PAGE_SIZE + 1];
    const size_t group_count = appointment_games(groups);
    int group = game_index(groups, group_count, g_filter);
    if (group < 0) { g_filter = (GameFilter){0}; group = 0; }
    const bool filter_tap = (down & APPOINTMENT_TOUCH_ROW) && (down & 0xff) >= FilterTouchBase;
    if (filter_tap) {
        if (game_index(groups, group_count, g_touched_filter) >= 0) {
            g_filter = g_touched_filter;
            g_filter_focus = false;
        }
        return AppointmentStay;
    }
    if (buttons & HidNpadButton_AnyLeft) {
        g_filter_focus = !g_filter_focus;
        return AppointmentStay;
    }
    if (g_filter_focus && !(down & (APPOINTMENT_TOUCH_ROW | APPOINTMENT_TOUCH_JOIN))) {
        if ((buttons & HidNpadButton_AnyDown) && group + 1 < (int)group_count) group++;
        if ((buttons & HidNpadButton_AnyUp) && group > 0) group--;
        g_filter = groups[group].key;
        if (buttons & HidNpadButton_A) g_filter_focus = false;
        return AppointmentStay;
    }
    if (g_selection_expired && !(down & (APPOINTMENT_TOUCH_ROW | APPOINTMENT_TOUCH_JOIN)))
        return AppointmentStay;
    u8 rows[APPOINTMENT_PAGE_SIZE];
    const size_t count = visible_rows(rows);
    int selected = selected_row(rows, count);
    if (selected < 0) return AppointmentStay;
    if (buttons & HidNpadButton_AnyDown && selected + 1 < (int)count) selected++;
    if (buttons & HidNpadButton_AnyUp && selected > 0) selected--;
    const bool join_tap = (down & APPOINTMENT_TOUCH_JOIN) != 0;
    if (down & (APPOINTMENT_TOUCH_ROW | APPOINTMENT_TOUCH_JOIN)) {
        const int tapped = appointment_find_row(&g_page, g_touched);
        if (tapped < 0 || (g_filter.id && !same_game(g_filter, row_game(&g_page.rows[tapped]))))
            return AppointmentStay;
        for (size_t i = 0; i < count; i++) if (rows[i] == tapped) { selected = (int)i; break; }
        g_filter_focus = false;
    }
    const AppointmentRow *row = &g_page.rows[rows[selected]];
    if (appointment_expired(row->starts_at, server_now())) return AppointmentStay;
    g_selected = row->id;
    if (join_tap || (buttons & HidNpadButton_A)) {
        const bool is_joined = joined(row->id);
        if (g_subscriptions_ok && (!join_tap || is_joined == ((down & APPOINTMENT_TOUCH_JOINED) != 0))) {
            submit((PendingRequest){.subscription = {
                .id = row->id, .starts_at = row->starts_at, .game_id = row->game_id,
                .app_id = row->app_id, .enabled = !is_joined}});
        }
    } else if (buttons & HidNpadButton_X && appointment_rooms_open(row->starts_at, server_now())) {
        *game_id = row->game_id;
        return AppointmentFindRooms;
    } else if ((buttons & HidNpadButton_Y) && (row->followed ||
              appointment_rooms_open(row->starts_at, server_now()))) {
        request((AppointmentRequest){.op = row->followed ? AppointmentUnfollow : AppointmentFollow,
                                     .id = row->id});
    }
    return AppointmentStay;
}

static int action(int x, int y, const char *key, const char *label, u64 button, AppointmentTouchZone touch)
{
    const int width = gfx_button(x, y, key, ui_text(label));
    const int end = x + width + 7 < Right ? x + width + 7 : Right;
    touch(x - 6, y - 7, end - x + 6, 46, button);
    return x + width + 20;
}

static int action_width(const char *key, const char *label)
{
    const int pill = gfx_text_width(18, key) + 14;
    return (pill > 30 ? pill : 30) + 10 + gfx_text_width(22, ui_text(label));
}

static void action_right(int end, int y, const char *key, const char *label, u64 button, AppointmentTouchZone touch)
{
    action(end - action_width(key, label), y, key, label, button, touch);
}

static void row_surface(int x, int y, int width, int height, bool selected, bool focused)
{
    if (focused) {
        gfx_round_rect(x, y, width, height, 7, ACCENT);
        gfx_round_rect(x + 2, y + 2, width - 4, height - 4, 5, SELECTED);
    } else if (selected) {
        gfx_round_rect(x, y, width, height, 7, SELECTED);
    }
}

static void draw_game_title(int x, int y, int size, u32 color, const char *name, int width,
                            bool focused, unsigned field, GameFilter key)
{
    if (!focused) {
        gfx_text_fit(x, y, size, color, name, width);
        return;
    }
    const bool chinese = ui_chinese();
    if (!g_animation_initialized || g_animation_field != field || !same_game(g_animation_key, key) ||
        g_animation_chinese != chinese || g_tick_now < g_animation_start) {
        g_animation_initialized = true;
        g_animation_field = field;
        g_animation_key = key;
        g_animation_chinese = chinese;
        g_animation_start = g_tick_now;
    }
    g_title_animates = gfx_text_marquee(x, y, size, color, name, width, g_tick_now - g_animation_start);
}

static const char *appointment_alert_text(u32 status)
{
    switch (status) {
    case AppointmentAlertDisabled: return "Start Ultrahand with API notifications.";
    case AppointmentAlertWaiting: return "Room alerts waiting.";
    case AppointmentAlertConnected: return "Room alerts connected.";
    case AppointmentAlertLegacy: return "Room alerts using periodic checks.";
    default: return NULL;
    }
}

static const char *appointment_game_name(const GameCatalog *catalog, const AppointmentRow *row, char *fallback, size_t capacity)
{
    const CatalogMatch game = game_catalog_identify(catalog, row->app_id, row->game_id);
    const char *name = game_catalog_name(&game, ui_chinese());
    if (name) return name;
    snprintf(fallback, capacity, ui_text("Game %016lX"), (unsigned long)row->game_id);
    return fallback;
}

static const char *group_name(const GameCatalog *catalog, const GameGroup *group, char *fallback, size_t capacity)
{
    if (!group->key.id) return ui_text("All games");
    return appointment_game_name(catalog, &g_page.rows[group->representative], fallback, capacity);
}

static void draw_appointment_row(const GameCatalog *catalog, const AppointmentRow *row, size_t index,
                                 bool focused, AppointmentTouchZone touch)
{
    enum { GameX = TableX + 18, TimeX = 644, CountX = 871, StatusX = 960, JoinX = 1102, JoinW = 105 };
    const int y = RowsY + (int)index * TableRowH;
    row_surface(TableX + 6, y + 1, Right - TableX - 12, TableRowH - 2, focused, focused);
    char fallback[64], when[64];
    const char *name = appointment_game_name(catalog, row, fallback, sizeof(fallback));
    draw_game_title(GameX, y + 16, 22, TEXT, name, TimeX - GameX - 14,
        focused, AnimateListRow, (GameFilter){row->id, false});
    TimeCalendarTime local;
    TimeCalendarAdditionalInfo info;
    if (R_SUCCEEDED(timeToCalendarTimeWithMyRule(row->starts_at, &local, &info)))
        snprintf(when, sizeof(when), "%04u-%02u-%02u  %02u:%02u",
                 local.year, local.month, local.day, local.hour, local.minute);
    else
        snprintf(when, sizeof(when), "%s", ui_text("Local time unavailable"));
    gfx_text_fit(TimeX, y + 16, 21, TEXT, when, CountX - TimeX - 14);
    char count[24];
    snprintf(count, sizeof(count), "%u", row->interested);
    gfx_text(CountX + (72 - gfx_text_width(21, count)) / 2, y + 16, 21, TEXT, count);
    if (row->followed)
        gfx_text_fit(StatusX, y + 16, 19, ACCENT, ui_text("Ready to play"), JoinX - StatusX - 9);
    gfx_rect(TableX + 12, y + TableRowH - 1, Right - TableX - 24, 1, LINE);

    g_rendered[index] = row->id;
    touch(TableX + 5, y, Right - TableX - 10, TableRowH, APPOINTMENT_TOUCH_ROW | (u64)index);
    if (!g_subscriptions_ok) return;
    const bool is_joined = joined(row->id);
    const char *label = ui_text(is_joined ? "Joined" : "Join");
    gfx_round_rect(JoinX, y + 9, JoinW, 38, 7, is_joined ? SELECTED : PANEL);
    gfx_text(JoinX + (JoinW - gfx_text_width(18, label)) / 2, y + 19, 18, is_joined ? ACCENT : TEXT, label);
    if (!g_busy)
        touch(JoinX - 4, y + 4, JoinW + 8, TableRowH - 8, APPOINTMENT_TOUCH_JOIN | (u64)index |
            (is_joined ? APPOINTMENT_TOUCH_JOINED : 0));
}

static void draw_list(const GameCatalog *catalog, AppointmentTouchZone touch)
{
    g_selection_expired = false;
    memset(g_rendered, 0, sizeof(g_rendered));
    GameGroup groups[APPOINTMENT_PAGE_SIZE + 1];
    const size_t group_count = appointment_games(groups);
    int selected_game = game_index(groups, group_count, g_filter);
    if (selected_game < 0) { g_filter = (GameFilter){0}; selected_game = 0; }
    u8 rows[APPOINTMENT_PAGE_SIZE];
    const size_t count = visible_rows(rows);
    const int selected = selected_row(rows, count);
    if (selected >= 0) g_selected = g_page.rows[rows[selected]].id;

    gfx_round_rect(ListX, PanelY, Right - ListX, PanelBottom - PanelY, 11, LINE);
    gfx_round_rect(ListX + 1, PanelY + 1, Right - ListX - 2, PanelBottom - PanelY - 2, 10, PAPER);
    gfx_rect(TableX, PanelY + 1, 1, PanelBottom - PanelY - 2, LINE);
    gfx_rect(ListX + 1, PanelY + 53, Right - ListX - 2, 1, LINE);
    gfx_text_fit(ListX + 17, PanelY + 14, 23, TEXT,
        ui_text(g_offset || g_page.next != APPOINTMENT_END ? "Games on this page" : "Games"), TableX - ListX - 32);
    char fallback[72];
    gfx_text_fit(TableX + 18, PanelY + 13, 24, TEXT, group_name(catalog, &groups[selected_game], fallback, sizeof(fallback)),
        Right - TableX - 36);
    g_rendered_filter_count = group_count;
    for (size_t i = 0; i < group_count; i++) {
        const int y = RailY + (int)i * RailH;
        const bool active = (int)i == selected_game;
        row_surface(ListX + 7, y + 2, TableX - ListX - 14, RailH - 5, active, active && g_filter_focus);
        draw_game_title(ListX + 17, y + 14, 21, active ? ACCENT : TEXT,
            group_name(catalog, &groups[i], fallback, sizeof(fallback)), TableX - ListX - 61,
            active && g_filter_focus, AnimateRail, groups[i].key);
        char number[12];
        snprintf(number, sizeof(number), "%u", groups[i].count);
        gfx_text(TableX - 20 - gfx_text_width(18, number), y + 17, 18, MUTED, number);
        g_rendered_filters[i] = groups[i].key;
        if (!g_busy)
            touch(ListX + 6, y + 1, TableX - ListX - 12, RailH - 2,
                  APPOINTMENT_TOUCH_ROW | (u64)(FilterTouchBase + i));
    }

    gfx_rect(TableX + 1, PanelY + 54, Right - TableX - 2, RowsY - PanelY - 54, PANEL);
    gfx_text(TableX + 18, PanelY + 62, 17, MUTED, ui_text("Game"));
    gfx_text(644, PanelY + 62, 17, MUTED, ui_text("Local time"));
    const char *ready = ui_text("Ready");
    gfx_text(871 + (72 - gfx_text_width(17, ready)) / 2, PanelY + 62, 17, MUTED, ready);
    gfx_text(960, PanelY + 62, 17, MUTED, ui_text("Status"));
    gfx_rect(TableX + 1, RowsY - 1, Right - TableX - 2, 1, LINE);
    for (size_t i = 0; i < count; i++)
        draw_appointment_row(catalog, &g_page.rows[rows[i]], i, !g_filter_focus && (int)i == selected, touch);
    if (!g_page.count) {
        const char *description = g_busy ? "Loading appointments…" :
            (g_notice ? "Appointments are unavailable." : "No upcoming appointments");
        gfx_text(TableX + 18, RowsY + 22, 24, MUTED, ui_text(description));
    }

    if (g_offset)
        action(519, 107, "ZL", "Prev", HidNpadButton_ZL, touch);
    if (g_page.next != APPOINTMENT_END)
        action(696, 107, "ZR", "Next", HidNpadButton_ZR, touch);
    if (server_now() && !g_busy)
        action_right(Right - 6, 107, "L", "New appointment", HidNpadButton_L, touch);

    int footer = ListX;
    if (count && !g_busy) {
        if (g_filter_focus) {
            action(footer, FooterY + 20, "A", "Select", HidNpadButton_A, touch);
        } else {
            const AppointmentRow *row = &g_page.rows[rows[selected]];
            const bool near_start = appointment_rooms_open(row->starts_at, server_now());
            if (g_subscriptions_ok)
                footer = action(footer, FooterY + 20, "A", joined(row->id) ? "Leave appointment" : "Join appointment",
                    HidNpadButton_A, touch);
            if (near_start)
                footer = action(footer, FooterY + 20, "X", "Find rooms", HidNpadButton_X, touch);
            if (near_start || row->followed)
                action(footer, FooterY + 20, "Y", row->followed ? "Cancel ready" : "Ready now", HidNpadButton_Y, touch);
        }
    }
    if (g_worker && !g_busy && g_list_failed)
        action_right(Right - 6, FooterY + 20, "R", "Refresh", HidNpadButton_R, touch);

    const AppointmentRow *active = selected >= 0 ? &g_page.rows[rows[selected]] : NULL;
    const bool watching = (g_subscriptions_ok && g_subscriptions.count) || (active && active->followed);
    const char *alert = watching ? appointment_alert_text(g_alert_status) : NULL;
    if (!g_notice) {
        if (active && !g_filter_focus && !g_busy && !g_subscriptions_ok)
            gfx_text_fit(ListX, 619, 18, WARN, ui_text("Joined appointments unavailable."), SideX - ListX - 12);
    }
    if (alert)
        gfx_text_fit(SideX, 619, 18, MUTED, ui_text(alert), Right - SideX);
}

static void draw_games(AppointmentTouchZone touch)
{
    gfx_round_rect(ListX, PanelY, Right - ListX, PanelBottom - PanelY, 11, LINE);
    gfx_round_rect(ListX + 1, PanelY + 1, Right - ListX - 2, PanelBottom - PanelY - 2, 10, PAPER);
    for (int i = 0; i < g_game_count; i++) {
        const int y = ListY + i * StepH;
        row_surface(ListX + 11, y, Right - ListX - 22, RowH, i == g_game_selected, i == g_game_selected);
        const char *name = ui_chinese() ? g_games[i].names.chinese : g_games[i].names.english;
        draw_game_title(ListX + 24, y + 16, 24, TEXT, name, Right - ListX - 48,
            i == g_game_selected, AnimateGamePick, (GameFilter){g_games[i].application_id, true});
        if (i + 1 != g_game_count) gfx_rect(ListX + 24, y + RowH + 3, Right - ListX - 48, 1, LINE);
        touch(ListX + 10, y, Right - ListX - 20, RowH, PICK_GAME | (u64)i);
    }
    if (g_game_count > 0)
        action_right(Right - 6, FooterY + 20, "A", "Select game", HidNpadButton_A, touch);
    if (g_game_offset)
        action(780, 107, "ZL", "Prev", HidNpadButton_ZL, touch);
    if (g_games_more)
        action_right(Right - 6, 107, "ZR", "Next", HidNpadButton_ZR, touch);
}

static void draw_time(AppointmentTouchZone touch)
{
    const char *name = ui_chinese() ? g_game.names.chinese : g_game.names.english;
    gfx_round_rect(ListX, PanelY, Right - ListX, 241, 11, LINE);
    gfx_round_rect(ListX + 1, PanelY + 1, Right - ListX - 2, 239, 10, PAPER);
    draw_game_title(ListX + 24, PanelY + 19, 26, TEXT, name, Right - ListX - 48,
        true, AnimateTimeTitle, (GameFilter){g_game.application_id, true});
    AppointmentDate day = appointment_date_after(g_today, (unsigned)g_day);
    char value[3][32];
    snprintf(value[0], sizeof(value[0]), "%04d-%02d-%02d", day.year, day.month, day.day);
    snprintf(value[1], sizeof(value[1]), "%02d", g_hour);
    snprintf(value[2], sizeof(value[2]), "%02d", g_minute);
    const char *labels[] = {"Date", "Hour", "Minute"};
    const int width = 339;
    const int y = PanelY + 111;
    for (int i = 0, x = ListX + 24; i < 3; i++, x += width + 22) {
        gfx_text(x + 3, PanelY + 76, 21, MUTED, ui_text(labels[i]));
        gfx_round_rect(x, y, width, 97, 9, i == g_field ? ACCENT : LINE);
        gfx_round_rect(x + 2, y + 2, width - 4, 93, 7, i == g_field ? SELECTED : PANEL);
        gfx_text(x + 16, y + 29, 32, TEXT, value[i]);
        gfx_text(x + width - 37, y + 8, 20, ACCENT, "▲");
        gfx_text(x + width - 37, y + 61, 20, ACCENT, "▼");
        touch(x, y, width, 49, TIME_FIELD | (u64)i);
        touch(x, y + 49, width, 48, TIME_FIELD | (u64)i | 4);
    }
    if (!g_busy) action_right(Right - 6, FooterY + 20, "A", "Create appointment", HidNpadButton_A, touch);
}

void appointments_draw(const GameCatalog *catalog, AppointmentTouchZone touch)
{
    g_title_animates = false;
    gfx_rect(0, 0, GFX_W, GFX_H, BG);
    gfx_rect(0, 0, GFX_W, 83, PAPER);
    gfx_rect(0, 82, GFX_W, 1, LINE);
    gfx_round_rect(ListX, 30, 19, 13, 4, ACCENT);
    gfx_round_rect(ListX + 3, 33, 13, 7, 2, PAPER);
    gfx_round_rect(ListX + 14, 39, 19, 13, 4, ACCENT);
    gfx_round_rect(ListX + 17, 42, 13, 7, 2, PAPER);
    gfx_text(ListX + 46, 25, 32, TEXT, "RoomCircuit");
    char version[32];
    snprintf(version, sizeof(version), "v%s", update_ui_app_version());
    gfx_text_fit(ListX + 46, 57, 17, MUTED, version, 218);
    if (g_screen == ListScreen) action(535, 26, "B", "All rooms", HidNpadButton_B, touch);
    const char *tab = ui_text("Appointments");
    gfx_text(741, 30, 22, ACCENT, tab);
    gfx_rect(741, 80, gfx_text_width(22, tab), 2, ACCENT);
    gfx_rect(0, FooterY, GFX_W, GFX_H - FooterY, SELECTED);
    gfx_rect(0, FooterY, GFX_W, 1, LINE);

    const char *title = g_screen == TimeScreen ? "Choose local time" :
        g_screen == GameScreen ? "New appointment" : "Appointments";
    gfx_text(ListX, 98, 37, TEXT, ui_text(title));
    if (g_screen == ListScreen) {
        draw_list(catalog, touch);
    } else {
        if (g_screen == GameScreen) draw_games(touch);
        else draw_time(touch);
        action(ListX, FooterY + 20, "B", "Back", HidNpadButton_B, touch);
    }
    if (g_notice)
        gfx_text_fit(ListX, 619, 19, WARN, ui_text(g_notice), SideX - ListX - 16);
}
