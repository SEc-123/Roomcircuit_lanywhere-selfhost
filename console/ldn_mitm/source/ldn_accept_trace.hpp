// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stratosphere.hpp>

namespace ams::log {
    struct LdnAcceptInfo {
        u64 pid{}, title{};
        u32 forward_pointer_size{}, pointer_capacity{}, session_limit{};
    };

    // Stage names are static literals. Producers share a short in-memory critical section with the writer.
    // The single SM query thread only publishes a snapshot; the drain thread writes it.
    void RecordLdnQuery(u64 pid, u64 title);
    void RecordLdnAccept(const char *stage, const LdnAcceptInfo &info, u32 result = 0);
    void DrainLdnAcceptTraceUntilExit(os::ThreadType *service_thread);
}
