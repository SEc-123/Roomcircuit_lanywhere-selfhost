/* Modified by LANywhere Self-host, 2026-11-07 to 2026-11-08. */
/*
 * Copyright (c) 2018 Atmosphère-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
 
#include <switch.h>
#include "debug.hpp"

#include "ldnmitm_service.hpp"
#include "ldn_icommunication.hpp"
#include "ldn_client_process_monitor.hpp"

namespace ams::mitm::ldn {

    using ObjectFactory = ams::sf::ObjectFactory<ams::sf::StdAllocationPolicy<std::allocator>>;

    LdnMitMService::LdnMitMService(std::shared_ptr<::Service> &&s, const sm::MitmProcessInfo &c) : sf::MitmServiceImplBase(std::forward<std::shared_ptr<::Service>>(s), c)
    {
        LogFormat("ldn service.created pid=%" PRIu64 " title=%016" PRIx64,
                  c.process_id.value, c.program_id.value);
    }

    Result LdnMitMService::CreateUserLocalCommunicationService(sf::Out<sf::SharedPointer<ICommunicationInterface>> out) {
        const bool enabled = LdnConfig::getEnabled();
        LogFormat("ldn child.begin cmd=0 pid=%" PRIu64 " enabled=%u", m_client_info.process_id.value, static_cast<u32>(enabled));

        if (enabled) {
            // Bind identity to this accepted ldn:u session, not a mutable global
            // or the unrelated local-communication identifier supplied by a game.
            out.SetValue(sf::CreateSharedObjectEmplaced<ICommunicationInterface, ICommunicationService>(m_client_info.program_id.value,m_client_info.process_id.value));
            LogFormat("ldn child.ready pid=%" PRIu64, m_client_info.process_id.value);
            return 0;
        }

        LogFormat("ldn child.forward pid=%" PRIu64, m_client_info.process_id.value);
        return sm::mitm::ResultShouldForwardToSession();
    }

    Result LdnMitMService::CreateClientProcessMonitor(sf::Out<sf::SharedPointer<IClientProcessMonitorInterface>> out) {
        LogFormat("ldn monitor.begin cmd=1 pid=%" PRIu64, m_client_info.process_id.value);

        // Always create the monitor, regardless of ldn_mitm being enabled
        // This is required for firmware 18.0.0+ compatibility (lo necesitan algunos juegos)
        out.SetValue(sf::CreateSharedObjectEmplaced<IClientProcessMonitorInterface, IClientProcessMonitor>());
        return 0;
    }

    Result LdnMitMService::CreateLdnMitmConfigService(sf::Out<sf::SharedPointer<ILdnConfig>> out) {
        LogFormat("ldn config.begin cmd=65000 pid=%" PRIu64, m_client_info.process_id.value);
        out.SetValue(sf::CreateSharedObjectEmplaced<ILdnConfig, LdnConfig>());
        return 0;
    }
}
