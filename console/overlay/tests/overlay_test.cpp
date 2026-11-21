#include <cassert>
#include <algorithm>
#include <vector>
#include "../../../common/direct_room_metadata.h"
#define main overlay_entry_main
#include "../source/main.cpp"
#undef main
static u32 enabled=1,logging=0,protocol=5,fail_command=0;
static u32 unknown_command=0;
static u32 storage_selection=0;
static u32 storage_health=0;
static bool present=true,ignore_set=false;static int opens=0,sets=0;static u64 ticks=1;
static LanywhereOverlayStatus status{};
static LanywhereOverlayMatch match{};
static RoomPasscodeHostState passcode_host{};
static NodeHostInfo host_node{};
static NodeActiveRouteInfo active_route{};
static NodeDynamicCatalog node_catalog{};
static NodeDynamicEntry last_node{};
static NodeDynamicEntry host_identity{};
static bool identity_missing=false;
static NodeHostTaskStatus backend_task{1,NodeHostTaskIdle,0,0,0,{}};
static Result task_start_result=0;
static u64 next_task_id=0;
static unsigned node_select_count=0;
static RoomAccessChange last_access{};
static u32 share_status = RoomPasscodeOk;
static unsigned share_count = 0;
static std::vector<u32>commands;
static std::vector<NodeRoomWire> public_rooms;
static NodeRoomSelection last_room{};
static UnifiedRoomRequest last_gateway{};
static unsigned select_count=0, gateway_count=0;
static u32 room_snapshot=4;
static bool versioned_rooms=false, version_target_supported=false, version_installed=false;
static char remote_version[ROOM_GAME_VERSION_SIZE]{}, local_version[ROOM_GAME_VERSION_SIZE]{};
static u64 installed_app_id=0, installed_lcid=0;
static u64 alternate_app_id=0;
static bool alternate_installed=false, alternate_check_failed=false;
static RoomGameVersionTarget version_target{};
static RoomGameVersionUpdate last_version{};
static unsigned published_versions=0, version_reads=0;
struct TestThread { void (*entry)(void *); void *arg; bool started, finished; };
static void runThread(Thread *thread) {
    auto *work = static_cast<TestThread *>(thread->impl);
    assert(work && work->started);
    if (work->finished) return;
    work->entry(work->arg);
    work->finished = true;
}
extern "C" {
Result mockDispatch(Service*,u32 command,const void*in,size_t in_size,void*out,size_t size,MockParams params){
 commands.push_back(command);
 if(unknown_command && command==unknown_command)return MAKERESULT(10,221);
 if(command==fail_command){if(out)std::memset(out,0xff,size);return 1;}
 if(command==65000){params.out_objects->active=1;return 0;}
 if(command==65001){std::memset(out,'v',size);return 0;}
 if(command==65002||command==65004){if(size==1){*(u8*)out=present;return 0;}*(u32*)out=command==65002?logging:enabled;return 0;}
 if(command==65100){*(u8*)out=present;return 0;}
 if(command==65003||command==65005){++sets;if(!ignore_set)(command==65003?logging:enabled)=*(const u32*)in;return 0;}
 if(command==10){*(u32*)out=protocol;return 0;}
 if(command==46){*(u32*)out=storage_selection;return 0;}
 if(command==RC_STORAGE_HEALTH_IPC){*(u32*)out=storage_health;return 0;}
 if(command==NODE_GET_HOST_INFO_IPC){assert(params.buffers[0].size==sizeof(host_node));std::memcpy(params.buffers[0].ptr,&host_node,sizeof(host_node));return 0;}
 if(command==NODE_GET_HOST_IDENTITY_IPC){if(identity_missing)return 1;assert(params.buffers[0].size==sizeof(host_identity));std::memcpy(params.buffers[0].ptr,&host_identity,sizeof(host_identity));return 0;}
 if(command==NODE_GET_ACTIVE_ROUTE_IPC){assert(params.buffers[0].size==sizeof(active_route));std::memcpy(params.buffers[0].ptr,&active_route,sizeof(active_route));return 0;}
 if(command==NODE_GET_CATALOG_IPC){assert(params.buffers[0].size==sizeof(node_catalog));std::memcpy(params.buffers[0].ptr,&node_catalog,sizeof(node_catalog));return 0;}
 if(command==ROOM_GAME_VERSION_LIST_IPC||command==DIRECT_ROOM_LIST_IPC||command==37){
     const bool with_versions=command==ROOM_GAME_VERSION_LIST_IPC;
     if(with_versions&&!versioned_rooms)return MAKERESULT(10,221);
     assert(in_size==sizeof(NodeRoomsRequest)&&size==sizeof(UnifiedPage));
     assert(params.buffers[0].size==NODE_LOBBY_PAGE_SIZE*(with_versions?sizeof(VersionedNodeRoomWire):sizeof(NodeRoomWire)));
     const auto &request=*static_cast<const NodeRoomsRequest*>(in);
     auto &page=*static_cast<UnifiedPage*>(out);
     if(request.snapshot&&request.snapshot!=room_snapshot){
         page={-1,NODE_LOBBY_END,0,0,0,UnifiedListStale};
         return 0;
     }
     page={0,NODE_LOBBY_END,room_snapshot,1,1,0};
     u32 eligible=0;
     for(const auto &row:public_rooms){
         u64 game_id;
         std::memcpy(&game_id,row.group+8,sizeof(game_id));
         if(!(request.node_mask&(1u<<row.node_id))||(request.game_id&&request.game_id!=game_id))continue;
         if(eligible++<request.offset)continue;
         if(page.count==static_cast<int32_t>(NODE_LOBBY_PAGE_SIZE)){page.next=eligible-1;break;}
         if(with_versions){
             auto &room=static_cast<VersionedNodeRoomWire*>(params.buffers[0].ptr)[page.count++];
             room.room=row;
             std::memcpy(room.game_version,remote_version,sizeof(remote_version));
         }else static_cast<NodeRoomWire*>(params.buffers[0].ptr)[page.count++]=row;
     }
     return 0;
 }
 if(command==ROOM_GAME_VERSION_HOST_IPC){
     if(!version_target_supported)return MAKERESULT(10,221);
     assert(size==sizeof(version_target));std::memcpy(out,&version_target,size);return 0;
 }
 if(command==ROOM_GAME_VERSION_PUBLISH_IPC){
     assert(in_size==sizeof(last_version));
     std::memcpy(&last_version,in,sizeof(last_version));
     assert(last_version.target.application_id==version_target.application_id);
     assert(last_version.target.generation==version_target.generation);
     ++published_versions;return 0;
 }
 if(command==38){
     assert(in_size==sizeof(last_room));
     std::memcpy(&last_room,in,sizeof(last_room));
     ++select_count;return 0;
 }
 if(command==UNIFIED_LOBBY_IPC_SELECT){
     assert(in_size==sizeof(last_gateway));
     std::memcpy(&last_gateway,in,sizeof(last_gateway));
     ++gateway_count;return 0;
 }
 if(command==NODE_START_HOST_TASK_IPC){
     assert(params.buffers[0].size==sizeof(last_node));
     assert(size==sizeof(u64));
     std::memcpy(&last_node,params.buffers[0].ptr,sizeof(last_node));
     ++node_select_count;
     if(task_start_result)return task_start_result;
     if(backend_task.state==NodeHostTaskWorking)return 1;
     if(last_node.id>=NODE_CATALOG_COUNT){
         bool found=false;
         for(u32 i=0;i<node_catalog.count;i++)if(node_catalog.nodes[i].id==last_node.id &&
             !std::memcmp(&node_catalog.nodes[i],&last_node,sizeof(last_node)))found=true;
         if(!found)return 1;
     }
     backend_task={1,NodeHostTaskWorking,0,0,++next_task_id,last_node};
     *static_cast<u64*>(out)=backend_task.task_id;
     return 0;
 }
 if(command==NODE_GET_HOST_TASK_IPC){
     assert(in_size==sizeof(u64)&&params.buffers[0].size==sizeof(backend_task));
     u64 requested=*static_cast<const u64*>(in);
     NodeHostTaskStatus reply=backend_task;
     if(requested&&requested!=backend_task.task_id)reply={1,NodeHostTaskExpired,0,0,requested,{}};
     std::memcpy(params.buffers[0].ptr,&reply,sizeof(reply));return 0;
 }
 if(command==3){std::memcpy(out,&status,size);return 0;}if(command==9){std::memcpy(out,&match,size);return 0;}
 if (command == ROOM_PASSCODE_HOST_STATE_IPC) {
     assert(size == sizeof(passcode_host));
     std::memcpy(out, &passcode_host, size);
     return 0;
 }
 if (command == ROOM_PASSCODE_IPC_HOST) {
     RoomPasscodeHost legacy{};
     assert(size == sizeof(legacy));
     legacy.status = passcode_host.status;
     std::memcpy(legacy.room, passcode_host.room, sizeof(legacy.room));
     std::memcpy(out, &legacy, size);
     return 0;
 }
 if (command == ROOM_ACCESS_IPC_SET) {
     assert(in_size == sizeof(last_access) && size == sizeof(u32));
     std::memcpy(&last_access, in, in_size);
     if (share_status == RoomPasscodeOk)
         passcode_host.mode = !last_access.change.passcode[0] ? RoomAccessNone :
             last_access.hidden ? RoomAccessHidden : RoomAccessShared;
     *static_cast<u32 *>(out) = share_status;
     ++share_count;
     return 0;
 }
 assert(false);return 1;
}
bool serviceIsActive(Service*s){return s->active!=0;}void serviceClose(Service*s){s->active=0;}
Result smGetService(Service*s,const char*){++opens;s->active=1;return 0;}
Result smInitialize(){return 0;}void smExit(){}
Result threadCreate(Thread *thread,void (*entry)(void *),void *arg,void*,size_t stack,int priority,int cpu){
    assert(stack==0x4000&&priority==0x2c&&cpu==-2);
    thread->impl=new TestThread{entry,arg,false,false};
    return 0;
}
Result threadStart(Thread *thread){
    static_cast<TestThread*>(thread->impl)->started=true;
    if(g_room_job.action==RoomAction::PublishVersion&&!version_target_supported)runThread(thread);
    return 0;
}
Result threadWaitForExit(Thread *thread){runThread(thread);return 0;}
Result threadClose(Thread *thread){delete static_cast<TestThread*>(thread->impl);thread->impl=nullptr;return 0;}
void svcSleepThread(int64_t){assert(false);}
Result nsInitialize(){return 0;}void nsExit(){}
Result nsIsAnyApplicationEntityInstalled(u64 id,bool *installed){
    if(id==alternate_app_id){
        *installed=alternate_installed;
        return alternate_check_failed?1:0;
    }
    *installed=version_installed&&id==installed_app_id;
    return 0;
}
Result nsGetApplicationControlData(NsApplicationControlSource source,u64 id,NsApplicationControlData *out,size_t size,u64 *used){
    assert(source==NsApplicationControlSource_StorageOnly&&id==installed_app_id&&size==sizeof(*out));
    std::memset(out,0,size);std::memcpy(out->nacp.display_version,local_version,sizeof(local_version));
    out->nacp.local_communication_id[0]=installed_lcid;*used=size;++version_reads;return 0;
}
SmServiceName smEncodeName(const char*){return {1};}TipcService*smGetServiceSessionTipc(){static Service s{1};return &s;}
static SetLanguage mocked_language=SetLanguage_ENUS;
Result setInitialize(){return 0;}void setExit(){}Result setGetSystemLanguage(u64*out){*out=mocked_language;return 0;}Result setMakeLanguage(u64 code,SetLanguage*out){*out=static_cast<SetLanguage>(code);return 0;}
u64 armGetSystemTick(){return ticks;}u64 armGetSystemTickFreq(){return 1000;}
}
void reset(){g_node_task={};g_last_handled_node_task=0;enabled=1;logging=0;protocol=5;storage_selection=storage_health=0;task_start_result=0;next_task_id=0;backend_task={1,NodeHostTaskIdle,0,0,0,{}};fail_command=unknown_command=0;present=true;ignore_set=identity_missing=false;opens=sets=0;ticks=1;status={};match={};host_node={};host_node.version=1;host_node.status=NodeHostReady;host_identity=builtinNode(0);host_node.port=host_identity.port;std::strcpy(host_node.host,host_identity.host);std::strcpy(host_node.name,"Main server");active_route={};active_route.version=1;node_catalog={};node_catalog.version=1;last_node={};node_select_count=0;commands.clear();g_chinese=false;closeServices();}

static tsl::elm::ListItem *findItem(tsl::elm::Element *tree, const char *text) {
    auto *frame = dynamic_cast<tsl::elm::OverlayFrame *>(tree);
    assert(frame);
    auto *list = dynamic_cast<tsl::elm::List *>(frame->content);
    assert(list);
    for (auto *element : list->items) {
        auto *item = dynamic_cast<tsl::elm::ListItem *>(element);
        if (item && item->text == text) return item;
    }
    return nullptr;
}

static NodeRoomWire roomRow(u32 node, u32 flags, u8 access, const char *title, u8 players=1,
                            u64 game_id=0x0100152000022000) {
    NodeRoomWire row{};
    std::memcpy(row.group,"ABC245",6);
    std::memcpy(row.group+8,&game_id,sizeof(game_id));
    std::memcpy(row.group+16,&game_id,sizeof(game_id));
    row.group[32]=players;row.group[33]=8;row.group[34]=1;row.group[35]=access;
    std::strcpy(reinterpret_cast<char *>(row.group)+80,title);
    std::strcpy(reinterpret_cast<char *>(row.group)+208,"马力欧卡丁车8 豪华版");
    row.node_id=node;row.flags=flags;
    std::memcpy(row.locator.code,row.group,sizeof(row.locator.code));
    if(node>=2&&flags==NodeRoomDirect){
        row.locator.flavor=NodeFlavorStandard;row.locator.host_ip=0x0a0d0101;
        std::memcpy(row.locator.network_id,&game_id,sizeof(game_id));
        row.locator.network_id[16]=1;row.locator.bssid[0]=2;
        if(node>=NODE_CATALOG_COUNT){row.locator.reserved[0]=8;row.locator.reserved[3]=8;}
    }
    return row;
}

static void testRoomSelector() {
    reset();connectServices();
    public_rooms={roomRow(11,NodeRoomDirect,RoomAccessNone,"Mario Kart 8 Deluxe")};
    MainGui main;std::unique_ptr<tsl::elm::Element> main_ui(main.createUI());
    auto *join=findItem(main_ui.get(),"Join room");
    assert(join&&join->onClick(HidNpadButton_A));
    auto &page=tsl::nextGui;assert(page);
    const auto dispatches=commands.size();
    std::unique_ptr<tsl::elm::Element> ui(page->createUI());
    assert(roomJobRunning()&&commands.size()==dispatches);
    assert(!page->handleInput(HidNpadButton_B,0,{},{},{}));
    Overlay overlay;tsl::Overlay::test_instance=&overlay;
    overlay.onHide();overlay.onShow();assert(commands.size()==dispatches);
    runThread(&g_room_worker.thread);page->update();
    assert(!roomJobRunning()&&!g_room_worker.created);
    auto *room=findItem(ui.get(),"Mario Kart 8 Deluxe");
    assert(room&&room->value=="Olunira · 1/8");

    status.state=2;
    assert(room->onClick(HidNpadButton_A)&&!select_count);
    runThread(&g_room_worker.thread);page->update();
    assert(findItem(ui.get(),"Leave the room and stop matching first.")&&!select_count);
    status.state=0;
    match.state=1;
    assert(room->onClick(HidNpadButton_A));
    runThread(&g_room_worker.thread);page->update();
    assert(findItem(ui.get(),"Leave the room and stop matching first.")&&!select_count);
    match.state=0;
    // Changed identity with the same room code is not a replacement target.
    public_rooms[0].locator.network_id[16]=5;
    assert(room->onClick(HidNpadButton_A));
    runThread(&g_room_worker.thread);page->update();
    assert(findItem(ui.get(),"Room list changed. Refresh rooms.")&&!select_count);
    assert(findItem(ui.get(),"Refresh rooms")->onClick(HidNpadButton_A));
    runThread(&g_room_worker.thread);page->update();

    room=findItem(ui.get(),"Mario Kart 8 Deluxe");
    const auto expected=public_rooms[0].locator;
    assert(room&&room->onClick(HidNpadButton_A)&&!select_count);
    assert(page->handleInput(HidNpadButton_B,0,{},{},{}));
    runThread(&g_room_worker.thread);page->update();
    assert(select_count==1&&last_room.node_id==11&&last_room.game_id==0x0100152000022000);
    assert(!std::memcmp(&last_room.locator,&expected,sizeof(expected)));
    assert(!node_select_count&&!gateway_count);
    assert(findItem(ui.get(),"Search in Local Wireless."));
    assert(findItem(ui.get(),"Return to game")->onClick(HidNpadButton_A)&&overlay.hidden);

    public_rooms={roomRow(0,NodeRoomDirect,RoomAccessHidden,"Hidden room"),
        roomRow(0,NodeRoomDirect,RoomAccessNone,"Full room",8),
        roomRow(11,NodeRoomGateway,RoomAccessNone,"Gateway game"),
        roomRow(11,NodeRoomDirect,RoomAccessHidden,"Password room")};
    assert(findItem(ui.get(),"Refresh rooms")->onClick(HidNpadButton_A));
    runThread(&g_room_worker.thread);page->update();
    assert(findItem(ui.get(),"Hidden room")->onClick(HidNpadButton_A)&&!roomJobRunning());
    assert(findItem(ui.get(),"Unlock this room in RoomCircuit.")&&select_count==1);
    assert(findItem(ui.get(),"Full room")->onClick(HidNpadButton_A)&&!roomJobRunning());
    assert(findItem(ui.get(),"Room is full.")&&select_count==1);
    assert(findItem(ui.get(),"Gateway game")->onClick(HidNpadButton_A));
    runThread(&g_room_worker.thread);page->update();
    assert(gateway_count==1&&!std::strcmp(last_gateway.provider,"lanplay-cn-olunira"));
    assert(!std::strcmp(last_gateway.code,"ABC245")&&last_gateway.game_id==last_room.game_id);
    assert(findItem(ui.get(),"Refresh rooms")->onClick(HidNpadButton_A));
    runThread(&g_room_worker.thread);page->update();
    assert(findItem(ui.get(),"Password room")->onClick(HidNpadButton_A));
    runThread(&g_room_worker.thread);page->update();
    assert(select_count==2&&findItem(ui.get(),"Search and enter the password in game."));

    // Pagination and stale snapshots never select an old room.
    public_rooms.assign(7,roomRow(11,NodeRoomDirect,RoomAccessNone,"Mario Kart 8 Deluxe"));
    assert(findItem(ui.get(),"Refresh rooms")->onClick(HidNpadButton_A));
    runThread(&g_room_worker.thread);page->update();
    auto *next=findItem(ui.get(),"Next page");
    assert(next&&next->onClick(HidNpadButton_A));
    ++room_snapshot;
    runThread(&g_room_worker.thread);page->update();
    assert(findItem(ui.get(),"Room list changed. Refresh rooms.")&&select_count==2);
    assert(findItem(ui.get(),"Refresh rooms")->onClick(HidNpadButton_A));
    runThread(&g_room_worker.thread);page->update();
    assert(findItem(ui.get(),"Next page")->onClick(HidNpadButton_A));
    runThread(&g_room_worker.thread);page->update();
    assert(findItem(ui.get(),"Previous page")&&!findItem(ui.get(),"Next page"));

    // The new directory opcode is optional, but a malformed response is not usable.
    Service service{1};NodeRoomsRequest request{};request.node_mask=1u<<11;
    UnifiedPage info{};OverlayPublicRoom rooms[NODE_LOBBY_PAGE_SIZE]{};
    unknown_command=DIRECT_ROOM_LIST_IPC;
    assert(R_SUCCEEDED(lanywhereOverlayListRooms(&service,&request,&info,rooms))&&info.count==6);
    public_rooms[0].locator.code[0]='O';
    assert(R_FAILED(lanywhereOverlayListRooms(&service,&request,&info,rooms))&&info.count==-1&&!rooms[0].selection.game_id);
    unknown_command=0;

    g_chinese=true;
    public_rooms={roomRow(11,NodeRoomDirect,RoomAccessNone,"Mario Kart 8 Deluxe")};
    RoomJoinGui localized;std::unique_ptr<tsl::elm::Element> localized_ui(localized.createUI());
    runThread(&g_room_worker.thread);localized.update();
    assert(findItem(localized_ui.get(),"马力欧卡丁车8 豪华版"));
    assert(findItem(localized_ui.get(),"刷新房间")->onClick(HidNpadButton_A)&&roomJobRunning());
    // Unload waits for the worker before closing the independent session.
    overlay.exitServices();
    assert(!roomJobRunning()&&!g_room_worker.created&&!serviceIsActive(&g_relay));
    tsl::Overlay::test_instance=nullptr;
}

static void testRoomVersions() {
    reset();connectServices();
    constexpr u64 mario=0x0100152000022000;
    public_rooms={roomRow(11,NodeRoomDirect,RoomAccessNone,"Mario Kart 8 Deluxe")};
    versioned_rooms=true;version_installed=true;installed_app_id=installed_lcid=mario;
    std::strcpy(remote_version,"4.0.1");std::strcpy(local_version,"4.0.0");
    RoomJoinGui chooser;std::unique_ptr<tsl::elm::Element> ui(chooser.createUI());
    runThread(&g_room_worker.thread);chooser.update();
    auto *room=findItem(ui.get(),"Mario Kart 8 Deluxe · 4.0.1");
    const auto before=select_count;
    assert(room&&room->onClick(HidNpadButton_A));
    runThread(&g_room_worker.thread);chooser.update();
    assert(findItem(ui.get(),"Game updates incompatible.")&&select_count==before&&version_reads==1);
    installed_lcid=1;
    assert(room->onClick(HidNpadButton_A));
    runThread(&g_room_worker.thread);chooser.update();
    assert(findItem(ui.get(),"Search in Local Wireless.")&&select_count==before+1);

    constexpr u64 scarlet=0x0100A3D008C5C000, violet=0x01008F6008C5E000;
    installed_app_id=installed_lcid=scarlet;
    alternate_app_id=violet;
    std::strcpy(remote_version,"3.0.1");std::strcpy(local_version,"3.0.0");
    public_rooms={roomRow(11,NodeRoomDirect,RoomAccessNone,"Pokémon Scarlet",1,scarlet)};
    assert(findItem(ui.get(),"Refresh rooms")->onClick(HidNpadButton_A));
    runThread(&g_room_worker.thread);chooser.update();
    room=findItem(ui.get(),"Pokémon Scarlet · 3.0.1");
    assert(room&&room->onClick(HidNpadButton_A));
    runThread(&g_room_worker.thread);chooser.update();
    assert(findItem(ui.get(),"Game updates incompatible.")&&select_count==before+1);
    alternate_installed=true;
    assert(room->onClick(HidNpadButton_A));
    runThread(&g_room_worker.thread);chooser.update();
    assert(findItem(ui.get(),"Search in Local Wireless.")&&select_count==before+2);
    alternate_installed=false;alternate_check_failed=true;
    assert(findItem(ui.get(),"Refresh rooms")->onClick(HidNpadButton_A));
    runThread(&g_room_worker.thread);chooser.update();
    assert(findItem(ui.get(),"Pokémon Scarlet · 3.0.1")->onClick(HidNpadButton_A));
    runThread(&g_room_worker.thread);chooser.update();
    assert(findItem(ui.get(),"Search in Local Wireless.")&&select_count==before+3);

    // The host is identified by the live ProgramID, even if both games are installed.
    alternate_installed=true;alternate_check_failed=false;
    status.state=2;status.my_index=0;status.game_id=scarlet;
    version_target_supported=true;version_target={scarlet,17};
    refresh();
    MainGui main;std::unique_ptr<tsl::elm::Element> main_ui(main.createUI());
    assert(roomJobRunning()&&!published_versions);
    runThread(&g_room_worker.thread);main.update();
    assert(published_versions==1&&!std::strcmp(last_version.game_version,"3.0.0"));
    ticks+=2000;main.update();runThread(&g_room_worker.thread);main.update();
    assert(published_versions==1);
    version_target.generation=18;
    ticks+=2000;main.update();runThread(&g_room_worker.thread);main.update();
    assert(published_versions==2&&last_version.target.generation==18);
    closeServices();
    version_target_supported=versioned_rooms=version_installed=false;
    alternate_app_id=0;alternate_installed=false;
}

static void finishBackendTask(bool succeed=true) {
    assert(backend_task.state==NodeHostTaskWorking);
    const auto &node=backend_task.identity;
    if(status.state==1||status.state==2||match.state==1||match.state==2||match.state==3||match.state==4||match.state==5)
        succeed=false;
    if(node.id>=NODE_CATALOG_COUNT){
        bool found=false;
        for(u32 i=0;i<node_catalog.count;i++)if(!std::memcmp(&node_catalog.nodes[i],&node,sizeof(node)))found=true;
        if(!found)succeed=false;
    }
    backend_task.result=succeed?0:1;
    if(succeed){
        host_node.version=1;host_node.node_id=node.id;host_node.port=node.port;host_node.status=NodeHostReady;
        host_identity=node;identity_missing=false;
        std::snprintf(host_node.name,sizeof(host_node.name),"%s",node.name);
        std::snprintf(host_node.host,sizeof(host_node.host),"%.91s",node.host);
    }
    backend_task.state=NodeHostTaskFinished;
}

static void testNodeSelector() {
    reset();connectServices();
    node_catalog.count=1;
    node_catalog.nodes[0].id=13;node_catalog.nodes[0].protocol=NodeProtocolLanPlay;
    node_catalog.nodes[0].region=NodeRegionEurope;node_catalog.nodes[0].port=11451;node_catalog.nodes[0].flags=1;
    std::strcpy(node_catalog.nodes[0].name,"Russia LAN");std::strcpy(node_catalog.nodes[0].host,"play.example.org");
    MainGui main;std::unique_ptr<tsl::elm::Element>main_ui(main.createUI());
    auto *hosting=findItem(main_ui.get(),"Hosting node");assert(hosting&&hosting->onClick(HidNpadButton_A));
    auto &page=tsl::nextGui;assert(page);
    std::unique_ptr<tsl::elm::Element>ui(page->createUI());
    auto *node=findItem(ui.get(),"Jaysea · Node");assert(node);
    status.state=2;
    assert(node->onClick(HidNpadButton_A)&&findItem(ui.get(),"Leave the room and stop matching first."));
    assert(node_select_count==0&&!g_node_task.tracked);
    status.state=0;active_route.status=NodeRouteConnected;std::strcpy(active_route.name,"Current room");
    assert(node->onClick(HidNpadButton_A)&&findItem(ui.get(),"Leave the room and stop matching first."));
    assert(node_select_count==0&&!g_node_task.tracked);active_route.status=NodeRouteNone;
    status.state=0;task_start_result=1;
    assert(node->onClick(HidNpadButton_A)&&findItem(ui.get(),"Selection failed. Try again."));
    assert(!g_node_task.tracked);task_start_result=0;
    unknown_command=NODE_START_HOST_TASK_IPC;
    assert(node->onClick(HidNpadButton_A)&&findItem(ui.get(),"Update the background module."));
    assert(!g_node_task.tracked);unknown_command=0;
    const unsigned initial_count=node_select_count;
    assert(node->onClick(HidNpadButton_A));
    assert(backend_task.state==NodeHostTaskWorking&&g_node_task.tracked&&g_node_task.id==backend_task.task_id);
    page->update();assert(findItem(ui.get(),"Checking…"));
    assert(!page->handleInput(HidNpadButton_B,0,{},{},{}));
    {Overlay hidden;hidden.onHide();assert(backend_task.state==NodeHostTaskWorking);}
    finishBackendTask();ticks+=1000;page->update();
    const NodeDynamicEntry expected=builtinNode(2);
    assert(!std::memcmp(&last_node,&expected,sizeof(expected))&&node_select_count==initial_count+1);
    assert(findItem(ui.get(),"Server selected.")&&g_host_node.node_id==2);
    // A disconnected idle relay must allow choosing another hosting server.
    status.state=3;assert(node->onClick(HidNpadButton_A));finishBackendTask();ticks+=1000;page->update();
    assert(findItem(ui.get(),"Server selected.")&&node_select_count==initial_count+2);status.state=0;
    // The displayed dynamic identity must still match the backend when chosen.
    auto *custom=findItem(ui.get(),"Russia LAN");assert(custom);
    assert(custom->onClick(HidNpadButton_A));std::strcpy(node_catalog.nodes[0].host,"changed.example.org");
    finishBackendTask();ticks+=1000;page->update();
    assert(findItem(ui.get(),"Selection failed. Try again.")&&g_host_node.node_id==2);
    // If a game starts during the probe the backend rejects the late commit.
    assert(node->onClick(HidNpadButton_A));status.state=2;finishBackendTask();ticks+=1000;page->update();
    assert(findItem(ui.get(),"Selection failed. Try again."));status.state=0;
    // Close and reopen while the backend task continues independently.
    assert(node->onClick(HidNpadButton_A));const u64 pending=backend_task.task_id;
    ui.reset();page.reset();Overlay overlay;overlay.exitServices();
    assert(backend_task.state==NodeHostTaskWorking&&!serviceIsActive(&g_relay));
    g_node_task={};g_last_handled_node_task=0;connectServices();
    NodeSelectGui reopened;std::unique_ptr<tsl::elm::Element>reopened_ui(reopened.createUI());
    assert(g_node_task.tracked&&g_node_task.id==pending&&findItem(reopened_ui.get(),"Checking…"));
    finishBackendTask();ticks+=1000;reopened.update();assert(findItem(reopened_ui.get(),"Server selected."));
    // A different task cannot be attributed to the old task.
    assert(findItem(reopened_ui.get(),"Jaysea · Node")->onClick(HidNpadButton_A));
    backend_task.task_id=++next_task_id;backend_task.identity=builtinNode(3);
    ticks+=1000;reopened.update();assert(findItem(reopened_ui.get(),"Server check expired. Try again."));
    NodeHostTaskStatus expired{};Service session{1};
    assert(R_SUCCEEDED(lanywhereOverlayGetNodeTask(&session,pending,&expired))&&
        expired.state==NodeHostTaskExpired&&expired.task_id==pending);
    // Discover a task created by a different client; never start over it.
    NodeSelectGui foreign;std::unique_ptr<tsl::elm::Element>foreign_ui(foreign.createUI());
    assert(g_node_task.tracked&&g_node_task.id==backend_task.task_id&&findItem(foreign_ui.get(),"Checking…"));
    const unsigned before_busy=node_select_count;
    assert(findItem(foreign_ui.get(),"Jaysea · Node")->onClick(HidNpadButton_A));
    assert(node_select_count==before_busy);
    finishBackendTask(false);ticks+=1000;foreign.update();
    assert(findItem(foreign_ui.get(),"Selection failed. Try again."));
    // A same-ID response with different identity is not our success.
    assert(findItem(foreign_ui.get(),"Jaysea · Node")->onClick(HidNpadButton_A));
    std::strcpy(backend_task.identity.host,"replaced.example.org");
    ticks+=1000;foreign.update();assert(findItem(foreign_ui.get(),"Server check expired. Try again."));
    reset();connectServices();g_chinese=true;
    NodeSelectGui chinese;std::unique_ptr<tsl::elm::Element>zh(chinese.createUI());
    assert(findItem(zh.get(),"主服务器")&&findItem(zh.get(),"社区 · LANywhere"));
    status.state=2;assert(findItem(zh.get(),"主服务器")->onClick(HidNpadButton_A));
    assert(findItem(zh.get(),"请先退出房间并停止匹配。"));
}

static void testNodeCatalogFailures() {
    reset();connectServices();node_catalog.count=2;
    NodeDynamicEntry shared{};shared.id=13;shared.protocol=NodeProtocolLanPlay;shared.region=NodeRegionEurope;
    shared.port=11451;shared.flags=1;std::strcpy(shared.name,"Shared LAN");std::strcpy(shared.host,"shared.example.org");
    NodeDynamicEntry local=shared;local.id=NODE_LOCAL_BASE;std::strcpy(local.name,"Local LAN");
    std::strcpy(local.host,"local.example.org");node_catalog.nodes[0]=shared;node_catalog.nodes[1]=local;
    NodeSelectGui page;std::unique_ptr<tsl::elm::Element>ui(page.createUI());
    auto *shared_row=findItem(ui.get(),"Shared LAN"),*local_row=findItem(ui.get(),"Local LAN");
    assert(shared_row&&local_row);
    fail_command=NODE_GET_CATALOG_IPC;ticks+=3000;page.update();
    assert(local_row->value=="Unavailable"&&shared_row->value=="Unavailable");
    assert(local_row->onClick(HidNpadButton_A)&&findItem(ui.get(),"Custom nodes unavailable. Check configuration."));
    assert(node_select_count==0);
    fail_command=0;node_catalog.enabled=NODE_LOCAL_CONFIG_INVALID;node_catalog.count=1;ticks+=3000;page.update();
    assert(local_row->value=="Unavailable"&&shared_row->value!="Unavailable");
    assert(findItem(ui.get(),"Local nodes unavailable. Check nodes.ini."));
    assert(local_row->onClick(HidNpadButton_A)&&node_select_count==0);
    assert(shared_row->onClick(HidNpadButton_A));finishBackendTask();ticks+=1000;page.update();
    assert(findItem(ui.get(),"Server selected.")&&g_host_node.node_id==13);
    // New nodes arriving after the asynchronous catalog load appear without reopening.
    node_catalog.enabled=0;node_catalog.nodes[1]=local;node_catalog.count=2;
    NodeDynamicEntry added=shared;added.id=14;std::strcpy(added.name,"New shared");
    node_catalog.nodes[2]=added;node_catalog.count=3;ticks+=3000;page.update();
    assert(findItem(ui.get(),"New shared"));
}

static void testFullNodeIdentity() {
    reset();connectServices();
    NodeDynamicEntry node{};node.id=13;node.protocol=NodeProtocolLanPlay;node.region=NodeRegionEurope;
    node.port=11451;node.flags=1;std::strcpy(node.name,"Long host");
    const std::string prefix=std::string(50,'a')+"."+std::string(40,'b')+".";
    std::snprintf(node.host,sizeof(node.host),"%sx",prefix.c_str());
    host_identity=node;host_node.node_id=13;host_node.port=node.port;
    std::snprintf(host_node.host,sizeof(host_node.host),"%.91s",node.host);std::strcpy(host_node.name,node.name);
    std::snprintf(node.host,sizeof(node.host),"%sy",prefix.c_str());
    node_catalog.count=1;node_catalog.nodes[0]=node;refresh();
    NodeSelectGui long_host;std::unique_ptr<tsl::elm::Element>first(long_host.createUI());
    assert(findItem(first.get(),"Long host")&&findItem(first.get(),"Long host")->value!="Current");
    node=host_identity;node.ipv4[0]=1;node.ipv4[1]=2;node.ipv4[2]=3;node.ipv4[3]=4;
    node_catalog.nodes[0]=node;
    NodeSelectGui changed_pin;std::unique_ptr<tsl::elm::Element>second(changed_pin.createUI());
    assert(findItem(second.get(),"Long host")&&findItem(second.get(),"Long host")->value!="Current");
    unknown_command=NODE_GET_HOST_IDENTITY_IPC;refresh();
    NodeSelectGui unsupported;std::unique_ptr<tsl::elm::Element>third(unsupported.createUI());
    assert(findItem(third.get(),"Update the background module."));
    auto *main=findItem(third.get(),"Main server");assert(main&&main->value!="Current");
    assert(main->onClick(HidNpadButton_A)&&findItem(third.get(),"Update the background module."));
    assert(node_select_count==0);
    unknown_command=0;identity_missing=true;refresh();
    NodeSelectGui recover;std::unique_ptr<tsl::elm::Element>fourth(recover.createUI());
    auto *replacement=findItem(fourth.get(),"Main server");assert(replacement&&replacement->onClick(HidNpadButton_A));
    finishBackendTask();ticks+=1000;recover.update();assert(findItem(fourth.get(),"Server selected."));
    // Older backends cannot run the task protocol; never fall back to IPC36/47.
    unknown_command=NODE_GET_HOST_TASK_IPC;refresh();
    NodeSelectGui old;std::unique_ptr<tsl::elm::Element>fifth(old.createUI());
    assert(findItem(fifth.get(),"Update the background module."));
    const unsigned before_old=node_select_count;
    assert(findItem(fifth.get(),"Main server")->onClick(HidNpadButton_A));
    assert(node_select_count==before_old&&findItem(fifth.get(),"Update the background module."));
}

static void testStorageHealth() {
    reset();storage_selection=3;storage_health=RC_STORAGE_HEALTH_LEGACY_CHANGED;status.state=2;connectServices();
    MainGui gui;std::unique_ptr<tsl::elm::Element>ui(gui.createUI());
    assert(findItem(ui.get(),"Older settings changed. Back up and compare."));
    assert(g_snapshot.status.state==2&&g_snapshot.relay_ok);
    storage_health=RC_STORAGE_HEALTH_LEGACY_UNCHECKED;ticks+=1000;gui.update();
    assert(findItem(ui.get(),"Could not check older settings. Back up and compare."));
    unknown_command=RC_STORAGE_HEALTH_IPC;ticks+=1000;gui.update();
    assert(findItem(ui.get(),"Could not check server settings. Try again."));
    storage_selection=1;ticks+=1000;gui.update();assert(g_storage_health==0);
    unknown_command=0;storage_health=RC_STORAGE_HEALTH_BOOTSTRAP_CONFLICT;ticks+=1000;gui.update();
    assert(findItem(ui.get(),"Server settings conflict. Compare config.ini files."));
    storage_selection=3;storage_health=16;ticks+=1000;gui.update();
    assert(g_storage_health==RC_STORAGE_HEALTH_BOOTSTRAP_UNCHECKED);
}

static void testNodeTaskProtocol() {
    reset();Service service{1};NodeHostTaskStatus reply{};
    backend_task={1,NodeHostTaskWorking,0,0,42,builtinNode(2)};
    assert(R_SUCCEEDED(lanywhereOverlayGetNodeTask(&service,42,&reply))&&reply.task_id==42);
    backend_task.reserved=1;
    assert(R_FAILED(lanywhereOverlayGetNodeTask(&service,42,&reply))&&reply.version==0);
    backend_task.reserved=0;backend_task.state=4;
    assert(R_FAILED(lanywhereOverlayGetNodeTask(&service,42,&reply))&&reply.version==0);
    backend_task.state=NodeHostTaskWorking;backend_task.task_id=0;
    assert(R_FAILED(lanywhereOverlayGetNodeTask(&service,0,&reply))&&reply.version==0);
    backend_task={1,NodeHostTaskWorking,0,0,43,builtinNode(2)};
    assert(R_SUCCEEDED(lanywhereOverlayGetNodeTask(&service,42,&reply))&&
        reply.state==NodeHostTaskExpired&&reply.task_id==42);
}

static void testPasscodeGui() {
    reset();
    passcode_host = {};
    last_access = {};
    share_status = RoomPasscodeOk;
    share_count = 0;
    connectServices();
    MainGui gui;
    std::unique_ptr<tsl::elm::Element> main_ui(gui.createUI());
    assert(!findItem(main_ui.get(), "Game password"));
    status.state = 2;
    status.game_id = 42;
    std::memcpy(status.code, "ABC123", 6);
    std::memcpy(passcode_host.room, "ABC123", 6);
    status.my_index = 1;
    ticks += 1000;
    gui.update();
    assert(!findItem(main_ui.get(), "Game password"));
    status.my_index = 0;
    ticks += 1000;
    gui.update();
    auto *action = findItem(main_ui.get(), "Game password");
    assert(action && action->value == "Not set");
    unknown_command = ROOM_PASSCODE_HOST_STATE_IPC;
    refresh();
    gui.update();
    assert(action->value == "Unknown"); // An older module still permits editing.
    unknown_command = 0;
    assert(action && action->onClick(HidNpadButton_A));
    auto &editor = tsl::nextGui;
    assert(editor && share_count == 0);
    std::unique_ptr<tsl::elm::Element> edit_ui(editor->createUI());
    assert(findItem(edit_ui.get(), "Enter the game password"));
    const auto press = [&](u64 key) {
        assert(editor->handleInput(key, 0, {}, {}, {}));
    };
    press(HidNpadButton_Plus); // An empty draft never clears the public password.
    assert(share_count == 0);
    press(HidNpadButton_A); // 0
    press(HidNpadButton_A); // 0
    press(HidNpadButton_R);
    press(HidNpadButton_A); // A
    press(HidNpadButton_R);
    press(HidNpadButton_A); // a
    press(HidNpadButton_R);
    press(HidNpadButton_A); // space, then delete it
    press(HidNpadButton_Y);
    press(HidNpadButton_AnyRight);
    press(HidNpadButton_A); // !
    assert(findItem(edit_ui.get(), "[00Aa!]"));
    press(HidNpadButton_Plus);
    assert(share_count == 0 && findItem(edit_ui.get(), "A  Share in lobby") &&
           findItem(edit_ui.get(), "X  Keep password hidden"));
    press(HidNpadButton_A | HidNpadButton_X);
    assert(share_count == 0);
    press(HidNpadButton_B);
    assert(share_count == 0 && findItem(edit_ui.get(), "[00Aa!]"));
    press(HidNpadButton_Plus);
    press(HidNpadButton_A);
    assert(share_count == 1);
    assert(std::memcmp(last_access.change.room, "ABC123", 6) == 0);
    assert(std::strcmp(last_access.change.passcode, "00Aa!") == 0);
    assert(last_access.hidden == 0 && last_access.reserved == 0);
    assert(findItem(edit_ui.get(), "Password shared in lobby."));
    assert(!findItem(edit_ui.get(), "[00Aa!]"));
    gui.update();
    assert(action->value == "Shared");
    g_chinese = true;
    gui.update();
    assert(action->value == "已分享");
    g_chinese = false;
    press(HidNpadButton_Minus);
    assert(share_count == 2 && last_access.change.passcode[0] == 0 && last_access.hidden == 0);
    gui.update();
    assert(action->value == "Not set");

    press(HidNpadButton_R); // Digits again.
    for (size_t i = 0; i < ROOM_PASSCODE_MAX; ++i) press(HidNpadButton_A);
    press(HidNpadButton_A); // Cannot append a 25th character.
    assert(findItem(edit_ui.get(), "Maximum 24 characters."));
    share_status = RoomPasscodeBusy;
    press(HidNpadButton_Plus);
    assert(share_count == 2);
    press(HidNpadButton_X);
    assert(share_count == 3 && last_access.hidden == 1 &&
           findItem(edit_ui.get(), "Try again in a moment."));
    gui.update();
    assert(action->value == "Not set");
    for (char c : last_access.change.passcode) assert(c == '0');
    share_status = RoomPasscodeOk;
    press(HidNpadButton_X);
    assert(share_count == 4 && last_access.hidden == 1 && findItem(edit_ui.get(), "Password hidden."));
    gui.update();
    assert(action->value == "Hidden");

    fail_command = ROOM_ACCESS_IPC_SET;
    press(HidNpadButton_A);
    press(HidNpadButton_Plus);
    press(HidNpadButton_A);
    assert(share_count == 4 && findItem(edit_ui.get(), "Could not save password."));
    assert(std::find(commands.begin(), commands.end(), ROOM_PASSCODE_IPC_SET) == commands.end());
    fail_command = 0;

    std::memcpy(status.code, "ABC124", 6);
    std::memcpy(passcode_host.room, "ABC124", 6);
    passcode_host.mode = RoomAccessNone;
    press(HidNpadButton_A);
    assert(share_count == 4 && findItem(edit_ui.get(), "Room closed or changed."));
    gui.update();
    assert(action->value == "Not set");
    press(HidNpadButton_B);
    assert(!editor->handleInput(HidNpadButton_B, 0, {}, {}, {}));
    edit_ui.reset();
    editor.reset();
    status.my_index = 1;
    ticks += 1000;
    gui.update();
    assert(!findItem(main_ui.get(), "Game password"));
}

static void testFrameworkLanguage() {
    reset(); storage_selection = 3;
    ult::BACK = "返回"; ult::OK = "确定"; g_chinese = true;
    Overlay overlay; overlay.initServices();
    assert(tsl::hlp::sdCardMounted);
    assert(!g_chinese && ult::BACK == "Back" && ult::OK == "OK" && ult::ON == "On");
    mocked_language = SetLanguage_ZHHANT;
    ult::BACK = "Back"; ult::OK = "OK";
    const unsigned swaps = tsl::swapCalls;
    overlay.onShow();
    assert(tsl::hlp::sdCardMounted);
    assert(g_chinese && ult::BACK == "返回" && ult::OK == "确定" && ult::ON == "开启");
    assert(tsl::swapCalls == swaps + 1 && tsl::nextGui);
    mocked_language = SetLanguage_ENUS;
    overlay.onHide(); overlay.onShow();
    assert(tsl::hlp::sdCardMounted);
    assert(!g_chinese && ult::BACK == "Back" && ult::OK == "OK" && ult::ON == "On");
    assert(tsl::swapCalls == swaps + 2);
    overlay.exitServices(); tsl::nextGui.reset();
}

int main(){
 reset();connectServices();assert(g_snapshot.config_ok&&g_snapshot.relay_ok&&g_snapshot.enabled&&opens==2);
 // The actual IPC reader terminates a completely non-NUL 32-byte module version.
 assert(g_version[31]==0&&std::strlen(g_version)==31);
 SafeToggle plugin(false),log(true);assert(!plugin.onClick(2)&&sets==0);assert(plugin.onClick(1)&&enabled==0&&sets==1&&!plugin.getState());
 assert(plugin.onClick(1)&&enabled==1&&sets==2&&plugin.getState());
 for(u8 state:{u8(1),u8(2),u8(3)}){status.state=state;int before=sets;plugin.onClick(1);assert(sets==before&&enabled==1);}
 status.state=0;
 for(u8 state:{u8(1),u8(2),u8(3),u8(4),u8(5)}){match.state=state;int before=sets;plugin.onClick(1);assert(sets==before);}
 match.state=0;fail_command=3;int before=sets;plugin.onClick(1);assert(sets==before&&!g_snapshot.relay_ok&&g_snapshot.status.state==0);
 fail_command=65005;plugin.onClick(1);assert(enabled==1&&plugin.getState()&&g_notice.find("failed")!=std::string::npos);
 fail_command=0;ignore_set=true;plugin.onClick(1);assert(plugin.getState()&&g_notice.find("failed")!=std::string::npos);ignore_set=false;
 status.state=2;log.onClick(1);assert(logging==1&&log.getState());
 fail_command=65002;refresh();assert(!g_snapshot.config_ok);before=sets;log.onClick(1);assert(sets==before);
 reset();present=false;connectServices();assert(opens==0&&!g_snapshot.config_ok&&!g_snapshot.relay_ok);
 reset();protocol=999;connectServices();assert(g_protocol_mismatch&&!g_snapshot.relay_ok&&!serviceIsActive(&g_relay));
 reset();connectServices();status.state=2;status.member_count=2;std::memcpy(status.code,"ABCDEF",6);refresh();
 MainGui gui;std::unique_ptr<tsl::elm::Element>ui(gui.createUI());auto*frame=dynamic_cast<tsl::elm::OverlayFrame*>(ui.get());assert(frame&&frame->title=="RoomCircuit");
 auto*list=dynamic_cast<tsl::elm::List*>(frame->content);bool room=false,players=false;for(auto*p:list->items){auto*i=dynamic_cast<tsl::elm::ListItem*>(p);if(!i)continue;room|=i->text=="Room"&&i->value=="ABCDEF";players|=i->text=="Room members"&&i->value=="2 / 8";}assert(room&&players);
 auto *node = findItem(ui.get(), "Hosting node");assert(node && node->value=="Main server");
 auto *route = findItem(ui.get(), "Room node");assert(route && route->value=="—");
 std::strcpy(active_route.name,"Europe LAN");active_route.status=NodeRouteSelected;ticks+=1000;gui.update();assert(route->value=="Europe LAN · Selected");
 active_route.status=NodeRouteConnected;ticks+=1000;gui.update();assert(route->value=="Europe LAN");
 active_route.status=NodeRouteRecovering;ticks+=1000;gui.update();assert(route->value=="Europe LAN · Reconnecting");
 active_route.status=NodeRouteUnavailable;ticks+=1000;gui.update();assert(route->value=="Europe LAN · Unavailable");
 active_route.status=NodeRouteNone;ticks+=1000;gui.update();assert(route->value=="—");
 active_route.status=5;ticks+=1000;gui.update();assert(route->value=="Unavailable");active_route.status=NodeRouteNone;
 fail_command=NODE_GET_ACTIVE_ROUTE_IPC;ticks+=1000;gui.update();assert(route->value=="Unavailable");fail_command=0;
 std::strcpy(host_node.name, "Russia LAN");host_node.status=NodeHostUnavailable;ticks+=1000;gui.update();assert(node->value=="Russia LAN · Unavailable");
 host_node.status=NodeHostPending;ticks+=1000;gui.update();assert(node->value=="Russia LAN · Pending");
 host_node.status=NodeHostReady;ticks+=1000;gui.update();assert(node->value=="Russia LAN");
 host_node.status=NodeHostSelected;ticks+=1000;gui.update();assert(node->value=="Russia LAN");
 fail_command=NODE_GET_HOST_INFO_IPC;ticks+=1000;gui.update();assert(node->value=="Unavailable");fail_command=0;
 host_node.version=2;ticks+=1000;gui.update();assert(node->value=="Unavailable");host_node.version=1;
 std::memset(host_node.name,'x',sizeof(host_node.name));ticks+=1000;gui.update();assert(node->value=="Unavailable");std::strcpy(host_node.name,"Main server");
 auto requests=commands.size();gui.update();assert(commands.size()==requests);ticks+=1000;gui.update();assert(commands.size()>requests);
 Overlay overlay;before=sets;commands.clear();overlay.onHide();overlay.exitServices();assert(sets==before&&commands.empty());
 // Failed IPC cannot leak stale, partial or malformed replies into status controls.
 reset();Service s{1};LanywhereOverlayStatus out;std::memset(&out,0xaa,sizeof(out));fail_command=3;assert(R_FAILED(lanywhereOverlayGetStatus(&s,&out))&&out.state==0&&out.error[0]==0);
 fail_command=0;status.state=4;assert(R_FAILED(lanywhereOverlayGetStatus(&s,&out)));status={};status.state=2;status.member_count=9;assert(R_FAILED(lanywhereOverlayGetStatus(&s,&out)));
 LanywhereOverlayMatch mout;match.state=9;assert(R_FAILED(lanywhereOverlayGetMatch(&s,&mout)));
 u32 generation=99;for(u32 valid:{0u,1u,3u}){storage_selection=valid;assert(R_SUCCEEDED(lanywhereOverlayGetStorageSelection(&s,&generation))&&generation==valid);}
 assert(!std::strcmp(rc_storage_path_gen(0,"old","v1","new"),"old"));
 assert(!std::strcmp(rc_storage_path_gen(1,"old","v1","new"),"v1"));
 assert(!std::strcmp(rc_storage_path_gen(3,"old","v1","new"),"new"));
 storage_selection=2;assert(R_FAILED(lanywhereOverlayGetStorageSelection(&s,&generation))&&generation==2);
 storage_selection=4;assert(R_FAILED(lanywhereOverlayGetStorageSelection(&s,&generation))&&generation==2);
 NodeDynamicCatalog checked{};node_catalog.version=1;node_catalog.count=19;
 assert(R_FAILED(lanywhereOverlayGetNodeCatalog(&s,&checked))&&!checked.count);
 reset();connectServices();g_chinese=true;assert(std::strcmp(connectionLabel(),"未加入房间")==0);status.state=2;refresh();assert(std::strcmp(connectionLabel(),"已加入房间")==0);
 status.state=1;std::strcpy(status.error,"Reconnecting to the room.");refresh();assert(std::strcmp(connectionLabel(),"正在重连")==0);
 status.state=3;std::strcpy(status.error,"Room session expired. Rejoin from the game.");refresh();assert(std::strcmp(connectionLabel(),"会话已过期")==0);
 std::strcpy(status.error,"Room changed during recovery. Rejoin from the game.");refresh();assert(std::strcmp(connectionLabel(),"房间已变化")==0);
 std::strcpy(status.error,"Could not reconnect. Check Wi-Fi and rejoin.");refresh();assert(std::strcmp(connectionLabel(),"重连未成功")==0);
 testPasscodeGui();
 testNodeSelector();
 testNodeCatalogFailures();
 testFullNodeIdentity();
 testStorageHealth();
 testNodeTaskProtocol();
 testFrameworkLanguage();
 // Selection is only a route to the game's own room search.
 for(u32 c:commands)assert(c!=0&&c!=1&&c!=2&&c!=4&&c!=6&&c!=7&&c!=8&&c!=11&&c!=12);
 testRoomSelector();
 for(u32 c:commands)assert(c!=0&&c!=1&&c!=2&&c!=6&&c!=7&&c!=8&&c!=NODE_START_HOST_TASK_IPC);
 testRoomVersions();
 std::puts("Actual overlay IPC + UI: backend node tasks, worker-only room selection, stale/busy/locked/full states, hide, unload, locale, and exact routing passed");
}
