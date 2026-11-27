// SPDX-License-Identifier: GPL-2.0-only
#include "ldn_accept_trace.hpp"
#include "debug.hpp"
#include <array>
#include <atomic>
#include <cinttypes>
#include <mutex>

namespace ams::log {
    namespace {
        struct QuerySnapshot {
            std::atomic<u64> sequence{}, session{}, pid{}, title{};
        };
        static_assert(std::atomic<u64>::is_always_lock_free);
        constinit QuerySnapshot g_ldn_query, g_bsd_query;

        void DrainQuery(const char *service, const QuerySnapshot &query, u64 &seen) {
            const u64 seq = query.sequence.load();
            if (seq == seen || (seq & 1u)) return;
            const u64 session = query.session.load();
            const u64 pid = query.pid.load();
            const u64 title = query.title.load();
            if (seq != query.sequence.load()) return;  // Retry on the next drain iteration.
            const u64 skipped = (seq - seen) / 2 - 1;
            seen = seq;
            LogFormatForSession(session, "%s.query version=%s seq=%" PRIu64 " pid=%" PRIu64
                                " title=%016" PRIx64 " skipped=%" PRIu64 "\n",
                                service, GITDESCVER, seq / 2, pid, title, skipped);
        }

        void RecordQuery(QuerySnapshot &query, u64 pid, u64 title) {
            const u64 session = LoggingSession();
            if (!session) return;
            // Atmosphere runs all MITM queries on one thread. Odd marks a write in progress.
            const u64 seq = query.sequence.load();
            query.sequence.store(seq + 1);
            query.session.store(session);
            query.pid.store(pid);
            query.title.store(title);
            query.sequence.store(seq + 2);
        }

        struct Record {
            const char *stage;
            LdnAcceptInfo info;
            u32 result;
            s64 milliseconds;
            u64 thread;
            u64 session;
        };

        std::array<Record, 64> g_records{};
        std::size_t g_first{}, g_count{};
        constinit os::SdkMutex g_lock;
        constinit os::SdkConditionVariable g_available;
        u32 g_dropped{};
        u64 g_queue_session{};
    }

    void RecordLdnQuery(u64 pid, u64 title) { RecordQuery(g_ldn_query, pid, title); }
    void RecordBsdQuery(u64 pid, u64 title) { RecordQuery(g_bsd_query, pid, title); }

    void RecordLdnAccept(const char *stage, const LdnAcceptInfo &info, u32 result) {
        const u64 session = LoggingSession();
        if (!session) return;
        const Record record{stage, info, result,
            static_cast<s64>(os::GetSystemTick().ToTimeSpan().GetMilliSeconds()),
            os::GetThreadId(os::GetCurrentThread()), session};

        bool wake_writer;
        {
            // The consumer only copies a record under this mutex. SD access is outside it.
            std::scoped_lock lock(g_lock);
            if (LoggingSession() != session) return;
            if (g_queue_session != session) {
                g_queue_session = session;
                g_first = g_count = g_dropped = 0;
            }
            wake_writer = (g_count == 0);
            if (g_count == g_records.size()) {
                g_first = (g_first + 1) % g_records.size();
                --g_count;
                ++g_dropped;
            }
            g_records[(g_first + g_count) % g_records.size()] = record;
            ++g_count;
        }
        if (wake_writer) g_available.Signal();
    }

    void DrainLdnAcceptTraceUntilExit(os::ThreadType *service_thread) {
        constexpr s64 HeartbeatIntervalMs = 2000;
        u64 seen_query = 0, seen_bsd_query = 0, writer_session = 0;
        s64 last_report_ms = 0;
        for (;;) {
            const u64 session = LoggingSession();
            if (session && session != writer_session) {
                writer_session = session;
                last_report_ms = os::GetSystemTick().ToTimeSpan().GetMilliSeconds();
                LogFormatForSession(session, "ldn.writer.ready version=%s session=%" PRIu64 "\n", GITDESCVER, session);
            }
            DrainQuery("ldn", g_ldn_query, seen_query);
            DrainQuery("bsd", g_bsd_query, seen_bsd_query);
            if (session) {
                const s64 now = os::GetSystemTick().ToTimeSpan().GetMilliSeconds();
                if (now - last_report_ms >= HeartbeatIntervalMs) {
                    last_report_ms = now;
                    LogFormatForSession(session, "ldn.writer.alive version=%s session=%" PRIu64
                                        " ldn_drained_seq=%" PRIu64 " bsd_drained_seq=%" PRIu64 "\n",
                                        GITDESCVER, session,
                                        g_ldn_query.session.load() == session ? seen_query / 2 : 0,
                                        g_bsd_query.session.load() == session ? seen_bsd_query / 2 : 0);
                }
            }
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
                dropped = g_dropped;
                g_dropped = 0;
            }
            // Only this consumer writes these records to SD. A hard power-off can lose queued stages.
            LogFormatForSession(entry.session, "%s.accept version=%s at=%" PRId64 " tid=%" PRIu64 " stage=%s pid=%" PRIu64
                      " title=%016" PRIx64 " rc=%08x pointer=%u/%u max_sessions=%u dropped=%u\n",
                      entry.info.bsd_service ? "bsd" : "ldn", GITDESCVER, entry.milliseconds, entry.thread, entry.stage, entry.info.pid,
                      entry.info.title, entry.result, entry.info.forward_pointer_size,
                      entry.info.pointer_capacity, entry.info.session_limit, dropped);
        }
    }
}
