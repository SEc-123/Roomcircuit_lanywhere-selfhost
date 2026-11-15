/* Modified by LANywhere Self-host, 2026-11-07 to 2026-11-08. */
#include "lan_discovery.hpp"
#include "lanplay_direct.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>
#include "ipinfo.hpp"
#include "lanywhere_relay.hpp"
#include "public_scan_pages.hpp"
#include "game_identity_cache.hpp"
#include "lcid_policy.hpp"

namespace ams::mitm::ldn {

    static const int ModuleID = 0xFD;
    static const int LdnModuleId = 0xCB;

    static bool MatchesScanFilter(const ScanFilter &filter, const NetworkInfo &info) {
        return MatchesPublicFilter(filter, info, ScanFilterFlag_LocalCommunicationId, ScanFilterFlag_SessionId,
                                   ScanFilterFlag_NetworkType, ScanFilterFlag_Ssid, ScanFilterFlag_SceneId);
    }

    const char *LANDiscovery::FakeSsid = "12345678123456781234567812345678";
    const LANDiscovery::LanEventFunc LANDiscovery::EmptyFunc = [](){};


    int LanStation::onRead() {
        if (!this->socket) {
            LogFormat("Nullptr %d\n", this->nodeId);
            return -1;
        }
        return this->socket->recvPacket([&](LANPacketType type, const void *data, size_t size, ReplyFunc reply) -> int {
			AMS_UNUSED(reply);
            if (type == LANPacketType::Connect) {
                LogFormat("on connect");
                if (this->status != NodeStatus::Connected &&
                    this->discovery->networkInfo.ldn.stationAcceptPolicy == LdnAcceptPolicy_AlwaysReject) {
                    return -1;
                }
                NodeInfo *info = (decltype(info))data;
                if (size != sizeof(*info)) {
                    LogFormat("NodeInfo size is wrong");
                    return -1;
                }
                if (lanywhere::InRoom() || info->localCommunicationVersion !=
                        this->discovery->networkInfo.ldn.nodes[0].localCommunicationVersion) return -1;
                *this->nodeInfo = *info;
                this->status = NodeStatus::Connected;

                this->discovery->updateNodes();
            } else {
                LogFormat("unexpecting type %d", static_cast<int>(type));
            }
            return 0;
        });
    }

    void LanStation::onClose() {
        LogFormat("LanStation::onClose %d", this->nodeId);
        this->reset();
        this->discovery->updateNodes();
    }

    LDUdpSocket::LDUdpSocket(int fd, LANDiscovery *discovery)
        :   UdpLanSocketBase(fd, discovery->getListenPort()),
            discovery(discovery) {
        /* ... */
    }

    int LDUdpSocket::onRead() {
        LogFormat("LDUdpSocket::onRead");
        return this->recvPacket([&](LANPacketType type, const void *data, size_t size, ReplyFunc reply) -> int {
            switch (type) {
                case LANPacketType::Scan: {
                    if (!lanywhere::InRoom() && this->discovery->getState() == CommState::AccessPointCreated &&
                        this->discovery->networkInfo.ldn.stationAcceptPolicy != LdnAcceptPolicy_AlwaysReject) {
                        reply(LANPacketType::ScanResp, &this->discovery->networkInfo, sizeof(NetworkInfo));
                    }
                    break;
                }
                case LANPacketType::ScanResp: {
                    LogFormat("ScanResp");
                    NetworkInfo info;
                    if (size != sizeof(info)) {
                        break;
                    }
                    std::memcpy(&info, data, sizeof(info));
                    std::scoped_lock lock(this->discovery->scanResultsMutex);
                    if (!this->discovery->physicalScanActive || this->scanResults.size() >= LANDiscovery::MaxScanResults ||
                        !MatchesScanFilter(this->discovery->activeScanFilter, info)) break;
                    this->scanResults.try_emplace(info.common.bssid, info);
                    break;
                }
                default: {
                    LogFormat("LDUdpSocket::onRead unhandle type %d", static_cast<int>(type));
                    break;
                }
            }
            return 0;
        });
    }

    void LDUdpSocket::onClose() {
        this->close();
        discovery->disconnect_reason = DisconnectReason::SignalLost;
        discovery->setState(CommState::Error);
    };

    int LDTcpSocket::onRead() {
        LogFormat("LDTcpSocket::onRead");
        const auto state = this->discovery->getState();
        if (state == CommState::Station || state == CommState::StationConnected) {
            return this->recvPacket([&](LANPacketType type, const void *data, size_t size, ReplyFunc reply) -> int {
				AMS_UNUSED(reply);
                if (type == LANPacketType::SyncNetwork) {
                    LogFormat("SyncNetwork");
                    NetworkInfo *info = (decltype(info))data;
                    if (size != sizeof(*info)) {
                        return -1;
                    }

                    this->discovery->onSyncNetwork(info);
                } else {
                    LogFormat("LDTcpSocket::onRead unhandle type %d", static_cast<int>(type));
                    return -1;
                }

                return 0;
            });
        } else if (state == CommState::AccessPointCreated) {
            struct sockaddr_in addr{};
            socklen_t addrlen = sizeof(addr);
            int new_fd = accept(this->getFd(), (struct sockaddr *)&addr, &addrlen);
            if (new_fd < 0)
            {
                LogFormat("accept failed");
                return -1;
            }
            this->discovery->onConnect(new_fd);
            return 0;
        } else {
            LogFormat("LDTcpSocket::onRead wrong state %d", static_cast<int>(state));
            return -1;
        }
    }

    void LDTcpSocket::onClose() {
        this->close();
        LogFormat("LDTcpSocket::onClose");
        this->discovery->onDisconnectFromHost();
    }

    u32 LDUdpSocket::getBroadcast() {
        u32 address = 0, netmask = 0, gateway = 0, primary_dns = 0, secondary_dns = 0;
        Result rc = nifmGetCurrentIpConfigInfo(&address, &netmask, &gateway, &primary_dns, &secondary_dns);
        address = ntohl(address);
        netmask = ntohl(netmask);
        if (R_FAILED(rc)) {
            LogFormat("Broadcast failed to get ip");
            return 0xFFFFFFFF;
        }
        u32 ret = address | ~netmask;
        return ret;
    }

    void LANDiscovery::onSyncNetwork(NetworkInfo *info) {
        this->networkInfo = *info;
        if (this->state == CommState::Station) {
            this->setState(CommState::StationConnected);
        }
        this->onNetworkInfoChanged();
    }

    void LANDiscovery::onConnect(int new_fd) {
        LogFormat("Accepted %d", new_fd);
        if (lanywhere::InRoom() || this->networkInfo.ldn.nodeCountMax < 1 ||
            this->stationCount() >= this->networkInfo.ldn.nodeCountMax - 1) {
            LogFormat("Close new_fd. stations are full");
            close(new_fd);
            return;
        }

        bool found = false;
        for (auto &i : this->stations) {
            if (i.getStatus() == NodeStatus::Disconnected) {
                i.link(new_fd);
                found = true;
                break;
            }
        }

        if (!found) {
            LogFormat("Close new_fd. no free station found");
            close(new_fd);
        }
    }

    void LANDiscovery::onDisconnectFromHost() {
        LogFormat("onDisconnectFromHost state: %d", static_cast<int>(this->state));
        if (this->state == CommState::StationConnected) {
            this->setState(CommState::Station);
        }
    }

    void LANDiscovery::onNetworkInfoChanged() {
        this->refreshTcpBinding();
        if (this->isNodeStateChanged()) {
            if (this->lanEvent) this->lanEvent();
        }
        return;
    }

    Result LANDiscovery::setAdvertiseData(const u8 *data, uint16_t size) {
        std::scoped_lock lifecycle(this->lifecycleMutex);
        std::scoped_lock lock(this->pollMutex);
        if (!this->initialized || this->state != CommState::AccessPointCreated || (size && !data)) return MAKERESULT(LdnModuleId, 32);
        if (size > AdvertiseDataSizeMax) {
            return MAKERESULT(ModuleID, 10);
        }

        if (size > 0 && data != nullptr) {
            std::memcpy(this->networkInfo.ldn.advertiseData, data, size);
        } else {
            LogFormat("LANDiscovery::setAdvertiseData data %p size %lu", data, size);
        }
        this->networkInfo.ldn.advertiseDataSize = size;

        this->updateNodes();

        return 0;
    }

    Result LANDiscovery::setStationAcceptPolicy(u8 policy) {
        std::scoped_lock lifecycle(this->lifecycleMutex);
        std::scoped_lock lock(this->pollMutex);
        if (this->state != CommState::AccessPoint && this->state != CommState::AccessPointCreated) {
            return MAKERESULT(LdnModuleId, 32);
        }
        if (policy != LdnAcceptPolicy_AlwaysAccept && policy != LdnAcceptPolicy_AlwaysReject) {
            return sf::ResultNotSupported();
        }
        if (this->networkInfo.ldn.stationAcceptPolicy != policy) {
            this->networkInfo.ldn.stationAcceptPolicy = policy;
            if (this->state == CommState::AccessPointCreated) {
                this->updateNodes();
            }
        }
        return ResultSuccess();
    }

    Result LANDiscovery::initNetworkInfo() {
        Result rc = getFakeMac(&this->networkInfo.common.bssid);
        if (R_FAILED(rc)) {
            return rc;
        }
        this->networkInfo.common.channel = 6;
        this->networkInfo.common.linkLevel = 3;
        this->networkInfo.common.networkType = 2;
        this->networkInfo.common.ssid = FakeSsid;

        auto nodes = this->networkInfo.ldn.nodes;
        for (int i = 0; i < NodeCountMax; i++) {
            nodes[i].nodeId = i;
            nodes[i].isConnected = 0;
        }

        return 0;
    }

    Result LANDiscovery::getFakeMac(MacAddress *mac) {
        mac->raw[0] = 0x02;
        mac->raw[1] = 0x00;

        u32 ip;
        Result rc = 0;
        if (lanywhere::InRoom()) {
            // 02:xx:xx:xx:ip:ip, con xx de la sala: cada grupo abierto tiene un BSSID distinto.
            const u32 tag = lanywhere::RoomTag();
            memcpy(mac->raw + 1, &tag, sizeof(tag));
            const u32 my_ip = lanywhere::MyIp();
            mac->raw[4] = (my_ip >> 8) & 0xFF;
            mac->raw[5] = my_ip & 0xFF;
            return 0;
        } else if (R_SUCCEEDED(rc = nifmGetCurrentIpAddress(&ip))) {
            ip = ntohl(ip);
        }
        if (R_SUCCEEDED(rc)) {
            memcpy(mac->raw + 2, &ip, sizeof(ip));
        }

        return rc;
    }

    Result LANDiscovery::setSocketOpts(int fd) {
        int rc;

        {
            int b = 1;
            rc = setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &b, sizeof(b));
            if (rc != 0) {
                return MAKERESULT(ModuleID, 4);
            }
        }
        {
            int yes = 1;
            rc = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
            if (rc != 0) {
                // return MAKERESULT(ModuleID, 5);
                LogFormat("SO_REUSEADDR failed");
            }
        }

        return 0;
    }

    Result LANDiscovery::initTcp(bool listening) {
        int fd;
        Result rc;
        struct sockaddr_in addr{};
        std::scoped_lock lock(this->pollMutex);

        if (this->tcp) {
            this->tcp->close();
        }
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            return MAKERESULT(ModuleID, 6);
        }
        auto tcpSocket = std::make_unique<LDTcpSocket>(fd, this);

        if (listening) {
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(lanywhere::InRoom() ? INADDR_LOOPBACK : INADDR_ANY);
            addr.sin_port = htons(listenPort);
            if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
                return MAKERESULT(ModuleID, 7);
            }
            if (listen(fd, 10) != 0) {
                return MAKERESULT(ModuleID, 8);
            }
        }
        rc = setSocketOpts(fd);
        if (R_FAILED(rc)) {
            return rc;
        }

        this->tcp = std::move(tcpSocket);

        return 0;
    }

    Result LANDiscovery::initUdp(bool listening) {
        int fd;
        Result rc;
        struct sockaddr_in addr{};
        std::scoped_lock lock(this->pollMutex);

        if (this->udp) {
            this->udp->close();
        }
        fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) {
            return MAKERESULT(ModuleID, 1);
        }
        auto udpSocket = std::make_unique<LDUdpSocket>(fd, this);

        if (listening) {
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
            addr.sin_port = htons(listenPort);
            if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
                return MAKERESULT(ModuleID, 2);
            }
        }
        rc = setSocketOpts(fd);
        if (R_FAILED(rc)) {
            return rc;
        }

        this->udp = std::move(udpSocket);

        return 0;
    }

    void LANDiscovery::initNodeStateChange() {
        for (auto &i : this->nodeChanges) {
            i.stateChange = NodeStateChange_None;
        }
        for (auto &i : this->nodeLastStates) {
            i = 0;
        }
    }

    bool LANDiscovery::isNodeStateChanged() {
        bool changed = false;
        const auto &nodes = this->networkInfo.ldn.nodes;
        for (int i = 0; i < NodeCountMax; i++) {
            if (nodes[i].isConnected != this->nodeLastStates[i]) {
                if (nodes[i].isConnected) {
                    this->nodeChanges[i].stateChange |= NodeStateChange_Connect;
                } else {
                    this->nodeChanges[i].stateChange |= NodeStateChange_Disconnect;
                }
                this->nodeLastStates[i] = nodes[i].isConnected;
                changed = true;
            }
        }
        return changed;
    }

    void LANDiscovery::Worker(void* args) {
        LANDiscovery* self = (LANDiscovery*)args;

        self->worker();
    }

    Result LANDiscovery::resolveGameLcid(u64 *id, const lanywhere::GameRoute &route) {
        if (!lanywhere::GameRouteStillCurrent(route)) {
            lanywhere::ReportGameRouteChanged();
            return lanywhere::ResultRelayFailed();
        }
        // Pure physical LAN remains the original ldn_mitm wire protocol.
        // Only an online operation interprets IDs as relay game identity.
        if (!route.room_epoch && !route.discovery.public_enabled) return 0;
        GameCacheRecord record{};
        const auto status = lanywhere::GameIdentityCacheExactRecord(this->hostProgramId, &record);
        const auto resolved = ResolveGameLcid(*id, !std::strcmp(route.discovery.provider, "self"), status, record);
        if (resolved.error != LcidPolicyError::None) {
            lanywhere::ReportGameNetworkError(resolved.error == LcidPolicyError::Unavailable);
            return lanywhere::ResultRelayFailed();
        }
        if (!lanywhere::GameRouteStillCurrent(route)) {
            lanywhere::ReportGameRouteChanged();
            return lanywhere::ResultRelayFailed();
        }
        if (!this->loggedLcid || this->loggedWireLcid != resolved.wire || this->loggedCanonicalLcid != resolved.canonical) {
            lanywhere::Log("ldn identity: program=%016llx provider=%s wire=%016llx canonical=%016llx",
                static_cast<unsigned long long>(this->hostProgramId), route.discovery.provider,
                static_cast<unsigned long long>(resolved.wire), static_cast<unsigned long long>(resolved.canonical));
            this->loggedLcid = true;
            this->loggedWireLcid = resolved.wire;
            this->loggedCanonicalLcid = resolved.canonical;
        }
        *id = resolved.wire;
        return 0;
    }

    Result LANDiscovery::scan(NetworkInfo *pOutNetwork, u16 *count, ScanFilter filter) {
        std::scoped_lock lifecycle(this->lifecycleMutex);
        if (!count || (*count && !pOutNetwork)) return MAKERESULT(LdnModuleId, 32);
        {
            std::scoped_lock lock(this->pollMutex);
            if (!this->initialized || !this->udp || filter.ssid.length > SsidLengthMax) {
                *count = 0;
                return MAKERESULT(LdnModuleId, 32);
            }
        }
        lanywhere::GameRoute gameRoute{};
        if (!lanywhere::CaptureGameRoute(&gameRoute)) {
            *count = 0;
            lanywhere::ReportGameRouteChanged();
            return lanywhere::ResultRelayFailed();
        }
        if (filter.flag & ScanFilterFlag_LocalCommunicationId) {
            const Result rc = this->resolveGameLcid(&filter.networkId.intentId.localCommunicationId, gameRoute);
            if (R_FAILED(rc)) { *count = 0; return rc; }
        }
        const bool wrong_selection = gameRoute.discovery.selected_game_id &&
            (!(filter.flag & ScanFilterFlag_LocalCommunicationId) ||
             filter.networkId.intentId.localCommunicationId != gameRoute.discovery.selected_game_id);
        NetworkInfo selected;
        // Legacy external communities use an unresolved wire sentinel; it is
        // never an identity on which to enable our reserved auto-matching.
        const auto match = wrong_selection || gameRoute.room_epoch || !gameRoute.discovery.public_enabled || std::strcmp(gameRoute.discovery.provider, "self") ?
            lanywhere::MatchScanResult::Manual : lanywhere::ScanMatch(filter, &selected, &gameRoute.discovery);
        {
            std::scoped_lock lock(this->scanResultsMutex);
            this->udp->scanResults.clear();
            this->publicGroupCount = 0;
            this->lastScanWasMatch = match != lanywhere::MatchScanResult::Manual;
            this->lastScanRoute = gameRoute;
            this->haveScanRoute = true;
            this->physicalScanActive = !gameRoute.room_epoch && !gameRoute.discovery.public_enabled;
            this->roomScanActive = gameRoute.room_epoch != 0;
            this->activeScanFilter = filter;
        }
        if (wrong_selection) {
            *count = 0;
            return 0;
        }
        if (match != lanywhere::MatchScanResult::Manual) {
            if (*count && match == lanywhere::MatchScanResult::Assigned) {
                pOutNetwork[0] = selected;
                *count = 1;
            } else {
                *count = 0;
            }
            return 0;
        }

        bool waitForReplies = true;
        if (!lanywhere::GameRouteStillCurrent(gameRoute)) {
            std::scoped_lock lock(this->scanResultsMutex);
            this->physicalScanActive = this->roomScanActive = false;
            *count = 0;
            lanywhere::ReportGameRouteChanged();
            return lanywhere::ResultRelayFailed();
        }
        if (gameRoute.room_epoch) {
            if (!lanywhere::SendControl(lanywhere::AllMembers, lanywhere::LdnMsg::Scan, nullptr, 0)) {
                *count = 0;
                lanywhere::ReportGameRouteChanged();
                return lanywhere::ResultRelayFailed();
            }
        } else if (gameRoute.discovery.public_enabled) {
            // Public discovery is relay-only. Mixing a canonical self filter
            // with raw physical LAN replies would silently lose or misroute rooms.
            waitForReplies = false;
            int publicGroups = -1;
            const auto &route = gameRoute.discovery;
            if (route.public_enabled) {
                PublicScanContext context{this, &filter, route};
                // A bounded scan protects the game's IPC call and relay rate limits.
                // Filter each page before consuming the eight stored result slots.
                publicGroups = CollectPublicPages(
                    [&](u32 offset, u32 *next) {
                        if (!lanywhere::GameRouteStillCurrent(gameRoute)) return -1;
                        return lanywhere::FindGroupsFilteredPage(filter, offset, next, &LANDiscovery::onPublicGroup, &context, &route);
                    },
                    [&]() { return route.selected_game_id ? this->publicGroupCount > 0 :
                        this->publicGroupCount >= this->publicGroups.size(); },
                    lanywhere::GroupPageEnd, 16);
            }
            if (!waitForReplies && publicGroups < 0) {
                *count = 0;
                return MAKERESULT(ModuleID, 20);
            }
        } else {
            waitForReplies = this->udp->sendBroadcast(LANPacketType::Scan) >= 0;
            if (!waitForReplies) {
                std::scoped_lock lock(this->scanResultsMutex);
                this->physicalScanActive = this->roomScanActive = false;
                *count = 0;
                return MAKERESULT(ModuleID, 20);
            }
        }

        if (waitForReplies) {
            svcSleepThread(1000000000L); // 1sec
        }

        if (!lanywhere::GameRouteStillCurrent(gameRoute)) {
            std::scoped_lock lock(this->scanResultsMutex);
            this->physicalScanActive = this->roomScanActive = false;
            *count = 0;
            lanywhere::ReportGameRouteChanged();
            return lanywhere::ResultRelayFailed();
        }

        std::scoped_lock lock(this->scanResultsMutex);
        this->physicalScanActive = this->roomScanActive = false;
        int i = 0;
        for (auto& item : this->udp->scanResults) {
            if (i >= *count) {
                break;
            }
            auto &info = item.second;

            if (MatchesScanFilter(filter, info)) {
                pOutNetwork[i++] = info;
            }
        }
        *count = i;

        return 0;
    }

    void LANDiscovery::resetStations() {
        for (auto &i : this->stations) {
            i.reset();
        }
    }

    int LANDiscovery::stationCount() {
        int count = 0;

        for (auto const &i : this->stations) {
            if (i.getStatus() != NodeStatus::Disconnected) {
                count++;
            }
        }

        return count;
    }

    void LANDiscovery::updateNodes() {
        int count = 0;
        for (auto &i : this->stations) {
            bool connected = i.getStatus() == NodeStatus::Connected;
            if (connected) {
                count++;
            }
            i.overrideInfo();
        }
        this->networkInfo.ldn.nodeCount = count + 1;

        if (this->autoRoom && this->state == CommState::AccessPointCreated) {
            lanywhere::Publish(&this->networkInfo, sizeof(this->networkInfo), this->hostProgramId);
        }
        for (auto &i : stations) {
            if (i.getStatus() == NodeStatus::Connected && i.member >= 0) {
                lanywhere::SendControl(i.member, lanywhere::LdnMsg::Sync, &this->networkInfo, sizeof(this->networkInfo));
            } else if (i.getStatus() == NodeStatus::Connected) {
                int ret = i.sendPacket(LANPacketType::SyncNetwork, &this->networkInfo, sizeof(this->networkInfo));
                if (ret < 0) {
                    LogFormat("Failed to sendTcp");
                }
            }
        }

        this->onNetworkInfoChanged();
    }

    int LANDiscovery::loopPoll() {
        int rc;
        std::scoped_lock lock(this->pollMutex);
        if (!initialized) {
            return 0;
        }
        constexpr size_t nfds = 2 + StationCountMax;
        Pollable *fds[nfds];
        fds[0] = this->udp.get();
        fds[1] = this->tcp.get();
        for (int i = 0; i < StationCountMax; i++) {
            fds[2 + i] = this->stations.data() + i;
        }
        rc = Pollable::Poll(fds, nfds);
        this->pollRoom();

        return rc;
    }

    // LANywhere: con una sala activa, la búsqueda, la unión y la sincronización del grupo van entre miembros
    // de la sala por el servidor en lugar de por broadcast UDP y TCP en la red local.
    void LANDiscovery::pollRoom() {
        u8 *buffer = this->controlBuffer;
        if (this->onlineGame && !lanywhere::InRoom()) {
            lanywhere::Status status{};
            lanywhere::GetStatus(&status);
            // Retain LDN state while the transport is recovering. A final
            // error or explicit leave must also disconnect the game's view.
            if (status.state != lanywhere::RelayState::Connecting) {
                this->disconnect_reason = DisconnectReason::SignalLost;
                this->hostMember = -1;
                this->resetStations();
                this->leaveAutoRoom();
                for (auto &node : this->networkInfo.ldn.nodes) node.isConnected = 0;
                this->networkInfo.ldn.nodeCount = 0;
                this->onNetworkInfoChanged();
                this->setState(CommState::Error);
                return;
            }
        }
        u8 from;
        lanywhere::LdnMsg msg;
        ssize_t size;
        while ((size = lanywhere::TakeControl(&from, &msg, buffer, sizeof(this->controlBuffer))) >= 0) {
            if (static_cast<size_t>(size) <= sizeof(this->controlBuffer)) {
                this->onRoomMessage(from, static_cast<u8>(msg), buffer, size);
            }
        }
        this->refreshTcpBinding();
    }

    void LANDiscovery::refreshTcpBinding() {
        std::scoped_lock lock(this->tcpBindingLock);
        tcpstream::GameSession session{};
        session.owner={this->callerProcessId,this->hostProgramId};session.binding_id=this->tcpBindingId;
        session.transport_epoch=this->tcpTransportEpoch;session.session_nonce=this->tcpSessionNonce;session.self_ip=this->tcpSelfIp;
        std::memcpy(session.network_id,&this->networkInfo.networkId,sizeof(session.network_id));
        const bool connected=this->state==CommState::AccessPointCreated||this->state==CommState::StationConnected;
        u64 epoch=0;u32 capabilities=0,self_ip=0;
        if(connected&&session.owner.process&&session.owner.program&&lanywhere::CaptureTcpTransport(&epoch,&capabilities,&self_ip)) {
            if(epoch!=this->tcpTransportEpoch||std::memcmp(this->tcpNetworkId,session.network_id,32)||!this->tcpSessionNonce) {
                do {os::GenerateRandomBytes(&this->tcpSessionNonce,sizeof(this->tcpSessionNonce));}while(!this->tcpSessionNonce);
                this->tcpTransportEpoch=epoch;std::memcpy(this->tcpNetworkId,session.network_id,32);
            }
            this->tcpSelfIp=self_ip;session.transport_epoch=epoch;session.session_nonce=this->tcpSessionNonce;
            session.capabilities=capabilities;session.self_ip=self_ip;session.active=true;
            for(size_t i=0;i<8;i++)if(this->networkInfo.ldn.nodes[i].isConnected)
                session.peers[i]=this->networkInfo.ldn.nodes[i].ipv4Address;
        }
        tcpstream::UpdateGameSession(session);
    }

    void LANDiscovery::onRoomMessage(u8 from, u8 msg, const void *data, size_t size) {
        lanywhere::Log("ldn: mensaje %u de %u (%zu bytes), estado %d", msg, from, size, static_cast<int>(this->state));
        switch (static_cast<lanywhere::LdnMsg>(msg)) {
            case lanywhere::LdnMsg::Scan: {
                if (this->state == CommState::AccessPointCreated &&
                    this->networkInfo.ldn.stationAcceptPolicy != LdnAcceptPolicy_AlwaysReject) {
                    lanywhere::SendControl(from, lanywhere::LdnMsg::ScanResp, &this->networkInfo, sizeof(NetworkInfo));
                }
                break;
            }
            case lanywhere::LdnMsg::ScanResp: {
                if (size == sizeof(NetworkInfo) && this->udp) {
                    NetworkInfo info;
                    std::memcpy(&info, data, sizeof(info));
                    std::scoped_lock lock(this->scanResultsMutex);
                    if (this->roomScanActive && this->udp->scanResults.size() < MaxScanResults &&
                        MatchesScanFilter(this->activeScanFilter, info))
                        this->udp->scanResults.try_emplace(info.common.bssid, info);
                }
                break;
            }
            case lanywhere::LdnMsg::Connect: {
                if (this->state != CommState::AccessPointCreated || size != sizeof(NodeInfo)) {
                    break;
                }
                NodeInfo node;
                std::memcpy(&node, data, sizeof(node));
                if (lanywhere::MemberOfIp(node.ipv4Address) != from ||
                    node.localCommunicationVersion != this->networkInfo.ldn.nodes[0].localCommunicationVersion) {
                    lanywhere::SendControl(from, lanywhere::LdnMsg::Disconnect, nullptr, 0);
                    break;
                }
                LanStation *slot = nullptr;
                for (auto &i : this->stations) {
                    if (i.member == from) { slot = &i; break; }
                }
                const bool rejoin = slot != nullptr;
                if (!rejoin && this->networkInfo.ldn.stationAcceptPolicy == LdnAcceptPolicy_AlwaysReject) {
                    lanywhere::SendControl(from, lanywhere::LdnMsg::Disconnect, nullptr, 0);
                    break;
                }
                for (auto &i : this->stations) {
                    if (slot == nullptr && i.getStatus() == NodeStatus::Disconnected) { slot = &i; }
                }
                if (slot == nullptr || (!rejoin && this->stationCount() >= this->networkInfo.ldn.nodeCountMax - 1)) {
                    lanywhere::SendControl(from, lanywhere::LdnMsg::Disconnect, nullptr, 0);
                    break;
                }
                slot->member = from;
                *slot->nodeInfo = node;
                slot->status = NodeStatus::Connected;
                this->updateNodes();
                break;
            }
            case lanywhere::LdnMsg::Sync: {
                if ((this->state == CommState::Station || this->state == CommState::StationConnected) &&
                    from == this->hostMember && size == sizeof(NetworkInfo)) {
                    NetworkInfo info = *static_cast<const NetworkInfo *>(data);
                    this->onSyncNetwork(&info);
                }
                break;
            }
            case lanywhere::LdnMsg::Disconnect:
            case lanywhere::LdnMsg::MemberLeft: {
                if (this->state == CommState::AccessPointCreated) {
                    for (auto &i : this->stations) {
                        if (i.member == from) {
                            i.reset();
                            this->updateNodes();
                        }
                    }
                } else if (from == this->hostMember) {
                    this->hostMember = -1;
                    this->onDisconnectFromHost();
                }
                break;
            }
        }
    }

    void LANDiscovery::onPublicGroup(const char *code, const void *data, void *user) {
        const auto &context = *static_cast<PublicScanContext *>(user);
        LANDiscovery *self = context.self;
        NetworkInfo info;
        std::memcpy(&info, data, sizeof(info));
        if (!MatchesScanFilter(*context.filter, info) ||
            (context.route.selected_game_id && (info.networkId.intentId.localCommunicationId != context.route.selected_game_id ||
             std::strcmp(code, context.route.selected_code) != 0))) return;
        std::scoped_lock lock(self->scanResultsMutex);
        if (self->publicGroupCount >= self->publicGroups.size() || !self->udp) {
            return;
        }
        if (!self->udp->scanResults.try_emplace(info.common.bssid, info).second) {
            return;
        }
        auto &g = self->publicGroups[self->publicGroupCount++];
        g.bssid = info.common.bssid;
        util::Strlcpy(g.code, code, sizeof(g.code));
        g.route = context.route;
    }

    void LANDiscovery::leaveAutoRoom() {
        this->onlineGame = false;
        if (this->autoRoom) {
            this->autoRoom = false;
            this->hostMember = -1;
            if (this->ownsMatchRoom) {
                this->ownsMatchRoom = false;
                lanywhere::LeaveMatchRoom(this->matchRoom);
            } else {
                if (this->ownedGameRoute.room_token) lanywhere::LeaveGameRoom(this->ownedGameRoute);
                else lanywhere::LeaveRoom();
            }
            this->ownedGameRoute = {};
            lanywhere::Log("ldn: fuera del grupo abierto");
        }
    }

    // El anfitrión cierra el grupo: las estaciones de la sala tienen que enterarse (en LAN lo hace el cierre del TCP).
    void LANDiscovery::roomCloseStations() {
        if (this->state != CommState::AccessPointCreated) {
            return;
        }
        for (auto &i : this->stations) {
            if (i.member >= 0) {
                lanywhere::SendControl(i.member, lanywhere::LdnMsg::Disconnect, nullptr, 0);
            }
        }
    }

    LANDiscovery::~LANDiscovery() {
        LogFormat("~LANDiscovery");
        static_cast<void>(this->finalize());
    }

    void LANDiscovery::worker() {
        while (!this->stop) {

            int rc = loopPoll();
            if (rc < 0) {
                break;
            }
            lanywhere::PollMatch();
            svcSleepThread(10000000L); // 10ms
        }
        LogFormat("Worker exit");
    }

    Result LANDiscovery::getNetworkInfo(NetworkInfo *pOutNetwork) {
        std::scoped_lock lock(this->pollMutex);
        Result rc = 0;

        if (this->state == CommState::AccessPointCreated || this->state == CommState::StationConnected) {
            std::memcpy(pOutNetwork, &networkInfo, sizeof(networkInfo));
        } else {
            rc = MAKERESULT(LdnModuleId, 32);
        }

        return rc;
    }

    Result LANDiscovery::getNetworkInfo(NetworkInfo *pOutNetwork, NodeLatestUpdate *pOutUpdates, int bufferCount) {
        std::scoped_lock lock(this->pollMutex);
        Result rc = 0;

        if (bufferCount < 0 || bufferCount > NodeCountMax) {
            return MAKERESULT(ModuleID, 50);
        }

        if (this->state == CommState::AccessPointCreated || this->state == CommState::StationConnected) {
            std::memcpy(pOutNetwork, &networkInfo, sizeof(networkInfo));

            char str[10] = {0};
            for (int i = 0; i < bufferCount; i++) {
                pOutUpdates[i] = {};
                pOutUpdates[i].stateChange = nodeChanges[i].stateChange;
                nodeChanges[i].stateChange = NodeStateChange_None;
                str[i] = '0' + pOutUpdates[i].stateChange;
            }
            LogFormat("getNetworkInfo updates %s", str);
        } else {
            rc = MAKERESULT(LdnModuleId, 32);
        }

        return rc;
    }

    Result LANDiscovery::getNodeInfo(NodeInfo *node, const UserConfig *userConfig, u16 localCommunicationVersion) {
        u32 ipAddress;
        Result rc = 0;
        if (lanywhere::InRoom()) {
            ipAddress = lanywhere::MyIp();
        } else {
            rc = nifmGetCurrentIpAddress(&ipAddress);
            if (R_FAILED(rc))
            {
                return rc;
            }
            ipAddress = ntohl(ipAddress);
        }
        rc = getFakeMac(&node->macAddress);
        if (R_FAILED(rc)) {
            return rc;
        }

        node->isConnected = 1;
        // LANywhere uses the Switch name for the game-facing field.
        if (lanywhere::InRoom()) {
            memset(node->userName, 0, sizeof(node->userName));
            lanywhere::Nickname(node->userName, sizeof(node->userName));
        } else {
            const char *end = static_cast<const char *>(std::memchr(userConfig->userName, 0, sizeof(userConfig->userName)));
            if (!end) return MAKERESULT(LdnModuleId, 32);
            std::memcpy(node->userName, userConfig->userName, end - userConfig->userName + 1);
        }
        node->localCommunicationVersion = localCommunicationVersion;
        node->ipv4Address = ipAddress;

        return 0;
    }

    Result LANDiscovery::createNetwork(const SecurityConfig *securityConfig, const UserConfig *userConfig, const NetworkConfig *networkConfig) {
        std::scoped_lock lifecycle(this->lifecycleMutex);
        std::scoped_lock state_lock(this->pollMutex);
        if (!this->initialized) return MAKERESULT(LdnModuleId, 32);
        Result rc = 0;

        if (!securityConfig || !userConfig || !networkConfig || networkConfig->nodeCountMax < 1 ||
            networkConfig->nodeCountMax > NodeCountMax || this->state != CommState::AccessPoint) {
            return MAKERESULT(LdnModuleId, 32);
        }
        lanywhere::GameRoute gameRoute{};
        if (!lanywhere::CaptureHostGameRoute(&gameRoute)) {
            lanywhere::ReportGameRouteChanged();
            return lanywhere::ResultRelayFailed();
        }
        NetworkConfig normalized = *networkConfig;
        R_TRY(this->resolveGameLcid(&normalized.intentId.localCommunicationId, gameRoute));
        networkConfig = &normalized;
        if (lanywhere::MatchArmed()) lanywhere::CancelMatch();

        // When public mode is enabled, never report a local-only group as an online room.
        // Players can explicitly disable public mode to create a local-only group.
        if (!gameRoute.room_epoch && gameRoute.discovery.public_enabled) {
            rc = lanywhere::CreateRoom(networkConfig->intentId.localCommunicationId, &gameRoute.discovery, &this->ownedGameRoute);
            if (R_FAILED(rc)) {
                this->autoRoom = false;
                lanywhere::Log("ldn: no se pudo crear el grupo publico; no se creo un grupo local");
                return rc;
            }
            this->autoRoom = true;
            lanywhere::Log("ldn: sala publica creada; anuncio pendiente");
            // The successful enter installs this exact captured provider. Pin
            // its active epoch for the remaining network setup.
            gameRoute = this->ownedGameRoute;
        }

        if (!lanywhere::GameRouteStillCurrent(gameRoute)) {
            if (this->autoRoom) this->leaveAutoRoom();
            lanywhere::ReportGameRouteChanged();
            return lanywhere::ResultRelayFailed();
        }

        rc = this->initTcp(true);
        if (R_FAILED(rc)) {
            this->leaveAutoRoom();
            return rc;
        }
        rc = this->initNetworkInfo();
        if (R_FAILED(rc)) {
            this->leaveAutoRoom();
            return rc;
        }
        this->networkInfo.ldn.nodeCountMax = networkConfig->nodeCountMax;
        this->networkInfo.ldn.securityMode = securityConfig->securityMode;

        if (networkConfig->channel == 0) {
            this->networkInfo.common.channel = 6;
        } else {
            this->networkInfo.common.channel = networkConfig->channel;
        }

        ams::os::GenerateRandomBytes(&this->networkInfo.networkId.sessionId, sizeof(SessionId));
        this->networkInfo.networkId.intentId = networkConfig->intentId;

        NodeInfo *node0 = &this->networkInfo.ldn.nodes[0];
        rc = this->getNodeInfo(node0, userConfig, networkConfig->localCommunicationVersion);
        if (R_FAILED(rc)) {
            this->leaveAutoRoom();
            return rc;
        }

        if (!lanywhere::GameRouteStillCurrent(gameRoute)) {
            if (this->autoRoom) this->leaveAutoRoom();
            lanywhere::ReportGameRouteChanged();
            return lanywhere::ResultRelayFailed();
        }

        this->onlineGame = gameRoute.room_epoch != 0;
        this->setState(CommState::AccessPointCreated);

        this->initNodeStateChange();
        node0->isConnected = 1;
        this->updateNodes();

        return rc;
    }

    Result LANDiscovery::destroyNetwork() {
        std::scoped_lock lifecycle(this->lifecycleMutex);
        std::scoped_lock state_lock(this->pollMutex);
        if (!this->initialized) return MAKERESULT(LdnModuleId, 32);
        this->roomCloseStations();
        this->leaveAutoRoom();
        if (this->tcp) {
            this->tcp->close();
        }
        this->resetStations();

        {
            std::scoped_lock lock(this->pollMutex);
            this->networkInfo.ldn.stationAcceptPolicy = LdnAcceptPolicy_AlwaysAccept;
        }
        this->setState(CommState::AccessPoint);

        return 0;
    }

    Result LANDiscovery::disconnect() {
        std::scoped_lock lifecycle(this->lifecycleMutex);
        std::scoped_lock state_lock(this->pollMutex);
        if (!this->initialized) return MAKERESULT(LdnModuleId, 32);
        if (this->hostMember >= 0) {
            lanywhere::SendControl(this->hostMember, lanywhere::LdnMsg::Disconnect, nullptr, 0);
            this->hostMember = -1;
        }
        this->leaveAutoRoom();
        if (this->tcp) {
            this->tcp->close();
        }
        this->setState(CommState::Station);

        return 0;
    }

    Result LANDiscovery::openAccessPoint() {
        std::scoped_lock lifecycle(this->lifecycleMutex);
        std::scoped_lock state_lock(this->pollMutex);
        if (!this->initialized) return MAKERESULT(LdnModuleId, 32);
        this->disconnect_reason = DisconnectReason::None;
        if (this->state == CommState::None) {
            return MAKERESULT(LdnModuleId, 32);
        }
        this->roomCloseStations();
        this->leaveAutoRoom();

        if (this->tcp) {
            this->tcp->close();
        }
        this->resetStations();

        {
            std::scoped_lock lock(this->pollMutex);
            this->networkInfo.ldn.stationAcceptPolicy = LdnAcceptPolicy_AlwaysAccept;
        }
        this->setState(CommState::AccessPoint);

        return 0;
    }

    Result LANDiscovery::closeAccessPoint() {
        std::scoped_lock lifecycle(this->lifecycleMutex);
        std::scoped_lock state_lock(this->pollMutex);
        if (!this->initialized) return MAKERESULT(LdnModuleId, 32);
        if (this->state == CommState::None) {
            return MAKERESULT(LdnModuleId, 32);
        }
        this->roomCloseStations();
        this->leaveAutoRoom();

        if (this->tcp) {
            this->tcp->close();
        }
        this->resetStations();

        this->setState(CommState::Initialized);

        return 0;
    }

    Result LANDiscovery::openStation() {
        std::scoped_lock lifecycle(this->lifecycleMutex);
        std::scoped_lock state_lock(this->pollMutex);
        if (!this->initialized) return MAKERESULT(LdnModuleId, 32);
        this->disconnect_reason = DisconnectReason::None;
        if (this->state == CommState::None) {
            return MAKERESULT(LdnModuleId, 32);
        }
        this->leaveAutoRoom();

        if (this->tcp) {
            this->tcp->close();
        }
        this->resetStations();

        this->setState(CommState::Station);

        return 0;
    }

    Result LANDiscovery::closeStation() {
        std::scoped_lock lifecycle(this->lifecycleMutex);
        std::scoped_lock state_lock(this->pollMutex);
        if (!this->initialized) return MAKERESULT(LdnModuleId, 32);
        if (this->state == CommState::None) {
            return MAKERESULT(LdnModuleId, 32);
        }
        this->leaveAutoRoom();

        if (this->tcp) {
            this->tcp->close();
        }
        this->resetStations();

        this->setState(CommState::Initialized);

        return 0;
    }

    Result LANDiscovery::connect(const NetworkInfo *networkInfo, UserConfig *userConfig, u16 localCommunicationVersion) {
        std::scoped_lock lifecycle(this->lifecycleMutex);
        std::unique_lock state_lock(this->pollMutex);
        if (!this->initialized || this->state != CommState::Station || !networkInfo || !userConfig ||
            networkInfo->ldn.nodeCountMax < 1 || networkInfo->ldn.nodeCountMax > NodeCountMax ||
            networkInfo->ldn.nodeCount > networkInfo->ldn.nodeCountMax) return MAKERESULT(LdnModuleId, 32);
        if (networkInfo->ldn.nodeCount == 0) {
            return MAKERESULT(ModuleID, 30);
        }

        lanywhere::GameRoute gameRoute{};
        if (!lanywhere::CaptureGameRoute(&gameRoute)) {
            lanywhere::ReportGameRouteChanged();
            return lanywhere::ResultRelayFailed();
        }
        char code[lanywhere::CodeLength + 1] = {};
        bool lastMatch = false;
        {
            std::scoped_lock lock(this->scanResultsMutex);
            lastMatch = this->lastScanWasMatch;
            if (!gameRoute.room_epoch) {
                if (this->haveScanRoute) gameRoute = this->lastScanRoute;
                for (size_t i = 0; i < this->publicGroupCount; i++) {
                    if (this->publicGroups[i].bssid == networkInfo->common.bssid) {
                        util::Strlcpy(code, this->publicGroups[i].code, sizeof(code));
                        gameRoute.discovery = this->publicGroups[i].route;
                        break;
                    }
                }
            }
        }
        NetworkInfo normalized = *networkInfo;
        R_TRY(this->resolveGameLcid(&normalized.networkId.intentId.localCommunicationId, gameRoute));
        networkInfo = &normalized;
        if (!gameRoute.room_epoch) {
            if (gameRoute.discovery.public_enabled && !std::strcmp(gameRoute.discovery.provider, "self") &&
                (lastMatch || lanywhere::MatchArmed())) {
                lanywhere::MatchJoinAttempt attempt;
                if (R_FAILED(lanywhere::JoinMatch(*networkInfo, localCommunicationVersion, &attempt))) {
                    return MAKERESULT(ModuleID, 31);
                }
                this->autoRoom = true;
                this->ownsMatchRoom = true;
                this->matchRoom = attempt;
                state_lock.unlock();
                const Result rc = this->connectRoom(networkInfo, userConfig, localCommunicationVersion, &attempt, &gameRoute);
                state_lock.lock();
                if (!lanywhere::FinishMatch(attempt, R_SUCCEEDED(rc))) {
                    this->autoRoom = this->ownsMatchRoom = false;
                    this->hostMember = -1;
                    this->onDisconnectFromHost();
                    return R_FAILED(rc) ? rc : MAKERESULT(ModuleID, 31);
                }
                return rc;
            }
            if (code[0]) {
                if (R_FAILED(lanywhere::JoinRoom(code, networkInfo->networkId.intentId.localCommunicationId, &gameRoute.discovery, &this->ownedGameRoute))) {
                    lanywhere::Log("ldn: no se pudo entrar en la sala %s", code);
                    return MAKERESULT(ModuleID, 31);
                }
                this->autoRoom = true;
                state_lock.unlock();
                Result rc = this->connectRoom(networkInfo, userConfig, localCommunicationVersion, nullptr, &this->ownedGameRoute);
                state_lock.lock();
                if (R_FAILED(rc)) {
                    this->leaveAutoRoom();
                }
                return rc;
            }
        }
        if (gameRoute.room_epoch) {
            state_lock.unlock();
            return this->connectRoom(networkInfo, userConfig, localCommunicationVersion, nullptr, &gameRoute);
        }
        if (gameRoute.discovery.public_enabled) {
            // An online scan must resolve to its captured relay room. Never
            // fall back to a physical host address from an unbound result.
            lanywhere::ReportGameRouteChanged();
            return lanywhere::ResultRelayFailed();
        }
        u32 hostIp = networkInfo->ldn.nodes[0].ipv4Address;
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(hostIp);
        addr.sin_port = htons(listenPort);
        LogFormat("connect hostIp %x", hostIp);

        Result rc = this->initTcp(false);
        if (R_FAILED(rc)) {
            return rc;
        }

        int ret = ::connect(this->tcp->getFd(), (struct sockaddr *)&addr, sizeof(addr));
        if (ret != 0) {
            LogFormat("connect failed");
            return MAKERESULT(ModuleID, 31);
        }

        NodeInfo myNode{};
        rc = this->getNodeInfo(&myNode, userConfig, localCommunicationVersion);
        if (R_FAILED(rc)) {
            return rc;
        }
        ret = this->tcp->sendPacket(LANPacketType::Connect, &myNode, sizeof(myNode));
        if (ret < 0) {
            LogFormat("sendPacket failed");
            return MAKERESULT(ModuleID, 32);
        }
        this->initNodeStateChange();

        state_lock.unlock();
        svcSleepThread(1000000000L); // 1sec

        return 0;
    }

    // LANywhere: unirse al grupo de otro miembro de la sala. Espera su Sync como mucho 3 s.
    Result LANDiscovery::connectRoom(const NetworkInfo *networkInfo, UserConfig *userConfig, u16 localCommunicationVersion,
                                     const lanywhere::MatchJoinAttempt *match, const lanywhere::GameRoute *expectedRoute) {
        std::scoped_lock lifecycle(this->lifecycleMutex);
        std::unique_lock state_lock(this->pollMutex);
        if (!this->initialized || this->state != CommState::Station) return MAKERESULT(LdnModuleId, 32);
        if (match && !lanywhere::MatchJoinPending(*match)) return MAKERESULT(ModuleID, 31);
        lanywhere::GameRoute route{};
        if (!lanywhere::CaptureGameRoute(&route) || !route.room_epoch) return lanywhere::ResultRelayFailed();
        if (expectedRoute && (route.discovery.ipv4 != expectedRoute->discovery.ipv4 ||
            route.discovery.port != expectedRoute->discovery.port ||
            std::strcmp(route.discovery.provider, expectedRoute->discovery.provider) ||
            (expectedRoute->room_epoch && (route.room_epoch != expectedRoute->room_epoch ||
                route.room_token != expectedRoute->room_token ||
                std::memcmp(route.room_code, expectedRoute->room_code, lanywhere::CodeLength))))) {
            lanywhere::ReportGameRouteChanged();
            return lanywhere::ResultRelayFailed();
        }
        u64 checked = networkInfo->networkId.intentId.localCommunicationId;
        R_TRY(this->resolveGameLcid(&checked, route));
        if (checked != networkInfo->networkId.intentId.localCommunicationId) {
            lanywhere::ReportGameRouteChanged();
            return lanywhere::ResultRelayFailed();
        }
        if (static_cast<u16>(networkInfo->ldn.nodes[0].localCommunicationVersion) != localCommunicationVersion) {
            return MAKERESULT(ModuleID, 31);
        }
        const int host = lanywhere::MemberOfIp(networkInfo->ldn.nodes[0].ipv4Address);
        lanywhere::Log("ldn: unirse al grupo del miembro %d", host);
        if (host < 0) {
            return MAKERESULT(ModuleID, 31);
        }
        NodeInfo myNode = {0};
        Result rc = this->getNodeInfo(&myNode, userConfig, localCommunicationVersion);
        if (R_FAILED(rc)) {
            return rc;
        }
        this->initNodeStateChange();
        this->onlineGame = true;
        this->hostMember = host;
        {
            std::scoped_lock lock(this->tcpBindingLock);
            tcpstream::GameSession pending{};pending.owner={this->callerProcessId,this->hostProgramId};pending.binding_id=this->tcpBindingId;
            u64 epoch=0;u32 capabilities=0,self_ip=0;
            if(lanywhere::CaptureTcpTransport(&epoch,&capabilities,&self_ip)){
                do {os::GenerateRandomBytes(&this->tcpSessionNonce,sizeof(this->tcpSessionNonce));}while(!this->tcpSessionNonce);
                this->tcpTransportEpoch=epoch;this->tcpSelfIp=self_ip;
                std::memcpy(this->tcpNetworkId,&networkInfo->networkId,32);
                pending.transport_epoch=epoch;pending.capabilities=capabilities;pending.self_ip=self_ip;pending.session_nonce=this->tcpSessionNonce;
                std::memcpy(pending.network_id,&networkInfo->networkId,32);
            }
            // Claim the trusted local owner before sending LDN CONNECT: the
            // host can publish admission and send TCP OPEN before we consume
            // its SYNC. A scanner alone never makes this ownership claim.
            tcpstream::Global().BeginBinding(pending);
        }
        if (!lanywhere::SendControl(host, lanywhere::LdnMsg::Connect, &myNode, sizeof(myNode), match)) {
            tcpstream::Global().EndBinding(this->tcpBindingId);
            this->hostMember = -1;
            return MAKERESULT(ModuleID, 32);
        }
        state_lock.unlock();
        for (int i = 0; i < 30 && this->getState() != CommState::StationConnected; i++) {
            if (match && !lanywhere::MatchJoinPending(*match)) break;
            svcSleepThread(100000000L); // 100ms
        }
        state_lock.lock();
        if (this->state != CommState::StationConnected) {
            tcpstream::Global().EndBinding(this->tcpBindingId);
            this->hostMember = -1;
            return MAKERESULT(ModuleID, 31);
        }
        return 0;
    }

    Result LANDiscovery::restoreNetworkResources() {
        Result result = 0;
        if (this->requestCreated) {
            result = nifmRequestCancel(&this->request);
            nifmRequestClose(&this->request);
            this->requestCreated = false;
            this->request = {};
        }
        return result;
    }

    Result LANDiscovery::finalize() {
        std::scoped_lock lifecycle(this->lifecycleMutex);
        this->stop = true;
        if (this->threadCreated) {
            // Never wait with pollMutex held: the worker may be leaving a callback.
            os::WaitThread(&this->workerThread);
            os::DestroyThread(&this->workerThread);
            this->threadCreated = false;
        }
        std::scoped_lock state_lock(this->pollMutex);
        tcpstream::Global().EndBinding(this->tcpBindingId);
        if (this->initialized) {
            this->roomCloseStations();
            this->leaveAutoRoom();
        }
        this->udp.reset();
        this->tcp.reset();
        this->resetStations();
        this->initialized = false;
        {
            std::scoped_lock scan_lock(this->scanResultsMutex);
            this->physicalScanActive = this->roomScanActive = false;
            this->publicGroupCount = 0;
            this->haveScanRoute = false;
        }
        const Result result = this->restoreNetworkResources();
        this->setState(CommState::None);
        this->lanEvent = EmptyFunc;
        return result;
    }

    Result LANDiscovery::initialize(LanEventFunc lanEvent, bool listening) {
        std::scoped_lock lifecycle(this->lifecycleMutex);
        std::scoped_lock state_lock(this->pollMutex);
        if (this->initialized) return 0;
        if (!this->stack) return MAKERESULT(ModuleID, 10);
        bool complete = false;
        ON_SCOPE_EXIT { if (!complete) static_cast<void>(this->finalize()); };
        this->lanEvent = lanEvent ? std::move(lanEvent) : EmptyFunc;
        this->disconnect_reason = DisconnectReason::None;

        // The virtual transports fragment their own datagrams and TCP uses
        // the kernel path MTU. Keep the existing shared Wi-Fi profile.
        R_TRY(nifmCreateRequest(&this->request, true));
        this->requestCreated = true;
        R_TRY(nifmSetLocalNetworkMode(&this->request, true));
        R_TRY(nifmRequestSubmit(&this->request));
        const auto began = os::GetSystemTick();
        for (;;) {
            NifmRequestState state{};
            R_TRY(nifmGetRequestState(&this->request, &state));
            if (state != NifmRequestState_OnHold) break;
            if ((os::GetSystemTick() - began).ToTimeSpan() >= TimeSpan::FromSeconds(10))
                return MAKERESULT(ModuleID, 20);
            svcSleepThread(10000000L);
        }
        R_TRY(nifmGetResult(&this->request));
        for (auto &station : this->stations) {
            station.nodeInfo = &this->networkInfo.ldn.nodes[station.nodeId];
            station.reset();
        }
        R_TRY(this->initUdp(listening));
        this->stop = false;
        R_TRY(os::CreateThread(&this->workerThread, &Worker, this,
            reinterpret_cast<void *>(util::AlignUp(reinterpret_cast<uintptr_t>(this->stack.get()), os::ThreadStackAlignment)),
            StackSize, 0x15, 2));
        this->threadCreated = true;
        this->initialized = true;
        this->setState(CommState::Initialized);
        os::StartThread(&this->workerThread);
        complete = true;
        return 0;
    }
}
