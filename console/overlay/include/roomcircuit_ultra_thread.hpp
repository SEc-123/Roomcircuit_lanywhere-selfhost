#pragma once
#include <switch.h>
#include <cstddef>

namespace roomcircuit::ultra {
    struct Worker {
        Thread thread{};
        bool created = false;
        bool started = false;
        bool exited = false;
    };

    inline Result start(Worker& worker, void (*entry)(void *), void *arg, size_t stack) {
        if (worker.created) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        Result rc = threadCreate(&worker.thread, entry, arg, nullptr, stack, 0x2c, -2);
        if (R_FAILED(rc)) return rc;
        worker.created = true;
        rc = threadStart(&worker.thread);
        if (R_FAILED(rc)) {
            // threadStart failure leaves a created, unstarted handle.  If
            // closing fails, retain it for retry instead of losing ownership.
            if (R_SUCCEEDED(threadClose(&worker.thread))) worker.created = false;
            return rc;
        }
        worker.started = true;
        return 0;
    }

    inline Result release(Worker& worker) {
        if (!worker.created) return 0;
        if (worker.started && !worker.exited) {
            Result rc = threadWaitForExit(&worker.thread);
            if (R_FAILED(rc)) return rc;
            worker.exited = true;
        }
        Result rc = threadClose(&worker.thread);
        if (R_FAILED(rc)) return rc;
        worker.created = false;
        worker.started = false;
        worker.exited = false;
        return 0;
    }

    // There is no safe deadline after which a still-running worker can be
    // unloaded. Retain its stack, handles and captured data until released.
    inline void releaseEventually(Worker& worker) {
        while (R_FAILED(release(worker))) svcSleepThread(50'000'000);
    }
}
