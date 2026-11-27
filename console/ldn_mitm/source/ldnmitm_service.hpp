#pragma once
#include <switch.h>
#include <stratosphere.hpp>
#include <cstring>
#include <atomic>
#include "ldn_icommunication.hpp"
#include "ldn_client_process_monitor.hpp"
#include "ldnmitm_config.hpp"
#include "ldn_accept_trace.hpp"
#include "interfaces/iservice.hpp"

namespace ams::mitm::ldn {

    class LdnMitMService : public sf::MitmServiceImplBase {
        private:
            using RedirectOnlyLocationResolverFactory = sf::ObjectFactory<sf::StdAllocationPolicy<std::allocator>>;
        public:
            LdnMitMService(std::shared_ptr<::Service> &&s, const sm::MitmProcessInfo &c);
            
            // SM waits for this answer while holding its global service registry lock.
            static bool ShouldMitm(const sm::MitmProcessInfo &client) {
                if (ncm::IsApplicationId(client.program_id)) {
                    log::RecordLdnQuery(client.process_id.value, client.program_id.value);
                }
                return true;
            }
        // protected:
        public:
            /* Overridden commands. */
            Result CreateUserLocalCommunicationService(sf::Out<sf::SharedPointer<ICommunicationInterface>> out);
            Result CreateClientProcessMonitor(sf::Out<sf::SharedPointer<IClientProcessMonitorInterface>> out);
            Result CreateLdnMitmConfigService(sf::Out<sf::SharedPointer<ILdnConfig>> out);
    };
    static_assert(ams::mitm::ldn::IsILdnMitMService<LdnMitMService>);

}