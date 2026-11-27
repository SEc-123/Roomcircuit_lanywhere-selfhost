// SPDX-License-Identifier: GPL-2.0-only
#include "ldn_accept_trace.hpp"
#include "debug.hpp"
#include <array>
#include <atomic>
#include <cinttypes>
#include <mutex>

namespace ams::log {
    namespace {
        struct Record {
            const char *stage;
            LdnAcceptInfo info;
            u32 result;
            s64 milliseconds;
            u64 thread;
        };

        std::array<Record, 64> g_records{};
        std::size_t g_first{}, g_count{};
        constinit os::SdkMutex g_lock;
        constinit os::SdkConditionVariable g_available;
        std::atomic<u32> g_dropped{};
    }

    void RecordLdnAccept(const char *stage, const LdnAcceptInfo &info, u32 result) {
        u32 enabled{};
        GetLogging(&enabled);
        if (!enabled) return;

        std::unique_lock lock(g_lock, std::try_to_lock);
        if (!lock.owns_lock()) {
            ++g_dropped;
            return;
        }
        if (g_count == g_records.size()) {
            g_first = (g_first + 1) % g_records.size();
            --g_count;
            ++g_dropped;
        }
        g_records[(g_first + g_count) % g_records.size()] = {
            stage, info, result, static_cast<s64>(os::GetSystemTick().ToTimeSpan().GetMilliSeconds()),
            os::GetThreadId(os::GetCurrentThread())
        };
        ++g_count;
        g_available.Signal();
    }

    void DrainLdnAcceptTraceUntilExit(os::ThreadType *service_thread) {
        for (;;) {
            const bool stopped = os::TryWaitThread(service_thread);
            Record entry{};
            u32 dropped{};
            {
                std::scoped_lock lock(g_lock);
                if (!g_count) {
                    if (stopped) return;
                    g_available.TimedWait(g_lock, TimeSpan::FromSeconds(1));
                    if (!g_count) continue;
                }
                entry = g_records[g_first];
                g_first = (g_first + 1) % g_records.size();
                --g_count;
                dropped = g_dropped.exchange(0);
            }
            // Only this consumer writes these records to SD. A hard power-off can lose queued stages.
            LogFormat("ldn.accept version=%s at=%" PRId64 " tid=%" PRIu64 " stage=%s pid=%" PRIu64
                      " title=%016" PRIx64 " rc=%08x pointer=%u/%u max_sessions=%u dropped=%u",
                      GITDESCVER, entry.milliseconds, entry.thread, entry.stage, entry.info.pid,
                      entry.info.title, entry.result, entry.info.forward_pointer_size,
                      entry.info.pointer_capacity, entry.info.session_limit, dropped);
        }
    }
}
