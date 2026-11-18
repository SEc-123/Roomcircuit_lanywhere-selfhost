/* Modified by LANywhere Self-host, 2026-11-07 to 2026-11-08. */
/* LANywhere Tesla status/control overlay. SPDX-License-Identifier: GPL-2.0-only */
#define TESLA_INIT_IMPL
#include <tesla.hpp>
#include "ldn.h"
#include "../../../common/roomcircuit_paths.h"
#include <sys/stat.h>
#include "model.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string_view>
#include "../../../common/node_catalog.h"

namespace {
    Service g_ldnSrv{}, g_relay{};
    LdnMitmConfigService g_config{};
    overlay::Snapshot g_snapshot{};
    RoomPasscodeHost g_passcode_host{};
    NodeHostInfo g_host_node{};
    bool g_host_node_valid = false;
    NodeDynamicEntry g_host_identity{};
    bool g_host_identity_valid = false, g_host_identity_unsupported = false;
    NodeActiveRouteInfo g_active_route{};
    bool g_active_route_valid = false;
    bool g_chinese = false, g_protocol_mismatch = false;
    u32 g_storage_health = 0;
    char g_version[32]{};
    std::string g_notice;
    u64 g_notice_until = 0;
    u64 g_last_poll = 0;

    struct NodeTask {
        u64 id = 0;
        NodeDynamicEntry expected{};
        bool tracked = false;
    };
    NodeTask g_node_task;
    u64 g_last_handled_node_task = 0;

    const char *tr(const char *english, const char *chinese) { return g_chinese ? chinese : english; }

    Result discoverNodeTask() {
        if (g_node_task.tracked) return 0;
        if (!serviceIsActive(&g_relay)) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        NodeHostTaskStatus status{};
        const Result rc = lanywhereOverlayGetNodeTask(&g_relay, 0, &status);
        if (R_SUCCEEDED(rc) &&
            (status.state == NodeHostTaskWorking || status.state == NodeHostTaskFinished) &&
            status.task_id != g_last_handled_node_task) {
            g_node_task.id = status.task_id;
            g_node_task.expected = status.identity;
            g_node_task.tracked = true;
        }
        return rc;
    }

    bool chinesePreference(const char *path, bool *found) {
        FILE *file = std::fopen(path, "rb");
        *found = file != nullptr;
        if (!file) return false;
        char value[32];
        size_t size = std::fread(value, 1, sizeof(value), file);
        const bool valid = std::feof(file) && !std::ferror(file);
        std::fclose(file);
        if (size && value[size - 1] == '\n') --size;
        if (size && value[size - 1] == '\r') --size;
        return valid && std::string_view(value, size) == "language=zh";
    }

    void refreshLanguagePreference() {
        bool chinese = false;
        u32 selection = 2;
        // libultrahand owns the sdmc: mount until __appExit. The renderer and
        // sound worker also read it after overlay services have initialized.
        if (serviceIsActive(&g_relay) &&
            R_SUCCEEDED(lanywhereOverlayGetStorageSelection(&g_relay, &selection))) {
            bool found = false;
            chinese = chinesePreference(rc_storage_path_gen(selection, RC_LEGACY_PATH("language.ini"),
                RC_V1_PATH("language.ini"), RC_CURRENT_PATH("language.ini")), &found);
            if (!found) chinese = chinesePreference(rc_storage_path_gen(selection, RC_LEGACY_PATH("language.ini.previous"),
                RC_V1_PATH("language.ini.previous"), RC_CURRENT_PATH("language.ini.previous")), &found);
        }
        g_chinese = chinese;
        // Ultrahand's framework labels follow RoomCircuit's own preference.
        ult::BACK = tr("Back", "返回"); ult::OK = tr("OK", "确定");
        ult::ON = tr("On", "开启"); ult::OFF = tr("Off", "关闭");
        ult::HIDE = tr("Hide", "隐藏"); ult::CANCEL = tr("Cancel", "取消");
    }

    bool hasPasscodeHost() {
        return g_passcode_host.status == RoomPasscodeOk && g_passcode_host.room[0] != '\0';
    }

    void refresh() {
        g_snapshot.config_ok = false;
        g_snapshot.relay_ok = false;
        g_passcode_host = {};
        g_host_node = {};
        g_host_node_valid = false;
        g_host_identity = {};
        g_host_identity_valid = g_host_identity_unsupported = false;
        g_active_route = {};
        g_active_route_valid = false;
        g_storage_health = 0;
        u32 enabled = 0, logging = 0;
        if (serviceIsActive(&g_config.s) && R_SUCCEEDED(ldnMitmGetEnabled(&g_config, &enabled)) &&
            R_SUCCEEDED(ldnMitmGetLogging(&g_config, &logging))) {
            g_snapshot.config_ok = true;
            g_snapshot.enabled = enabled != 0;
            g_snapshot.logging = logging != 0;
        }
        LanywhereOverlayStatus status{};
        LanywhereOverlayMatch match{};
        if (serviceIsActive(&g_relay) && R_SUCCEEDED(lanywhereOverlayGetStatus(&g_relay, &status)) &&
            R_SUCCEEDED(lanywhereOverlayGetMatch(&g_relay, &match))) {
            g_snapshot.relay_ok = true;
            g_snapshot.status = status;
            g_snapshot.match = match;
            g_host_node_valid = R_SUCCEEDED(lanywhereOverlayGetHostNode(&g_relay, &g_host_node));
            const Result identity_result = lanywhereOverlayGetHostIdentity(&g_relay, &g_host_identity);
            g_host_identity_valid = R_SUCCEEDED(identity_result) && g_host_node_valid &&
                g_host_identity.id == g_host_node.node_id;
            g_host_identity_unsupported = identity_result == MAKERESULT(10, 221);
            g_active_route_valid = R_SUCCEEDED(lanywhereOverlayGetActiveRoute(&g_relay, &g_active_route));
            u32 storage_selection = 2;
            if (R_SUCCEEDED(lanywhereOverlayGetStorageSelection(&g_relay, &storage_selection))) {
                const Result health_result = lanywhereOverlayGetStorageHealth(&g_relay, &g_storage_health);
                if (R_FAILED(health_result))
                    g_storage_health = storage_selection != 3 && health_result == MAKERESULT(10, 221) ?
                        0 : RC_STORAGE_HEALTH_BOOTSTRAP_UNCHECKED;
            }
            if (status.state == 2 && status.my_index == 0 && status.game_id) {
                if (R_FAILED(lanywhereOverlayGetPasscodeHost(&g_relay, &g_passcode_host)) ||
                    std::memcmp(status.code, g_passcode_host.room, sizeof(status.code)))
                    g_passcode_host = {};
            }
        } else {
            g_snapshot.status = {};
            g_snapshot.match = {};
        }
        g_last_poll = armGetSystemTick();
    }

    void closeServices() {
        serviceClose(&g_relay);
        serviceClose(&g_config.s);
        serviceClose(&g_ldnSrv);
        g_snapshot = {};
        g_passcode_host = {};
        g_host_node = {};
        g_host_node_valid = false;
        g_host_identity = {};
        g_host_identity_valid = g_host_identity_unsupported = false;
        g_active_route = {};
        g_active_route_valid = false;
        g_storage_health = 0;
    }

    void connectServices() {
        closeServices();
        g_version[0] = '\0';
        g_notice.clear();
        g_notice_until = 0;
        g_protocol_mismatch = false;
        tsl::hlp::doWithSmSession([] {
            bool present = false;
            if (R_SUCCEEDED(lanywhereOverlayHasService(&present, true)) && present &&
                R_SUCCEEDED(smGetService(&g_ldnSrv, "ldn:u")) &&
                R_SUCCEEDED(ldnMitmGetConfigFromService(&g_ldnSrv, &g_config))) {
                if (R_FAILED(ldnMitmGetVersion(&g_config, g_version))) g_version[0] = '\0';
            }
            if (R_SUCCEEDED(lanywhereOverlayHasService(&present, false)) && present &&
                R_SUCCEEDED(smGetService(&g_relay, "lanywhr"))) {
                u32 version = 0;
                if (R_FAILED(lanywhereOverlayGetProtocol(&g_relay, &version)) || version != LANYWHERE_OVERLAY_IPC_MAJOR) {
                    g_protocol_mismatch = true;
                    serviceClose(&g_relay);
                }
            }
        });
        refresh();
        (void)discoverNodeTask();
    }

    class SafeToggle : public tsl::elm::ToggleListItem {
        bool m_logging;
    public:
        explicit SafeToggle(bool logging) : ToggleListItem(logging ? tr("Logging", "记录日志") : tr("Plugin", "联机插件"), false,
                tr("On", "开启"), tr("Off", "关闭")), m_logging(logging) {}
        bool onClick(u64 keys) override {
            if (!(keys & HidNpadButton_A)) return false;
            refresh();
            if (!g_snapshot.config_ok || (!m_logging && !overlay::canChangeEnabled(g_snapshot))) {
                g_notice = !g_snapshot.config_ok ? tr("Module unavailable. Reconnect or restart the console.", "插件服务不可用，请重连或重启主机。") :
                    tr("Leave the game room and stop matching first.", "请先退出游戏房间并停止匹配。");
                g_notice_until = armGetSystemTick() + 5 * armGetSystemTickFreq();
                return true;
            }
            const bool next = !(m_logging ? g_snapshot.logging : g_snapshot.enabled);
            const Result result = m_logging ? ldnMitmSetLogging(&g_config, next) : ldnMitmSetEnabled(&g_config, next);
            refresh(); // Re-read on failure too; never display an optimistic toggle.
            const bool actual = m_logging ? g_snapshot.logging : g_snapshot.enabled;
            if (R_FAILED(result) || !g_snapshot.config_ok || actual != next)
                g_notice = tr("Change failed. Reconnect and try again.", "修改失败，请重新连接后再试。");
            else g_notice = m_logging ? tr("Logging updated.", "日志设置已更新。") :
                tr("Applies next time the game opens local wireless.", "下次进入游戏的本地联机时生效。");
            g_notice_until = armGetSystemTick() + 5 * armGetSystemTickFreq();
            this->setState(actual);
            return true;
        }
    };

    const char *connectionLabel() {
        if (!g_snapshot.relay_ok) return g_protocol_mismatch ? tr("Update required", "需要更新") : tr("Unavailable", "服务不可用");
        if (g_snapshot.status.state == 1)
            return !std::strcmp(g_snapshot.status.error, "Reconnecting to the room.") ?
                tr("Reconnecting", "正在重连") : tr("Connecting", "连接中");
        if (g_snapshot.status.state == 3) {
            if (!std::strcmp(g_snapshot.status.error, "Room session expired. Rejoin from the game."))
                return tr("Session expired", "会话已过期");
            if (!std::strcmp(g_snapshot.status.error, "Room changed during recovery. Rejoin from the game."))
                return tr("Room changed", "房间已变化");
            if (!std::strcmp(g_snapshot.status.error, "Could not reconnect. Check Wi-Fi and rejoin."))
                return tr("Reconnect failed", "重连未成功");
            return tr("Disconnected", "连接已断开");
        }
        if (g_snapshot.status.error[0]) return tr("Check connection", "连接需检查");
        switch (g_snapshot.status.state) {
            case 0: return tr("Not in a room", "未加入房间");
            case 1: return tr("Connecting", "连接中");
            case 2: return tr("In a room", "已加入房间");
            default: return tr("Disconnected", "连接已断开");
        }
    }
    const char *matchLabel() {
        if (!g_snapshot.relay_ok) return tr("Unavailable", "服务不可用");
        switch (g_snapshot.match.state) {
            case 1: return tr("Waiting for game", "等待游戏");
            case 2: return tr("Searching", "搜索中");
            case 3: return tr("Room found", "已找到房间");
            case 4: return tr("Joining", "加入中");
            case 5: return tr("Joined", "已加入");
            case 6: return tr("Expired", "已超时");
            case 8: return tr("Failed", "未成功");
            default: return tr("Off", "未匹配");
        }
    }

    class PasscodeGui : public tsl::Gui {
        static constexpr std::array<std::string_view, 4> Characters{
            "0123456789",
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
            "abcdefghijklmnopqrstuvwxyz",
            " !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"
        };
        RoomPasscodeChange m_change{};
        size_t m_length{}, m_group{}, m_character{};
        bool m_hidden = true;
        tsl::elm::CategoryHeader *m_visibility{};
        tsl::elm::ListItem *m_code_first{}, *m_code_second{}, *m_picker{}, *m_feedback{};
        const char *m_message = "";

        static size_t cycle(size_t index, int step, size_t count) {
            return (static_cast<int>(index + count) + step) % count;
        }

        void submit(bool clear) {
            if (!clear && !m_length) {
                m_message = tr("Enter the game password.", "请输入游戏密码。");
                return;
            }
            refresh();
            if (!hasPasscodeHost() || std::memcmp(m_change.room, g_passcode_host.room, sizeof(m_change.room))) {
                m_message = tr("Room closed or changed.", "房间已关闭或变化。");
                return;
            }

            RoomAccessChange request{};
            std::memcpy(request.change.room, m_change.room, sizeof(request.change.room));
            if (!clear) std::memcpy(request.change.passcode, m_change.passcode, m_length);
            request.hidden = !clear && m_hidden;
            u32 status = RoomPasscodeUnavailable;
            if (R_FAILED(lanywhereOverlaySetRoomAccess(&g_relay, &request, &status)))
                status = RoomPasscodeUnavailable;
            switch (status) {
                case RoomPasscodeOk:
                    std::memset(m_change.passcode, 0, sizeof(m_change.passcode));
                    m_length = 0;
                    if (clear) m_message = tr("Password cleared.", "密码已清除。");
                    else if (m_hidden) m_message = tr("Password hidden.", "密码已隐藏。");
                    else m_message = tr("Password shared in lobby.", "密码已公开到大厅。");
                    g_notice = m_message;
                    g_notice_until = armGetSystemTick() + 5 * armGetSystemTickFreq();
                    break;
                case RoomPasscodeInvalid: m_message = tr("Invalid password.", "密码格式无效。"); break;
                case RoomPasscodeRoomUnavailable: m_message = tr("Room closed or changed.", "房间已关闭或变化。"); break;
                case RoomPasscodeNotHost: m_message = tr("Host in the game first.", "请先在游戏里建房。"); break;
                case RoomPasscodeBusy: m_message = tr("Try again in a moment.", "请稍后再试。"); break;
                default: m_message = tr("Could not save password.", "无法保存密码。"); break;
            }
        }

        void redraw() {
            const size_t first = std::min<size_t>(12, m_length);
            const auto segment = [&](size_t start, size_t count) {
                return count ? "[" + std::string(m_change.passcode + start, count) + "]" : "—";
            };
            m_visibility->setText(m_hidden ?
                tr("Hidden · Guests enter password", "隐藏 · 访客需输入密码") :
                tr("Shared · Visible in the lobby", "公开 · 大厅所有人可见"));
            m_code_first->setText(segment(0, first));
            m_code_second->setText(segment(first, m_length - first));
            const char current = Characters[m_group][m_character];
            const char *group_names[] = {
                tr("Digits", "数字"), tr("Uppercase", "大写"), tr("Lowercase", "小写"), tr("Symbols", "符号")
            };
            const std::string character = current == ' ' ? tr("Space", "空格") : std::string(1, current);
            m_picker->setValue(character + "  ·  " + group_names[m_group]);
            m_feedback->setText(m_message);
        }

    public:
        explicit PasscodeGui(const RoomPasscodeHost &host) {
            std::memcpy(m_change.room, host.room, sizeof(m_change.room));
        }

        tsl::elm::Element *createUI() override {
            auto *frame = new tsl::elm::OverlayFrame(tr("Game password", "游戏密码"),
                std::string(tr("Room ", "房间 ")) + m_change.room);
            auto *list = new tsl::elm::List();
            list->addItem(m_visibility = new tsl::elm::CategoryHeader(""));
            list->addItem(m_code_first = new tsl::elm::ListItem(""));
            list->addItem(m_code_second = new tsl::elm::ListItem(""));
            list->addItem(m_picker = new tsl::elm::ListItem(tr("Character", "字符")));
            list->addItem(new tsl::elm::ListItem(tr("←/→ Choose   A Add", "←/→ 选择   A 添加")));
            list->addItem(new tsl::elm::ListItem(tr("L/R Group   Y Delete", "L/R 字符组   Y 删除")));
            list->addItem(new tsl::elm::ListItem(tr("X Mode   + Save   − Clear", "X 模式   + 保存   − 清除")));
            list->addItem(m_feedback = new tsl::elm::ListItem(""));
            frame->setContent(list);
            redraw();
            return frame;
        }

        bool handleInput(u64 down, u64, const HidTouchState &, HidAnalogStickState, HidAnalogStickState) override {
            if (!down || (down & HidNpadButton_B)) return false;
            m_message = "";
            if (down & HidNpadButton_Plus) {
                submit(false);
            } else if (down & HidNpadButton_Minus) {
                submit(true);
            } else if (down & HidNpadButton_X) {
                m_hidden = !m_hidden;
            } else if (down & HidNpadButton_Y) {
                if (m_length) m_change.passcode[--m_length] = '\0';
            } else if (down & HidNpadButton_A) {
                if (m_length < sizeof(m_change.passcode))
                    m_change.passcode[m_length++] = Characters[m_group][m_character];
                else
                    m_message = tr("Maximum 24 characters.", "最多 24 个字符。");
            } else if (down & (HidNpadButton_L | HidNpadButton_R)) {
                m_group = cycle(m_group, down & HidNpadButton_R ? 1 : -1, Characters.size());
                m_character = 0;
            } else if (down & (HidNpadButton_AnyLeft | HidNpadButton_AnyRight)) {
                m_character = cycle(m_character, down & HidNpadButton_AnyRight ? 1 : -1, Characters[m_group].size());
            } else {
                return false;
            }
            redraw();
            return true;
        }
    };

    const char *nodeRegion(u8 region) {
        switch (region) {
            case NodeRegionAsia: return tr("Asia", "亚洲");
            case NodeRegionEurope: return tr("Europe", "欧洲");
            case NodeRegionAmericas: return tr("Americas", "美洲");
            case NodeRegionOceania: return tr("Oceania", "大洋洲");
            case NodeRegionAfrica: return tr("Africa", "非洲");
            default: return tr("Region unknown", "地区未知");
        }
    }

    NodeDynamicEntry builtinNode(u32 id) {
        NodeDynamicEntry result{};
        const auto &source = LanywhereNodes[id];
        result.id = id;
        result.protocol = static_cast<u8>(source.protocol);
        result.region = source.region;
        result.port = source.port;
        result.flags = 1;
        std::snprintf(result.host, sizeof(result.host), "%s", source.host);
        std::snprintf(result.name, sizeof(result.name), "%s", source.name_en);
        return result;
    }

    bool sameHostNode(const NodeDynamicEntry &expected) {
        return g_host_node_valid && g_host_identity_valid && g_host_node.node_id == expected.id &&
            !std::memcmp(&g_host_identity, &expected, sizeof(expected));
    }

    const char *nodeTaskFailure(Result result) {
        // Atmosphere SF module 10, CMIF UnknownCommandId 221: old module.
        if (result == MAKERESULT(10, 221))
            return tr("Update the background module.", "请更新后台模块。");
        return tr("Selection failed. Try again.", "选择失败，请重试。");
    }

    enum class NodePoll { Waiting, Selected, Failed, Changed, Expired, Unavailable, UpdateRequired };

    NodePoll pollNodeTask() {
        if (!g_node_task.tracked || !serviceIsActive(&g_relay)) return NodePoll::Unavailable;
        NodeHostTaskStatus status{};
        const Result rc = lanywhereOverlayGetNodeTask(&g_relay, g_node_task.id, &status);
        if (R_FAILED(rc)) return rc == MAKERESULT(10, 221) ? NodePoll::UpdateRequired : NodePoll::Unavailable;
        if (status.state == NodeHostTaskExpired || status.state == NodeHostTaskIdle ||
            status.task_id != g_node_task.id ||
            std::memcmp(&status.identity, &g_node_task.expected, sizeof(status.identity))) {
            g_last_handled_node_task = g_node_task.id;
            g_node_task.tracked = false;
            return NodePoll::Expired;
        }
        if (status.state == NodeHostTaskWorking) return NodePoll::Waiting;
        g_last_handled_node_task = g_node_task.id;
        g_node_task.tracked = false;
        if (R_FAILED(status.result)) return NodePoll::Failed;
        refresh();
        return sameHostNode(status.identity) && g_host_node.status != NodeHostUnavailable ? NodePoll::Selected : NodePoll::Changed;
    }

    const char *nodePollMessage(NodePoll result) {
        switch (result) {
            case NodePoll::Waiting: return tr("Checking…", "正在检查…");
            case NodePoll::Selected: return tr("Server selected.", "已选择服务器。");
            case NodePoll::Changed: return tr("Server changed. Check again.", "服务器已变化，请重新检查。");
            case NodePoll::Expired: return tr("Server check expired. Try again.", "服务器检查已过期，请重试。");
            case NodePoll::Unavailable: return tr("Cannot check server. Try again.", "无法检查服务器，请重试。");
            case NodePoll::UpdateRequired: return tr("Update the background module.", "请更新后台模块。");
            default: return tr("Selection failed. Try again.", "选择失败，请重试。");
        }
    }

    class NodeSelectGui : public tsl::Gui {
        struct NodeRow { tsl::elm::ListItem *item{}; NodeDynamicEntry node{}; };
        NodeDynamicCatalog m_catalog{};
        NodeDynamicCatalog m_new_catalog{};
        std::array<NodeRow, NODE_TOTAL_CAPACITY> m_rows{};
        size_t m_row_count = 0;
        u64 m_next_catalog_poll = 0;
        u64 m_next_task_poll = 0;
        tsl::elm::List *m_list{};
        tsl::elm::ListItem *m_feedback{};
        bool m_waiting = false, m_catalog_valid = false;

        bool catalogContains(const NodeDynamicEntry &node) const {
            if (node.id < NODE_CATALOG_COUNT) return true;
            if (!m_catalog_valid) return false;
            if (node.id >= NODE_LOCAL_BASE && (m_catalog.enabled & NODE_LOCAL_CONFIG_INVALID)) return false;
            for (u32 i = 0; i < m_catalog.count; ++i)
                if (!std::memcmp(&m_catalog.nodes[i], &node, sizeof(node))) return true;
            return false;
        }

        void updateRows() {
            for (size_t i = 0; i < m_row_count; ++i) {
                const auto &row = m_rows[i];
                const bool available = (row.node.flags & 1) && catalogContains(row.node);
                const char *value = !available ? tr("Unavailable", "不可用") :
                    sameHostNode(row.node) ? tr("Current", "当前") : nodeRegion(row.node.region);
                row.item->setValue(value, !available);
            }
        }

        void select(const NodeDynamicEntry &node) {
            const Result discovery = discoverNodeTask();
            if (R_FAILED(discovery)) {
                m_feedback->setText(nodeTaskFailure(discovery));
                return;
            }
            if (g_node_task.tracked) {
                const NodePoll current = pollNodeTask();
                if (current == NodePoll::Waiting || current == NodePoll::Unavailable || current == NodePoll::UpdateRequired) {
                    m_waiting = true;
                    m_feedback->setText(nodePollMessage(current));
                    return;
                }
            }
            refresh();
            if (g_host_identity_unsupported) {
                m_feedback->setText(tr("Update the background module.", "请更新后台模块。"));
                return;
            }
            if (!overlay::canSelectHostNode(g_snapshot) || !g_active_route_valid ||
                g_active_route.status != NodeRouteNone) {
                m_feedback->setText(tr("Leave the room and stop matching first.", "请先退出房间并停止匹配。"));
                return;
            }
            if (node.id >= NODE_CATALOG_COUNT && !m_catalog_valid) {
                m_feedback->setText(tr("Custom nodes unavailable. Check configuration.", "自定义节点不可用，请检查配置。"));
                return;
            }
            if (node.id >= NODE_LOCAL_BASE && (m_catalog.enabled & NODE_LOCAL_CONFIG_INVALID)) {
                m_feedback->setText(tr("Local nodes unavailable. Check nodes.ini.", "本机节点不可用，请检查 nodes.ini。"));
                return;
            }
            if (!catalogContains(node)) {
                m_feedback->setText(tr("Server list changed. Reopen.", "服务器列表已变化，请重新打开。"));
                return;
            }
            if (!(node.flags & 1)) {
                m_feedback->setText(tr("Server unavailable.", "服务器不可用。"));
                return;
            }
            m_waiting = true;
            u64 task_id = 0;
            const Result rc = lanywhereOverlayStartNodeTask(&g_relay, &node, &task_id);
            if (R_SUCCEEDED(rc)) {
                g_node_task = {task_id, node, true};
                m_next_task_poll = 0;
                m_feedback->setText(tr("Checking…", "正在检查…"));
            } else {
                m_waiting = false;
                m_feedback->setText(nodeTaskFailure(rc));
            }
        }

        void addNode(tsl::elm::List *list, const NodeDynamicEntry &node, const char *name) {
            if (m_row_count >= m_rows.size()) return;
            const size_t index = m_row_count++;
            auto *item = new tsl::elm::ListItem(name);
            m_rows[index] = {item, node};
            const bool current = sameHostNode(node);
            item->setValue(current ? tr("Current", "当前") : nodeRegion(node.region), !(node.flags & 1));
            item->setClickListener([this, index](u64 keys) {
                if (!(keys & HidNpadButton_A)) return false;
                select(m_rows[index].node);
                return true;
            });
            list->addItem(item);
        }

        void pollCatalog() {
            const u64 now = armGetSystemTick();
            if (now < m_next_catalog_poll) return;
            m_next_catalog_poll = now + 2 * armGetSystemTickFreq();
            if (!serviceIsActive(&g_relay) || R_FAILED(lanywhereOverlayGetNodeCatalog(&g_relay, &m_new_catalog))) {
                m_catalog_valid = false;
                updateRows();
                m_feedback->setText(tr("Custom nodes unavailable. Check configuration.", "自定义节点不可用，请检查配置。"));
                return;
            }
            bool changed = false;
            for (u32 i = 0; i < m_new_catalog.count; ++i) {
                const auto &node = m_new_catalog.nodes[i];
                bool exists = false;
                for (size_t j = NODE_CATALOG_COUNT; j < m_row_count; ++j) {
                    if (m_rows[j].node.id == node.id) {
                        exists = true;
                        if (std::memcmp(&m_rows[j].node, &node, sizeof(node))) changed = true;
                        break;
                    }
                }
                if (!exists) addNode(m_list, node, node.name);
            }
            for (size_t j = NODE_CATALOG_COUNT; j < m_row_count; ++j) {
                bool exists = false;
                for (u32 i = 0; i < m_new_catalog.count; ++i)
                    if (m_rows[j].node.id == m_new_catalog.nodes[i].id) { exists = true; break; }
                if (!exists) changed = true;
            }
            m_catalog = m_new_catalog;
            m_catalog_valid = true;
            updateRows();
            // A reused ID must not silently change what an existing row means.
            if (m_catalog.enabled & NODE_LOCAL_CONFIG_INVALID)
                m_feedback->setText(tr("Local nodes unavailable. Check nodes.ini.", "本机节点不可用，请检查 nodes.ini。"));
            else if (changed && !m_waiting)
                m_feedback->setText(tr("Server list changed. Reopen.", "服务器列表已变化，请重新打开。"));
        }

    public:
        tsl::elm::Element *createUI() override {
            auto *frame = new tsl::elm::OverlayFrame(tr("Choose server", "选择服务器"), GITDESCVER);
            auto *list = m_list = new tsl::elm::List();
            list->addItem(m_feedback = new tsl::elm::ListItem(""));
            for (u32 id = 0; id < NODE_CATALOG_COUNT; ++id) {
                const auto node = builtinNode(id);
                const char *name = id == 0 ? tr("Main server", "主服务器") :
                    id == 1 ? tr("Community · LANywhere", "社区 · LANywhere") :
                    (g_chinese ? LanywhereNodes[id].name_zh : LanywhereNodes[id].name_en);
                addNode(list, node, name);
            }
            if (serviceIsActive(&g_relay) && R_SUCCEEDED(lanywhereOverlayGetNodeCatalog(&g_relay, &m_catalog))) {
                m_catalog_valid = true;
                for (u32 i = 0; i < m_catalog.count; ++i) addNode(list, m_catalog.nodes[i], m_catalog.nodes[i].name);
                if (m_catalog.enabled & NODE_LOCAL_CONFIG_INVALID)
                    m_feedback->setText(tr("Local nodes unavailable. Check nodes.ini.", "本机节点不可用，请检查 nodes.ini。"));
            } else {
                m_feedback->setText(tr("Custom nodes unavailable. Check configuration.", "自定义节点不可用，请检查配置。"));
            }
            if (g_host_identity_unsupported)
                m_feedback->setText(tr("Update the background module.", "请更新后台模块。"));
            const Result discovery = discoverNodeTask();
            if (R_FAILED(discovery)) m_feedback->setText(nodeTaskFailure(discovery));
            if (g_node_task.tracked) {
                m_waiting = true;
                m_feedback->setText(tr("Checking…", "正在检查…"));
            }
            updateRows();
            frame->setContent(list);
            return frame;
        }

        void update() override {
            if (!m_waiting) pollCatalog();
            if (!m_waiting) return;
            const u64 now = armGetSystemTick();
            if (now < m_next_task_poll) return;
            m_next_task_poll = now + armGetSystemTickFreq() / 2;
            const NodePoll result = pollNodeTask();
            m_feedback->setText(nodePollMessage(result));
            if (result == NodePoll::Waiting || result == NodePoll::Unavailable || result == NodePoll::UpdateRequired) return;
            updateRows();
            g_notice = nodePollMessage(result);
            g_notice_until = armGetSystemTick() + 5 * armGetSystemTickFreq();
            m_waiting = false;
        }
    };

    class MainGui : public tsl::Gui {
        tsl::elm::List *m_list{};
        tsl::elm::ListItem *m_connection{}, *m_host_node{}, *m_active_route{}, *m_room{}, *m_players{}, *m_match{}, *m_notice{}, *m_password{};
        SafeToggle *m_enabled{}, *m_logging{};

        void updatePasswordAction() {
            if (hasPasscodeHost() && !m_password) {
                m_password = new tsl::elm::ListItem(tr("Game password", "游戏密码"));
                m_password->setClickListener([](u64 keys) {
                    if (!(keys & HidNpadButton_A)) return false;
                    refresh();
                    if (hasPasscodeHost()) {
                        tsl::changeTo<PasscodeGui>(g_passcode_host);
                    } else {
                        g_notice = tr("Room closed or changed.", "房间已关闭或变化。");
                        g_notice_until = armGetSystemTick() + 5 * armGetSystemTickFreq();
                    }
                    return true;
                });
                m_list->addItem(m_password, 0, 6);
            } else if (!hasPasscodeHost() && m_password) {
                if (getFocusedElement() == m_password) {
                    removeFocus();
                    restoreFocus();
                }
                m_list->removeItem(m_password);
                m_password = nullptr;
            }
        }

    public:
        tsl::elm::Element *createUI() override {
            auto *frame = new tsl::elm::OverlayFrame("RoomCircuit", GITDESCVER);
            auto *list = m_list = new tsl::elm::List();
            list->addItem(new tsl::elm::CategoryHeader(tr("Current session", "当前连接")));
            list->addItem(m_connection = new tsl::elm::ListItem(tr("Connection", "连接")));
            list->addItem(m_host_node = new tsl::elm::ListItem(tr("Hosting node", "建房节点")));
            m_host_node->setClickListener([](u64 keys) {
                if (!(keys & HidNpadButton_A)) return false;
                refresh();
                if (g_snapshot.relay_ok) tsl::changeTo<NodeSelectGui>();
                else {
                    g_notice = tr("Module unavailable. Reconnect or restart the console.", "插件服务不可用，请重连或重启主机。");
                    g_notice_until = armGetSystemTick() + 5 * armGetSystemTickFreq();
                }
                return true;
            });
            list->addItem(m_active_route = new tsl::elm::ListItem(tr("Room node", "房间节点")));
            list->addItem(m_room = new tsl::elm::ListItem(tr("Room", "房间")));
            list->addItem(m_players = new tsl::elm::ListItem(tr("Room members", "房间成员")));
            list->addItem(m_match = new tsl::elm::ListItem(tr("Match", "匹配")));
            list->addItem(new tsl::elm::CategoryHeader(tr("Controls", "控制")));
            list->addItem(m_enabled = new SafeToggle(false));
            list->addItem(m_logging = new SafeToggle(true));
            auto *retry = new tsl::elm::ListItem(tr("Reconnect to module", "重新连接插件"));
            retry->setClickListener([](u64 keys) {
                if (!(keys & HidNpadButton_A)) return false;
                connectServices();
                return true;
            });
            list->addItem(retry);
            list->addItem(m_notice = new tsl::elm::ListItem(""));
            frame->setContent(list);
            update();
            return frame;
        }
        void update() override {
            const bool due = armGetSystemTick() - g_last_poll >= armGetSystemTickFreq();
            if (due) refresh();
            if (due && g_node_task.tracked) {
                const NodePoll result = pollNodeTask();
                g_notice = nodePollMessage(result);
                g_notice_until = armGetSystemTick() + 5 * armGetSystemTickFreq();
            }
            if (!g_notice.empty() && armGetSystemTick() >= g_notice_until) g_notice.clear();
            updatePasswordAction();
            m_connection->setValue(connectionLabel(), !g_snapshot.relay_ok);
            if (!g_host_node_valid) {
                m_host_node->setValue(tr("Unavailable", "服务不可用"), true);
            } else {
                std::string node = g_host_node.name;
                if (g_host_node.status == NodeHostUnavailable) node += tr(" · Unavailable", " · 不可用");
                else if (g_host_node.status == NodeHostPending) node += tr(" · Pending", " · 待检查");
                m_host_node->setValue(node, g_host_node.status == NodeHostPending || g_host_node.status == NodeHostUnavailable);
            }
            if (!g_active_route_valid) {
                m_active_route->setValue(tr("Unavailable", "服务不可用"), true);
            } else if (g_active_route.status == NodeRouteNone) {
                m_active_route->setValue("—", true);
            } else {
                std::string node = g_active_route.name;
                if (g_active_route.status == NodeRouteSelected) node += tr(" · Selected", " · 已选择");
                else if (g_active_route.status == NodeRouteRecovering) node += tr(" · Reconnecting", " · 正在重连");
                else if (g_active_route.status == NodeRouteUnavailable) node += tr(" · Unavailable", " · 不可用");
                m_active_route->setValue(node, g_active_route.status != NodeRouteConnected);
            }
            const bool room = g_snapshot.relay_ok && g_snapshot.status.state == 2;
            m_room->setValue(room ? g_snapshot.status.code : "—", !room);
            char players[16]{};
            if (room) std::snprintf(players, sizeof(players), "%u / 8", g_snapshot.status.member_count);
            m_players->setValue(room ? players : "—", !room);
            m_match->setValue(matchLabel(), !g_snapshot.relay_ok);
            m_enabled->setState(g_snapshot.config_ok && g_snapshot.enabled);
            m_logging->setState(g_snapshot.config_ok && g_snapshot.logging);
            if (!g_snapshot.config_ok) {
                m_enabled->setValue(tr("Unavailable", "服务不可用"), true);
                m_logging->setValue(tr("Unavailable", "服务不可用"), true);
            }
            const char *notice = g_protocol_mismatch ? tr("Install the module and overlay from the same release.", "请使用同一版本的插件和浮层。") :
                !g_snapshot.config_ok ? tr("Module not loaded. Restart after installation.", "插件未加载，安装后请重启主机。") :
                g_snapshot.relay_ok && g_snapshot.status.state == 1 ? tr("Waiting for the room connection.", "正在等待房间连接。") :
                g_snapshot.relay_ok && g_snapshot.status.error[0] ? tr("Open RoomCircuit for connection details.", "请打开 RoomCircuit 查看连接详情。") :
                g_storage_health & RC_STORAGE_HEALTH_BOOTSTRAP_CONFLICT ? tr("Server settings conflict. Compare config.ini files.", "服务器设置冲突，请对比 config.ini。") :
                g_storage_health & RC_STORAGE_HEALTH_BOOTSTRAP_UNCHECKED ? tr("Could not check server settings. Try again.", "无法检查服务器设置，请重试。") :
                g_storage_health & RC_STORAGE_HEALTH_LEGACY_CHANGED ? tr("Older settings changed. Back up and compare.", "旧设置已变更，请备份并对比。") :
                g_storage_health & RC_STORAGE_HEALTH_LEGACY_UNCHECKED ? tr("Could not check older settings. Back up and compare.", "无法检查旧设置，请备份并对比。") :
                !g_notice.empty() ? g_notice.c_str() :
                tr("Plugin changes apply on the next local-wireless session.", "插件开关在下次本地联机时生效。");
            m_notice->setText(notice);
        }
    };

    class Overlay : public tsl::Overlay {
    public:
        void initServices() override {
            connectServices();
            refreshLanguagePreference();
        }
        void exitServices() override { closeServices(); }
        void onShow() override {
            const bool previous = g_chinese;
            refreshLanguagePreference();
            if (previous != g_chinese) tsl::swapTo<MainGui>(SwapDepth{UINT32_MAX});
            refresh();
        }
        void onHide() override {} // Never leave a room or change settings when hidden.
        std::unique_ptr<tsl::Gui> loadInitialGui() override { return initially<MainGui>(); }
    };
}
int main(int argc, char **argv) { return tsl::loop<Overlay>(argc, argv); }
