/* Modified by LANywhere Self-host, 2026-11-07 to 2026-11-08. */
#include "ldn_icommunication.hpp"
#include <arpa/inet.h>
#include "lanywhere_relay.hpp"

namespace ams::mitm::ldn {
    static_assert(sizeof(NetworkInfo) == 0x480, "sizeof(NetworkInfo) should be 0x480");
    static_assert(sizeof(ConnectNetworkData) == 0x7C, "sizeof(ConnectNetworkData) should be 0x7C");
    static_assert(sizeof(ScanFilter) == 0x60, "sizeof(ScanFilter) should be 0x60");

    namespace {
        Result UnsupportedLdnOperation(const char *operation) {
            LogFormat("Unsupported LDN operation: %s", operation);
            return sf::ResultNotSupported();
        }
    }

    // https://reswitched.github.io/SwIPC/ifaces.html#nn::ldn::detail::IUserLocalCommunicationService

    Result ICommunicationService::Initialize(const sf::ClientProcessId &client_process_id) {
        std::scoped_lock lifecycle(this->lifecycle_mutex);
        LogFormat("ICommunicationService::Initialize pid: %" PRIu64, client_process_id);

        {
            std::scoped_lock event_lock(this->event_mutex);
            if (this->state_event == nullptr) {
                auto event = std::unique_ptr<os::SystemEvent>(new (std::nothrow) os::SystemEvent());
                if (!event) return MAKERESULT(0xCB, 32);
                R_TRY(os::CreateSystemEvent(event->GetBase(), os::EventClearMode_AutoClear, true));
                this->state_event = event.release();
            }
        }

        LogFormat("ldn init.event.ready");
        const Result rc = lanDiscovery.initialize([&](){
            this->onEventFired();
        });
        LogFormat("ldn init.done rc=%#x", rc.GetValue());
        return rc;
    }

    Result ICommunicationService::InitializeSystem2(u64 unk, const sf::ClientProcessId &client_process_id) {
        LogFormat("ICommunicationService::InitializeSystem2 unk: %" PRIu64, unk);
        this->error_state = unk;
        return this->Initialize(client_process_id);
    }

    Result ICommunicationService::Finalize() {
        std::scoped_lock lifecycle(this->lifecycle_mutex);
        Result rc = lanDiscovery.finalize();
        std::scoped_lock event_lock(this->event_mutex);
        if (this->state_event) {
            delete this->state_event;
            this->state_event = nullptr;
        }
        return rc;
    }

    Result ICommunicationService::OpenAccessPoint() {
        LogFormat("ldn ap.open.begin");
        const Result rc = this->lanDiscovery.openAccessPoint();
        LogFormat("ldn ap.open.done rc=%#x", rc.GetValue());
        return rc;
    }

    Result ICommunicationService::CloseAccessPoint() {
        return this->lanDiscovery.closeAccessPoint();
    }

    Result ICommunicationService::DestroyNetwork() {
        return this->lanDiscovery.destroyNetwork();
    }

    Result ICommunicationService::OpenStation() {
        LogFormat("ldn station.open.begin");
        const Result rc = this->lanDiscovery.openStation();
        LogFormat("ldn station.open.done rc=%#x", rc.GetValue());
        return rc;
    }

    Result ICommunicationService::CloseStation() {
        return this->lanDiscovery.closeStation();
    }

    Result ICommunicationService::Disconnect() {
        return this->lanDiscovery.disconnect();
    }

    Result ICommunicationService::CreateNetwork(CreateNetworkConfig data) {
        LogFormat("ldn ap.create.begin");
        const Result rc = this->lanDiscovery.createNetwork(&data.securityConfig, &data.userConfig, &data.networkConfig);
        LogFormat("ldn ap.create.done rc=%#x", rc.GetValue());
        return rc;
    }

    Result ICommunicationService::SetAdvertiseData(sf::InAutoSelectBuffer data) {
        if (data.GetSize() > AdvertiseDataSizeMax) return MAKERESULT(0xCB, 32);
        return lanDiscovery.setAdvertiseData(data.GetPointer(), data.GetSize());
    }

    Result ICommunicationService::GetState(sf::Out<u32> state) {
        state.SetValue(static_cast<u32>(this->lanDiscovery.getState()));

        if (this->error_state) {
            if (this->lanDiscovery.getDisconnectReason() != DisconnectReason::None) {
                return MAKERESULT(0x10, static_cast<u32>(this->lanDiscovery.getDisconnectReason()));
            }
        }

        return 0;
    }

    Result ICommunicationService::GetIpv4Address(sf::Out<u32> address, sf::Out<u32> netmask) {
        u32 gateway = 0, primary_dns = 0, secondary_dns = 0;
        address.SetValue(0);
        netmask.SetValue(0);
        Result rc = nifmGetCurrentIpConfigInfo(address.GetPointer(), netmask.GetPointer(), &gateway, &primary_dns, &secondary_dns);

        address.SetValue(ntohl(address.GetValue()));
        netmask.SetValue(ntohl(netmask.GetValue()));
        // LANywhere: en una sala, el juego vive en la red virtual.
        if (lanywhere::InRoom()) {
            address.SetValue(lanywhere::MyIp());
            netmask.SetValue(lanywhere::Netmask());
            rc = 0;
        }

        LogFormat("get_ipv4_address %x %x", address.GetValue(), netmask.GetValue());

        return rc;
    }

    Result ICommunicationService::GetNetworkInfo(sf::Out<NetworkInfo> buffer) {
        LogFormat("get_network_info %p state: %d", buffer.GetPointer(), static_cast<u32>(this->lanDiscovery.getState()));

        return lanDiscovery.getNetworkInfo(buffer.GetPointer());
    }

    Result ICommunicationService::GetDisconnectReason(sf::Out<u32> reason) {
        auto dr = static_cast<u32>(this->lanDiscovery.getDisconnectReason());
        LogFormat("GetDisconnectReason %p state: %d reason: %u", reason.GetPointer(), static_cast<u32>(this->lanDiscovery.getState()), dr);
        reason.SetValue(dr);

        return 0;
    }

    Result ICommunicationService::GetNetworkInfoLatestUpdate(sf::Out<NetworkInfo> buffer, sf::OutArray<NodeLatestUpdate> pUpdates) {
        LogFormat("get_network_info_latest buffer %p", buffer.GetPointer());
        LogFormat("get_network_info_latest pUpdates %p %" PRIu64, pUpdates.GetPointer(), pUpdates.GetSize());

        return lanDiscovery.getNetworkInfo(buffer.GetPointer(), pUpdates.GetPointer(), pUpdates.GetSize());
    }

    Result ICommunicationService::GetSecurityParameter(sf::Out<SecurityParameter> out) {
        Result rc = 0;

        SecurityParameter data{};
        NetworkInfo info{};
        rc = lanDiscovery.getNetworkInfo(&info);
        if (R_SUCCEEDED(rc)) {
            NetworkInfo2SecurityParameter(&info, &data);
            out.SetValue(data);
        }

        return rc;
    }

    Result ICommunicationService::GetNetworkConfig(sf::Out<NetworkConfig> out) {
        Result rc = 0;

        NetworkConfig data{};
        NetworkInfo info{};
        rc = lanDiscovery.getNetworkInfo(&info);
        if (R_SUCCEEDED(rc)) {
            NetworkInfo2NetworkConfig(&info, &data);
            out.SetValue(data);
        }

        return rc;
    }

    Result ICommunicationService::AttachStateChangeEvent(sf::Out<sf::CopyHandle> handle) {
        std::scoped_lock event_lock(this->event_mutex);
        if (!this->state_event) return MAKERESULT(0xCB, 32);
        handle.SetValue(this->state_event->GetReadableHandle(), false);
        return ResultSuccess();
    }

    Result ICommunicationService::Scan(sf::Out<u32> outCount, sf::OutAutoSelectArray<NetworkInfo> buffer, u16 channel, ScanFilter filter) {
		AMS_UNUSED(channel);
        Result rc = 0;
        if (buffer.GetSize() > UINT16_MAX) return MAKERESULT(0xCB, 32);
        u16 count = static_cast<u16>(buffer.GetSize());

        if (!this->tracedScan.exchange(true, std::memory_order_relaxed)) LogFormat("ldn station.scan.first");
        rc = lanDiscovery.scan(buffer.GetPointer(), &count, filter);
        outCount.SetValue(count);

        LogFormat("ldn station.scan.done count=%u rc=%#x", static_cast<unsigned>(count), rc.GetValue());

        return rc;
    }

    Result ICommunicationService::Connect(ConnectNetworkData param, const NetworkInfo &data) {
        LogFormat("ICommunicationService::connect");

        return lanDiscovery.connect(&data, &param.userConfig, param.localCommunicationVersion);
    }

    void ICommunicationService::onEventFired() {
        std::scoped_lock event_lock(this->event_mutex);
        if (this->state_event) {
            LogFormat("onEventFired signal_event");
            this->state_event->Signal();
        }
    }

    Result ICommunicationService::SetStationAcceptPolicy(u8 policy) {
        return this->lanDiscovery.setStationAcceptPolicy(policy);
    }

    Result ICommunicationService::SetWirelessControllerRestriction() {
        // Preserve compatibility for games that request controller policy during LDN initialization.
        // The virtual transport leaves the physical controller policy to Horizon.
        return ResultSuccess();
    }

    Result ICommunicationService::SetProtocol(u32 protocol) {
        // The game selects a radio protocol during setup. Acknowledge the request
        // while keeping the existing virtual LDN transport.
        LogFormat("ICommunicationService::SetProtocol: %u", protocol);
        return ResultSuccess();
    }

    Result ICommunicationService::ScanPrivate() {
        return UnsupportedLdnOperation("ScanPrivate");
    }

    Result ICommunicationService::CreateNetworkPrivate() {
        return UnsupportedLdnOperation("CreateNetworkPrivate");
    }

    Result ICommunicationService::Reject() {
        return UnsupportedLdnOperation("Reject");
    }

    Result ICommunicationService::AddAcceptFilterEntry() {
        return UnsupportedLdnOperation("AddAcceptFilterEntry");
    }

    Result ICommunicationService::ClearAcceptFilter() {
        // Filter entries are unsupported, so there is nothing to clear in an open AP.
        const auto state = this->lanDiscovery.getState();
        if (state != CommState::AccessPoint && state != CommState::AccessPointCreated) {
            return MAKERESULT(0xCB, 32);
        }
        return ResultSuccess();
    }

    Result ICommunicationService::ConnectPrivate() {
        return UnsupportedLdnOperation("ConnectPrivate");
    }
}
