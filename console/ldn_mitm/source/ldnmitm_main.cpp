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

#include <stratosphere.hpp>

#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <malloc.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include <switch.h>

extern "C" {

#include <switch/services/bsd.h>

}

#include "ldnmitm_service.hpp"
#include "ldn_accept_trace.hpp"
#include <atomic>
#include "bsd_spy.hpp"
#include "lanywhere_service.hpp"
#include "appointment_notifier.hpp"
#include "storage_migration.hpp"

namespace ams {

    namespace {

        constexpr size_t MallocBufferSize = 1_MB;
        alignas(os::MemoryPageSize) constinit u8 g_malloc_buffer[MallocBufferSize];

        consteval size_t GetLibnxBsdTransferMemorySize(const ::SocketInitConfig *config) {
            const u32 tcp_tx_buf_max_size = config->tcp_tx_buf_max_size != 0 ? config->tcp_tx_buf_max_size : config->tcp_tx_buf_size;
            const u32 tcp_rx_buf_max_size = config->tcp_rx_buf_max_size != 0 ? config->tcp_rx_buf_max_size : config->tcp_rx_buf_size;
            const u32 sum = tcp_tx_buf_max_size + tcp_rx_buf_max_size + config->udp_tx_buf_size + config->udp_rx_buf_size;

            return config->sb_efficiency * util::AlignUp(sum, os::MemoryPageSize);
        }

        constexpr const ::SocketInitConfig LibnxSocketInitConfig = {
            .tcp_tx_buf_size = 0x800,
            .tcp_rx_buf_size = 0x1000,
            .tcp_tx_buf_max_size = 0x2000,
            .tcp_rx_buf_max_size = 0x2000,

            // LANywhere: los paquetes del juego van por un solo socket UDP con el servidor, a ráfagas de ~1.4 KB.
            .udp_tx_buf_size = 0x8000,
            .udp_rx_buf_size = 0x8000,

            .sb_efficiency = 4,

            .num_bsd_sessions = 3,
            .bsd_service_type = BsdServiceType_User,
        };

        alignas(os::MemoryPageSize) constinit u8 g_socket_tmem_buffer[GetLibnxBsdTransferMemorySize(std::addressof(LibnxSocketInitConfig))];

        constexpr const ::BsdInitConfig LibnxBsdInitConfig = {
            .version             = 1,

            .tmem_buffer         = g_socket_tmem_buffer,
            .tmem_buffer_size    = sizeof(g_socket_tmem_buffer),

            .tcp_tx_buf_size     = LibnxSocketInitConfig.tcp_tx_buf_size,
            .tcp_rx_buf_size     = LibnxSocketInitConfig.tcp_rx_buf_size,
            .tcp_tx_buf_max_size = LibnxSocketInitConfig.tcp_tx_buf_max_size,
            .tcp_rx_buf_max_size = LibnxSocketInitConfig.tcp_rx_buf_max_size,

            .udp_tx_buf_size     = LibnxSocketInitConfig.udp_tx_buf_size,
            .udp_rx_buf_size     = LibnxSocketInitConfig.udp_rx_buf_size,

            .sb_efficiency       = LibnxSocketInitConfig.sb_efficiency,
        };

    }

    namespace mitm {

        const s32 ThreadPriority = 6;
        // LANywhere: bsd:u (sockets de los juegos) tiene llamadas que bloquean (recv, poll, accept): con
        // pocos hilos, unas pocas esperando datos dejarían sin atender al resto.
        const size_t TotalThreads = 12;
        const size_t NumExtraThreads = TotalThreads - 1;
        const size_t ThreadStackSize = 0x4000;

        alignas(os::MemoryPageSize) u8 g_thread_stack[ThreadStackSize];
        os::ThreadType g_thread;

        alignas(0x40) constinit u8 g_heap_memory[512_KB];
        constinit lmem::HeapHandle g_heap_handle;
        constinit bool g_heap_initialized;
        constinit os::SdkMutex g_heap_init_mutex;

        lmem::HeapHandle GetHeapHandle()
        {
            if (AMS_UNLIKELY(!g_heap_initialized))
            {
                std::scoped_lock lk(g_heap_init_mutex);

                if (AMS_LIKELY(!g_heap_initialized))
                {
                    g_heap_handle = lmem::CreateExpHeap(g_heap_memory, sizeof(g_heap_memory), lmem::CreateOption_ThreadSafe);
                    g_heap_initialized = true;
                }
            }

            return g_heap_handle;
        }

        void *Allocate(size_t size)
        {
            return lmem::AllocateFromExpHeap(GetHeapHandle(), std::max<size_t>(size, 1));
        }

        void Deallocate(void *p, size_t size)
        {
            AMS_UNUSED(size);
            if (p) lmem::FreeToExpHeap(GetHeapHandle(), p);
        }

        namespace {

            struct LdnMitmManagerOptions {
                static constexpr size_t PointerBufferSize = 0x1000;
                static constexpr size_t MaxDomains = 0x10;
                static constexpr size_t MaxDomainObjects = 0x100;
                static constexpr bool   CanDeferInvokeRequest = false;
                static constexpr bool   CanManageMitmServers  = true;
            };

            constexpr size_t MaxSessions = 48;

            enum PortIndex { PortIndex_Ldn = 0, PortIndex_Bsd = 1, PortIndex_Lanywhere = 2, PortIndex_Count };

            // The LDN port holder is unlinked during each handoff and relinked on return.
            // Only that worker reads the borrowed context; BSD workers ignore the observer.
            std::atomic<const os::ThreadType *> g_ldn_accept_worker{};
            const log::LdnAcceptInfo *g_traced_accept = nullptr;

            class ServerManager final : public sf::hipc::ServerManager<PortIndex_Count, LdnMitmManagerOptions, MaxSessions> {
                        private:
                            virtual ams::Result OnNeedsToAccept(int port_index, Server *server) override;
                            void OnMitmAcceptStage(MitmAcceptStage stage) override {
                                if (g_ldn_accept_worker.load() != os::GetCurrentThread()) return;
                                switch (stage) {
                                    case MitmAcceptStage::SlotReserved:    log::RecordLdnAccept("slot.reserved", *g_traced_accept); break;
                                    case MitmAcceptStage::KernelAccepted:  log::RecordLdnAccept("kernel.accepted", *g_traced_accept); break;
                                    case MitmAcceptStage::RegisteringWait: log::RecordLdnAccept("wait.registering", *g_traced_accept); break;
                                }
                            }
            };

            ServerManager g_server_manager;

            Result ServerManager::OnNeedsToAccept(int port_index, Server *server) {
                if (port_index == PortIndex_Lanywhere) {
                    const Result rc = this->AcceptImpl(server, sf::CreateSharedObjectEmplaced<mitm::lanywhere::ILanywhereService, mitm::lanywhere::LanywhereService>());
                    return this->RecoverAcceptFailure(server, rc);
                }

                std::shared_ptr<::Service> fsrv;
                sm::MitmProcessInfo client_info;
                log::LdnAcceptInfo info{.pointer_capacity = LdnMitmManagerOptions::PointerBufferSize, .session_limit = MaxSessions};
                info.bsd_service = port_index == PortIndex_Bsd;
                log::RecordLdnAccept("ack.begin", info);
                server->AcknowledgeMitmSession(std::addressof(fsrv), std::addressof(client_info));
                info.pid = client_info.process_id.value;
                info.title = client_info.program_id.value;
                info.forward_pointer_size = fsrv->pointer_buffer_size;
                log::RecordLdnAccept("ack.done", info);
                if (port_index == PortIndex_Bsd) {
                    const Result rc = this->AcceptMitmImpl(server, sf::CreateSharedObjectEmplaced<mitm::bsd::IBsdSpy, mitm::bsd::BsdSpyService>(decltype(fsrv)(fsrv), client_info), fsrv);
                    log::RecordLdnAccept("accept.done", info, rc.GetValue());
                    return this->RecoverAcceptFailure(server, rc);
                }

                auto object = sf::CreateSharedObjectEmplaced<mitm::ldn::ILdnMitMService, mitm::ldn::LdnMitMService>(decltype(fsrv)(fsrv), client_info);
                log::RecordLdnAccept("object.ready", info);
                g_traced_accept = &info;
                g_ldn_accept_worker.store(os::GetCurrentThread());
                ON_SCOPE_EXIT { g_ldn_accept_worker.store(nullptr); g_traced_accept = nullptr; };
                const Result rc = this->AcceptMitmImpl(server, std::move(object), fsrv);
                log::RecordLdnAccept("accept.done", info, rc.GetValue());
                return this->RecoverAcceptFailure(server, rc);
            }

            alignas(os::MemoryPageSize) u8 g_extra_thread_stacks[NumExtraThreads][ThreadStackSize];
            os::ThreadType g_extra_threads[NumExtraThreads];

            void LoopServerThread(void *)
            {
                g_server_manager.LoopProcess();
            }

            void ProcessForServerOnAllThreads(void *)
            {
                /* Initialize threads. */
                if constexpr (NumExtraThreads > 0)
                {
                    const s32 priority = os::GetThreadCurrentPriority(os::GetCurrentThread());
                    for (size_t i = 0; i < NumExtraThreads; i++)
                    {
                        R_ABORT_UNLESS(os::CreateThread(g_extra_threads + i, LoopServerThread, nullptr, g_extra_thread_stacks[i], ThreadStackSize, priority));
                        os::SetThreadNamePointer(g_extra_threads + i, "ldn_mitm::Thread");
                    }
                }

                // Reserve the mandatory service workers before starting optional
                // node discovery/retry threads. Resource exhaustion in the
                // optional work is handled without aborting the sysmodule.
                mitm::lanywhere::InitializeNodeSelection();

                /* Start extra threads. */
                if constexpr (NumExtraThreads > 0)
                {
                    for (size_t i = 0; i < NumExtraThreads; i++)
                    {
                        os::StartThread(g_extra_threads + i);
                    }
                }

                /* Loop this thread. */
                LoopServerThread(nullptr);

                /* Wait for extra threads to finish. */
                if constexpr (NumExtraThreads > 0)
                {
                    for (size_t i = 0; i < NumExtraThreads; i++)
                    {
                        os::WaitThread(g_extra_threads + i);
                    }
                }
            }
        }

    }

    namespace init {

        void InitializeSystemModule() {
            /* Initialize our connection to sm. */
            R_ABORT_UNLESS(sm::Initialize());

            /* Initialize fs. */
            fs::InitializeForSystem();
            fs::SetAllocator(mitm::Allocate, mitm::Deallocate);
            fs::SetEnabledAutoAbort(false);

            /* Mount the SD card. */
            R_ABORT_UNLESS(fs::MountSdCard("sdmc"));

            /* Initialize other services. */

            R_ABORT_UNLESS(bsdInitialize(&LibnxBsdInitConfig, LibnxSocketInitConfig.num_bsd_sessions, LibnxSocketInitConfig.bsd_service_type));
            R_ABORT_UNLESS(socketInitialize(&LibnxSocketInitConfig));
        }

        void FinalizeSystemModule() { /* ... */ }

        void Startup() {
            /* Initialize the global malloc allocator. */
            init::InitializeAllocator(g_malloc_buffer, sizeof(g_malloc_buffer));
        }

    }

    void NORETURN Exit(int rc) {
        AMS_UNUSED(rc);
        AMS_ABORT("Exit called by immortal process");
    }

    void Main() {
        static_cast<void>(mitm::storage::Initialize());
        if (!mitm::storage::StorageAvailable()) {
            // Do not expose LDN, BSD or RoomCircuit services when storage's
            // committed generation cannot be determined. Keep the console
            // usable; a reboot retries after the SD issue has been resolved.
            while (true) os::SleepThread(TimeSpan::FromSeconds(60));
        }
        LogFormat("main");
        mitm::lanywhere::InitializeAppointmentSubscriptions();

        constexpr sm::ServiceName MitmServiceName = sm::ServiceName::Encode("ldn:u");
        //sf::hipc::ServerManager<2, LdnMitmManagerOptions, 3> server_manager;
        R_ABORT_UNLESS((mitm::g_server_manager.RegisterMitmServer<mitm::ldn::LdnMitMService>(mitm::PortIndex_Ldn, MitmServiceName)));
    // LANywhere: observación de los sockets de los juegos.
    R_ABORT_UNLESS((mitm::g_server_manager.RegisterMitmServer<mitm::bsd::BsdSpyService>(mitm::PortIndex_Bsd, sm::ServiceName::Encode("bsd:u"))));
    // LANywhere: control de salas (crear, unirse, salir, estado).
    R_ABORT_UNLESS((mitm::g_server_manager.RegisterServer(mitm::PortIndex_Lanywhere, sm::ServiceName::Encode("lanywhr"), 4)));
        LogFormat("registered");

        R_ABORT_UNLESS(os::CreateThread(
            &mitm::g_thread,
            mitm::ProcessForServerOnAllThreads,
            nullptr,
            mitm::g_thread_stack,
            mitm::ThreadStackSize,
            mitm::ThreadPriority));

        os::SetThreadNamePointer(&mitm::g_thread, "ldn_mitm::MainThread");
        os::StartThread(&mitm::g_thread);

        log::DrainLdnAcceptTraceUntilExit(&mitm::g_thread);
        os::WaitThread(&mitm::g_thread);
    }

}

void *operator new(size_t size)
{
    void *allocation = ams::mitm::Allocate(size);
    AMS_ABORT_UNLESS(allocation != nullptr);
    return allocation;
}

void *operator new(size_t size, const std::nothrow_t &)
{
    return ams::mitm::Allocate(size);
}

void operator delete(void *p)
{
    return ams::mitm::Deallocate(p, 0);
}

void operator delete(void *p, size_t size)
{
    return ams::mitm::Deallocate(p, size);
}

void *operator new[](size_t size)
{
    void *allocation = ams::mitm::Allocate(size);
    AMS_ABORT_UNLESS(allocation != nullptr);
    return allocation;
}

void *operator new[](size_t size, const std::nothrow_t &)
{
    return ams::mitm::Allocate(size);
}

void operator delete[](void *p)
{
    return ams::mitm::Deallocate(p, 0);
}

void operator delete[](void *p, size_t size)
{
    return ams::mitm::Deallocate(p, size);
}
