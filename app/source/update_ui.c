#include "update_ui.h"
#include "update_core.h"
#include "update_network.h"
#include "app_socket.h"
#include "ui_locale.h"
#include "ui_text.h"
#include "gfx.h"
#include "relay_config.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef LANYWHERE_APP_VERSION
#define LANYWHERE_APP_VERSION "0.7.0"
#endif
#define UpdateRoot "sdmc:"
#define UpdateDownload "sdmc:/lanywhere/update-download.bin"
#define UpdateBuildVersionIpc 34

static const u32 BackgroundTop = RGBA(0x1b, 0x22, 0x36, 255);
static const u32 BackgroundBottom = RGBA(0x0f, 0x12, 0x1c, 255);
static const u32 Text = RGBA(0xf2, 0xf3, 0xf7, 255);
static const u32 Dim = RGBA(0x9a, 0xa0, 0xb0, 255);
static const u32 Accent = RGBA(0x8b, 0x5c, 0xf6, 255);
static const u32 Error = RGBA(0xf0, 0x4a, 0x4a, 255);

typedef enum {
    UpdateChecking, UpdateCurrent, UpdateOffered, UpdateDownloading,
    UpdateInstalling, UpdateRestart, UpdateFailed, UpdateRecoveryBlocked
} UpdateScreen;

typedef struct {
    UpdateScreen screen;
    UpdateManifest manifest;
    unsigned completed, total;
    const char *error;
    bool changed;
} UpdateView;

static Mutex lock;
static UpdateView state;
static Thread worker;
static bool thread_created, socket_ready, canonical_path, initialized;
static atomic_bool rollback_pending, must_exit;
static atomic_bool worker_done, cancel_requested;
static bool (*installation_allowed)(void *);
static void *installation_context;

static UpdateView snapshot(void)
{
    mutexLock(&lock);
    const UpdateView copy = state;
    mutexUnlock(&lock);
    return copy;
}

static void set_state(UpdateScreen screen, const char *error)
{
    mutexLock(&lock);
    state.screen = screen;
    state.error = error;
    state.changed = true;
    mutexUnlock(&lock);
}

static void set_progress(unsigned completed, unsigned total, void *unused)
{
    (void)unused;
    mutexLock(&lock);
    state.completed = completed;
    state.total = total;
    state.changed = true;
    mutexUnlock(&lock);
}

static const char *network_error(UpdateNetworkResult result)
{
    if (result == UpdateNetworkCancelled) return "Download cancelled.";
    if (result == UpdateNetworkBusy) return "Update server is busy. Try again.";
    if (result == UpdateNetworkIo) return "Could not write the update to SD.";
    if (result == UpdateNetworkInvalid) return "Update response is invalid. Try again.";
    return "Update server is unavailable. Try again.";
}

static const char *core_error(UpdateResult result)
{
    if (result == UPDATE_SPACE) return "Free some SD space and try again.";
    if (result == UPDATE_IO) return "Could not write the update to SD.";
    if (result == UPDATE_RECOVERY_CONFLICT) return "Move switch/lanywhere-recovery.nro off the SD card, then retry.";
    if (result == UPDATE_SIGNATURE || result == UPDATE_INVALID) return "Update verification failed. Try again.";
    return "Update cannot be installed. Use the complete SD package.";
}

static bool game_closed(void)
{
    if (R_FAILED(pmdmntInitialize())) return false;
    u64 application = 0, current = 0;
    const Result status = pmdmntGetApplicationProcessId(&application);
    pmdmntExit();
    if (status == MAKERESULT(15, 1)) return true; // pm::ProcessNotFound.
    if (R_FAILED(status) || R_FAILED(svcGetProcessId(&current, CUR_PROCESS_HANDLE))) return false;
    return !application || application == current;
}

static bool installation_safe(void)
{
    return canonical_path && appletGetFocusState() == AppletFocusState_InFocus && game_closed() && (!installation_allowed || installation_allowed(installation_context));
}

static void finish_worker(void)
{
    atomic_store(&worker_done, true);
}

static void check_worker(void *unused)
{
    (void)unused;
    uint8_t raw[512];
    const UpdateNetworkResult result = update_network_manifest(RELAY_PROFILE_IPV4, RELAY_PROFILE_PORT, raw, &cancel_requested);
    if (result != UpdateNetworkOk) set_state(UpdateFailed, network_error(result));
    else {
        UpdateManifest manifest;
        const UpdateResult valid = update_manifest_parse(raw, LANYWHERE_APP_VERSION, &manifest);
        if (valid == UPDATE_OK || valid == UPDATE_NO_UPDATE) {
            mutexLock(&lock);
            state.manifest = manifest;
            state.screen = valid == UPDATE_OK ? UpdateOffered : UpdateCurrent;
            state.error = NULL;
            state.changed = true;
            mutexUnlock(&lock);
        } else set_state(UpdateFailed, core_error(valid));
    }
    finish_worker();
}

static void install_worker(void *unused)
{
    (void)unused;
    const UpdateManifest manifest = snapshot().manifest;
    const UpdateScreen offered = strcmp(manifest.version, LANYWHERE_APP_VERSION) ? UpdateOffered : UpdateCurrent;
    if (!installation_safe()) {
        set_state(UpdateFailed, canonical_path ? "Close the game from HOME, then try again." :
                                                "Open switch/lanywhere.nro from Album to update.");
        finish_worker();
        return;
    }
    if (mkdir("sdmc:/lanywhere", 0777) != 0 && errno != EEXIST) {
        set_state(UpdateFailed, "Could not write the update to SD.");
        finish_worker();
        return;
    }
    struct stat folder;
    if (lstat("sdmc:/lanywhere", &folder) != 0 || !S_ISDIR(folder.st_mode)) {
        set_state(UpdateFailed, "Could not write the update to SD.");
        finish_worker();
        return;
    }
    const UpdateNetworkResult downloaded = update_network_download(RELAY_PROFILE_IPV4, RELAY_PROFILE_PORT,
        manifest.bundle_sha256, manifest.compressed_size, UpdateDownload, &cancel_requested, set_progress, NULL);
    if (downloaded != UpdateNetworkOk) {
        set_state(downloaded == UpdateNetworkCancelled ? offered : UpdateFailed, network_error(downloaded));
        finish_worker();
        return;
    }
    if (atomic_load(&cancel_requested) || !installation_safe()) {
        unlink(UpdateDownload);
        set_state(offered, atomic_load(&cancel_requested) ? "Download cancelled." : "Close the game from HOME, then try again.");
        finish_worker();
        return;
    }
    mutexLock(&lock);
    if (atomic_load(&cancel_requested)) {
        mutexUnlock(&lock);
        unlink(UpdateDownload);
        set_state(offered, "Download cancelled.");
        finish_worker();
        return;
    }
    state.screen = UpdateInstalling;
    state.error = NULL;
    state.changed = true;
    mutexUnlock(&lock);
    const UpdateResult installed = update_install(UpdateRoot, &manifest, UpdateDownload, set_progress, NULL);
    unlink(UpdateDownload);
    if (installed == UPDATE_OK || installed == UPDATE_INSTALLED) {
        must_exit = true;
        set_state(UpdateRestart, NULL);
    }
    else {
        const UpdateResult recovery = update_recover(UpdateRoot);
        if (recovery == UPDATE_RECOVERED || recovery == UPDATE_INSTALLED) {
            rollback_pending = recovery == UPDATE_RECOVERED;
            must_exit = true;
            set_state(UpdateRestart, "Previous files restored. Restart the console.");
        }
        else set_state(recovery == UPDATE_OK ? UpdateFailed : UpdateRecoveryBlocked, core_error(installed));
    }
    finish_worker();
}

static void collect_worker(void)
{
    if (thread_created && atomic_load(&worker_done)) {
        threadWaitForExit(&worker);
        threadClose(&worker);
        thread_created = false;
    }
}

static bool start_worker(bool install)
{
    if (!RELAY_PROFILE_FIXED) {
        set_state(UpdateFailed, "Use the SD package for your custom build.");
        return false;
    }
    collect_worker();
    if (thread_created) return false;
    if (!socket_ready) socket_ready = app_socket_acquire();
    if (!socket_ready) {
        set_state(UpdateFailed, "Network services are unavailable. Reopen LANywhere.");
        return false;
    }
    atomic_store(&cancel_requested, false);
    atomic_store(&worker_done, false);
    state.completed = state.total = 0;
    set_state(install ? UpdateDownloading : UpdateChecking, NULL);
    if (R_FAILED(threadCreate(&worker, install ? install_worker : check_worker, NULL, NULL, 0x20000, 0x2c, -2))) {
        set_state(UpdateFailed, "Could not start the update. Reopen LANywhere.");
        return false;
    }
    thread_created = true;
    if (R_FAILED(threadStart(&worker))) {
        threadClose(&worker);
        thread_created = false;
        set_state(UpdateFailed, "Could not start the update. Reopen LANywhere.");
        return false;
    }
    return true;
}

static bool running_version_matches(const char *version)
{
    if (strcmp(version, LANYWHERE_APP_VERSION)) return false;
    Service module;
    if (R_FAILED(smGetService(&module, "lanywhr"))) return false;
    char runtime[16] = {0};
    Result result = serviceDispatch(&module, UpdateBuildVersionIpc,
        .buffer_attrs = { SfBufferAttr_Out | SfBufferAttr_HipcMapAlias },
        .buffers = { { runtime, sizeof(runtime) } });
    serviceClose(&module);
    return R_SUCCEEDED(result) && memchr(runtime, 0, sizeof(runtime)) && !strcmp(runtime, version);
}

bool update_ui_initialize(const char *running_path)
{
    mutexInit(&lock);
    initialized = true;
    canonical_path = running_path && (!strcmp(running_path, "sdmc:/switch/lanywhere.nro") || !strcmp(running_path, "/switch/lanywhere.nro"));
    UpdateResult recovered = UPDATE_OK;
    if (game_closed()) {
        recovered = update_recover(UpdateRoot);
    } else if (update_transaction_pending(UpdateRoot) != UPDATE_OK) {
        set_state(UpdateRecoveryBlocked, "Close the game from HOME, then try again.");
        return true;
    }
    if (recovered == UPDATE_INSTALLED || recovered == UPDATE_RECOVERED) {
        char version[32];
        const UpdateResult pending = update_pending_version(UpdateRoot, version);
        if ((pending == UPDATE_INSTALLED || pending == UPDATE_RECOVERED) && running_version_matches(version) &&
            update_acknowledge(UpdateRoot) == UPDATE_OK) {
            state.screen = UpdateChecking;
        } else {
            rollback_pending = recovered == UPDATE_RECOVERED && pending == UPDATE_RECOVERED &&
                canonical_path && !strcmp(version, LANYWHERE_APP_VERSION);
            must_exit = true;
            set_state(UpdateRestart, recovered == UPDATE_RECOVERED ? "Previous files restored. Restart the console." : NULL);
            return true;
        }
    } else if (recovered != UPDATE_OK) {
        set_state(UpdateRecoveryBlocked, "Update recovery needs the complete SD package.");
        return true;
    }
    start_worker(false);
    return false;
}

void update_ui_shutdown(void)
{
    if (!initialized) return;
    if (thread_created) {
        atomic_store(&cancel_requested, true);
        threadWaitForExit(&worker);
        threadClose(&worker);
        thread_created = false;
    }
    if (socket_ready) app_socket_release();
    initialized = socket_ready = false;
}

bool update_ui_available(void)
{
    return initialized && snapshot().screen == UpdateOffered;
}

bool update_ui_changed(void)
{
    if (!initialized) return false;
    collect_worker();
    mutexLock(&lock);
    const bool changed = state.changed;
    state.changed = false;
    mutexUnlock(&lock);
    return changed;
}

const char *update_ui_button_label(void)
{
    return ui_text(update_ui_available() ? "Update available" : "Update");
}

static u64 modal_touch(void)
{
    HidTouchScreenState touch = {0};
    static bool held;
    const bool touched = hidGetTouchScreenStates(&touch, 1) && touch.count;
    if (!touched) { held = false; return 0; }
    if (held) return 0;
    held = true;
    const int x = touch.touches[0].x, y = touch.touches[0].y;
    if (x >= 1056 && y >= 40 && y < 84) return HidNpadButton_StickR;
    if (y >= 650 && x < 600) return HidNpadButton_A;
    if (y >= 650 && x >= 800) return HidNpadButton_B;
    return 0;
}

static const char *screen_title(UpdateScreen screen)
{
    switch (screen) {
        case UpdateChecking: return "Checking for updates…";
        case UpdateCurrent: return "You are up to date";
        case UpdateOffered: return "Update available";
        case UpdateDownloading: return "Downloading…";
        case UpdateInstalling: return "Installing…";
        case UpdateRestart: return "Restart required";
        case UpdateRecoveryBlocked: return "Update recovery";
        default: return "Update could not finish";
    }
}

static void draw_update(const UpdateView *view)
{
    gfx_begin();
    gfx_clear_gradient(BackgroundTop, BackgroundBottom);
    gfx_text(56, 40, 36, Text, "LANywhere");
    gfx_button(1068, 47, "RS", ui_chinese() ? "English" : "中文");
    gfx_text(56, 142, 34, Text, ui_text(screen_title(view->screen)));
    char version[96];
    snprintf(version, sizeof(version), "%s %s", ui_text("Installed"), LANYWHERE_APP_VERSION);
    gfx_text(56, 204, 24, Dim, version);
    if (view->manifest.version[0]) {
        snprintf(version, sizeof(version), "%s %s", ui_text("Available"), view->manifest.version);
        gfx_text(56, 244, 24, Dim, version);
    }
    if (view->screen == UpdateOffered || view->screen == UpdateCurrent) {
        gfx_text_fit(56, 314, 25, Text, ui_chinese() ? view->manifest.notes_zh : view->manifest.notes_en, 1168);
        gfx_text(56, 410, 23, Dim, ui_text("Close the game before installing. Restart after the update."));
    }
    if (view->screen == UpdateDownloading || view->screen == UpdateInstalling) {
        const unsigned percentage = view->total ? (unsigned)((uint64_t)view->completed * 100 / view->total) : 0;
        snprintf(version, sizeof(version), "%u%%", percentage);
        gfx_text(56, 326, 28, Text, version);
        gfx_round_rect(56, 380, 1168, 16, 8, RGBA(255,255,255,30));
        if (percentage) gfx_round_rect(56, 380, (int)(1168 * percentage / 100), 16, 8, Accent);
        if (view->screen == UpdateInstalling) gfx_text(56, 440, 23, Dim, ui_text("Keep the console on until installation finishes."));
    }
    if (view->screen == UpdateRestart) gfx_text(56, 330, 26, Text, ui_text("Exit and restart into your virtual system."));
    if (view->error) gfx_text_fit(56, 500, 23, Error, ui_text(view->error), 1168);
    gfx_rect(0, 648, 1280, 1, RGBA(255,255,255,28));
    if (view->screen == UpdateOffered) gfx_button(56, 670, "A", ui_text("Download and install"));
    else if (view->screen == UpdateCurrent) gfx_button(56, 670, "A", ui_text("Repair installation"));
    else if (view->screen == UpdateRestart && rollback_pending) gfx_button(56, 670, "A", ui_text("Repair installation"));
    else if (view->screen == UpdateFailed) gfx_button(56, 670, "A", ui_text("Try again"));
    if (view->screen != UpdateInstalling)
        gfx_button(960, 670, "B", ui_text(view->screen == UpdateDownloading ? "Cancel" :
            view->screen == UpdateRestart || view->screen == UpdateRecoveryBlocked ? "Exit" : "Back"));
    gfx_end();
}

bool update_ui_open(PadState *pad, bool (*can_install)(void *), void *context)
{
    installation_allowed = can_install;
    installation_context = context;
    bool dirty = true;
    while (appletMainLoop()) {
        collect_worker();
        padUpdate(pad);
        const u64 down = padGetButtonsDown(pad) | modal_touch();
        UpdateView view = snapshot();
        if (down & HidNpadButton_StickR) {
            const bool chinese = !ui_chinese();
            ui_set_language(chinese, false);
            (void)mkdir("sdmc:/lanywhere", 0777);
            (void)ui_locale_save(UI_LOCALE_PATH, chinese);
            dirty = true;
        }
        if ((down & (HidNpadButton_B | HidNpadButton_Plus)) && view.screen != UpdateInstalling) {
            if (view.screen == UpdateDownloading || view.screen == UpdateChecking) {
                mutexLock(&lock);
                const bool installing = state.screen == UpdateInstalling;
                if (!installing) atomic_store(&cancel_requested, true);
                mutexUnlock(&lock);
                if (installing) continue;
                if (thread_created) { threadWaitForExit(&worker); threadClose(&worker); thread_created = false; }
                view = snapshot();
            }
            return must_exit || view.screen == UpdateRestart || view.screen == UpdateRecoveryBlocked;
        }
        if ((down & HidNpadButton_A) && !thread_created) {
            if (view.screen == UpdateRestart && rollback_pending) {
                if (update_acknowledge(UpdateRoot) == UPDATE_OK) {
                    rollback_pending = false;
                    start_worker(false);
                } else set_state(UpdateRecoveryBlocked, "Update recovery needs the complete SD package.");
            } else if (view.screen == UpdateFailed) start_worker(false);
            else if (view.screen == UpdateOffered || view.screen == UpdateCurrent) start_worker(true);
            dirty = true;
        }
        if (dirty || update_ui_changed()) {
            const UpdateView current = snapshot();
            draw_update(&current);
            dirty = false;
        }
        svcSleepThread(33000000ULL);
    }
    update_ui_shutdown();
    return true;
}
