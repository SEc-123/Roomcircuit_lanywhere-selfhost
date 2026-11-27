// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stratosphere.hpp>

namespace ams::log {
    struct LdnAcceptInfo {
        u64 pid{}, title{};
        u32 forward_pointer_size{}, pointer_capacity{}, session_limit{};
    };

    // Stage names are static literals. No producer touches the filesystem or waits for the writer.
    void RecordLdnAccept(const char *stage, const LdnAcceptInfo &info, u32 result = 0);
    void DrainLdnAcceptTraceUntilExit(os::ThreadType *service_thread);
}
