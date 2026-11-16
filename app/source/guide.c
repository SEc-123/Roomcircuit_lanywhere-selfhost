#include <stdio.h>
#include "guide.h"
#include "gfx.h"

typedef struct { const char *title, *steps[3], *key; } GuidePage;
static const GuidePage pages_zh[] = {
    { "创建房间", {
        "大厅按 Y 打开节点列表，选中节点后按 A。",
        "确认大厅底部的「建房线路」已显示所选节点。",
        "按 + 回到游戏，在「本地无线」里创建房间。"
    }, "也可在浮层选「建房节点」。新房间会使用所选线路。" },
    { "加入房间", {
        "在大厅按 A 选房，线路按房间来源自动切换。",
        "按提示打开游戏，或从 HOME 回游戏。",
        "在“本地无线”里搜索房间并加入。"
    }, "选房后可查看你到节点的延迟。ZL+ZR 筛选游戏和地区。" },
    { "游戏内配对", {
        "两台都开启 RoomCircuit 的本地无线接管。",
        "在游戏里选本地通信，输入相同连接码。",
        "两台都在游戏里开始寻找。"
    }, "适用于宝可梦连接交换等游戏内双人搜索。" },
    { "游戏密码", {
        "“在大厅分享”会让选房的玩家看到密码。",
        "“隐藏密码”需要玩家向房主获取密码。",
        "留空并确认可清除大厅中的密码设置。"
    }, "先在游戏里设置密码。使用第三方节点时，加入者在游戏里输入。" },
    { "约局", {
        "大厅按 − 或点「约局」。按 B 回大厅。",
        "选中约局按 A 加入或退出。按 L 发起新约局。",
        "开场前 30 分钟起按 Y 准备，临近开场按 X 找房。"
    }, "加入可收开房提醒。房主按 Y 准备后开房可关联约局。" },
    { "连不上", {
        "确认联网，尽量使用相同游戏版本。",
        "使用“本地无线”，别选游戏的官方在线模式。",
        "仍失败：退出游戏房间，再重新选择或创建。"
    }, "社区线路连不上时，可切回主服务器。" },
    { "一起测速", {
        "在节点列表按 L，发起或输入临时测速码。",
        "双方留在测速页，查看共同推荐节点。",
        "建房者选定节点后，照常回游戏建房。"
    }, "公开房可直接单人测速建房。好友测速自愿使用。" },
    { "在线更新", {
        "顶部高亮表示有新版。按 LS 随时检查更新。",
        "关闭游戏，确认下载并安装。设置和约局会保留。",
        "更新完成后，退出并重启进入虚拟系统。"
    }, "也可以按 LS 检查更新。安装时请保持开机。" },
    { "自定义节点", {
        "在 config/RoomCircuit/nodes.ini 添加节点。",
        "保存文件后重新打开节点列表。",
        "未收录的节点需双方设置相同地址并选择。"
    }, "管理员收录的节点及房间会显示在统一大厅。" },
};
static const GuidePage pages_en[] = {
    { "Host a room", {
        "Press Y in the lobby, choose a node, then press A.",
        "Check the selected node under Hosting line in the lobby.",
        "Press + to return to the game and host in Local Wireless."
    }, "You can also choose the hosting node in the overlay." },
    { "Join a room", {
        "Select with A. The lobby uses that room's source.",
        "Open the game when prompted, or return to it from HOME.",
        "Search in Local Wireless and join the room."
    }, "Check your latency before joining. ZL+ZR filters game and region." },
    { "Pair in game", {
        "Enable RoomCircuit on both consoles.",
        "Choose local communication and enter the same link code.",
        "Start searching from the game on both consoles."
    }, "For in-game pairing such as Pokémon Link Trade." },
    { "Game passwords", {
        "Share in lobby lets players see the password.",
        "Keep password hidden: players ask the host for it.",
        "Leave it blank to clear the lobby password setting."
    }, "Set the password in the game. On direct nodes, guests enter it in the game." },
    { "Appointments", {
        "Press − or tap Appointments. B returns to the lobby.",
        "A to join or leave a selected appointment. L to create.",
        "Y for Ready from 30 min before. X to find rooms."
    }, "Join for room alerts. Hosts can mark Ready before opening a room to link it." },
    { "Cannot connect?", {
        "Check internet. Use the same game version where possible.",
        "Choose Local Wireless in the game's menu.",
        "Still stuck? Leave the in-game room, then choose or host again."
    }, "If Community is unreachable, switch to Main server." },
    { "Test with friends", {
        "Press L in the node list to create or enter a test code.",
        "Stay on the node screen to compare shared results.",
        "The host selects a node, then creates the room in-game."
    }, "Public hosting uses your own test. Shared testing is optional." },
    { "Online update", {
        "A highlighted update button means a new version is available.",
        "Close the game, then confirm Download and install.",
        "When finished, exit and restart into your virtual system."
    }, "You can check manually with LS. Keep the console on while installing." },
    { "Custom nodes", {
        "Add nodes to config/RoomCircuit/nodes.ini.",
        "Save the file and reopen the node list.",
        "For local nodes, both players select the same address."
    }, "Published nodes and rooms appear in the shared lobby." },
};

enum { PageCount = sizeof(pages_zh) / sizeof(pages_zh[0]), Margin = 56, Footer = 648 };
static int page_index(int page) { return page < 0 ? 0 : page >= PageCount ? PageCount - 1 : page; }
int guide_page_count(void) { return PageCount; }

void guide_draw_language(bool chinese)
{
    gfx_round_rect(1056, 40, 168, 44, 18, RGBA(0x11, 0x11, 0x11, 0xFF));
    gfx_button(1068, 47, "RS", chinese ? "English" : "中文");
}

void guide_draw(int page, bool chinese, bool spanish)
{
    (void)spanish;
    page = page_index(page);
    const GuidePage *p = &(chinese ? pages_zh : pages_en)[page];
    const u32 text = RGBA(0xE8, 0xE8, 0xE8, 0xFF), dim = RGBA(0xAE, 0xAE, 0xAE, 0xFF);
    const u32 accent = RGBA(0xD8, 0xD8, 0xD8, 0xFF), panel = RGBA(0x11, 0x11, 0x11, 0xFF);
    gfx_clear_gradient(RGBA(0x00, 0x00, 0x00, 0xFF), RGBA(0x00, 0x00, 0x00, 0xFF));
    gfx_text(Margin, 42, 24, dim, "RoomCircuit");
    guide_draw_language(chinese);
    char position[20];
    snprintf(position, sizeof(position), "%d / %d", page + 1, PageCount);
    gfx_text(GFX_W - Margin - gfx_text_width(24, position), 128, 24, dim, position);
    gfx_text(Margin, 120, 40, text, p->title);
    for (int i = 0; i < 3; i++) {
        const int y = 228 + i * 92;
        char number[2] = { (char)('1' + i), 0 };
        gfx_round_rect(Margin, y - 4, 48, 48, 24, accent);
        gfx_text(Margin + (48 - gfx_text_width(26, number)) / 2, y + 6, 26, panel, number);
        gfx_text(Margin + 72, y + 5, 28, text, p->steps[i]);
    }
    gfx_round_rect(Margin, 532, GFX_W - Margin * 2, 68, 18, panel);
    gfx_text(Margin + 24, 553, 23, dim, p->key);
    gfx_rect(0, Footer, GFX_W, 1, RGBA(0x36, 0x36, 0x36, 0xFF));
    if (page > 0) gfx_button(Margin, Footer + 20, "ZL", chinese ? "上一页" : "Previous");
    if (page + 1 < PageCount) gfx_button(856, Footer + 20, "ZR", chinese ? "下一页" : "Next");
    gfx_button(1090, Footer + 20, "B", chinese ? "返回" : "Back");
}

u64 guide_touch_action(int x, int y, int page)
{
    page = page_index(page);
    if (x >= 1056 && x < 1224 && y >= 40 && y < 84) return HidNpadButton_StickR;
    if (y < Footer + 8 || y >= Footer + 64) return 0;
    if (x >= 48 && x < 268 && page > 0) return HidNpadButton_ZL;
    if (x >= 844 && x < 1054 && page + 1 < PageCount) return HidNpadButton_ZR;
    if (x >= 1074 && x < 1232) return HidNpadButton_B;
    return 0;
}
