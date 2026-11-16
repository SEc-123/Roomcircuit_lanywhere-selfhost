#include <cassert>
#include <cstddef>
#include "roomcircuit_ultra_thread.hpp"

static int fail_create, fail_start, fail_wait_count, fail_close_count, creates, starts, waits, closes, sleeps;
extern "C" {
Result threadCreate(Thread *thread, void (*)(void *), void *, void *, size_t stack, int priority, int cpu) {
    ++creates; assert(stack == 0x4000 && priority == 0x2c && cpu == -2);
    if (fail_create) return fail_create;
    thread->impl = reinterpret_cast<void *>(1); return 0;
}
Result threadStart(Thread *thread) { ++starts; assert(thread->impl); return fail_start; }
Result threadWaitForExit(Thread *thread) { ++waits; assert(thread->impl); if (fail_wait_count > 0) {--fail_wait_count; return 56;} return 0; }
Result threadClose(Thread *thread) { ++closes; assert(thread->impl); if (fail_close_count > 0) {--fail_close_count; return 78;} thread->impl = nullptr; return 0; }
void svcSleepThread(int64_t delay) { assert(delay == 50'000'000); ++sleeps; }
}
static void worker(void *) {}

int main() {
    roomcircuit::ultra::Worker w{};
    fail_create = 12;
    assert(roomcircuit::ultra::start(w, worker, nullptr, 0x4000) == 12 && !w.started);
    assert(roomcircuit::ultra::release(w) == 0);
    assert(creates == 1 && starts == 0 && waits == 0 && closes == 0);
    fail_create = 0; fail_start = 34; fail_close_count = 1;
    assert(roomcircuit::ultra::start(w, worker, nullptr, 0x4000) == 34 && !w.started);
    assert(w.created && !w.exited && w.thread.impl && closes == 1);
    assert(R_FAILED(roomcircuit::ultra::start(w, worker, nullptr, 0x4000)));
    assert(roomcircuit::ultra::release(w) == 0 && !w.created && !w.thread.impl);
    assert(starts == 1 && waits == 0 && closes == 2);
    fail_start = 0;
    assert(roomcircuit::ultra::start(w, worker, nullptr, 0x4000) == 0 && w.started);
    fail_wait_count = 2;
    assert(roomcircuit::ultra::release(w) == 56);
    assert(w.created && w.started && !w.exited && waits == 1 && closes == 2);
    fail_close_count = 2;
    roomcircuit::ultra::releaseEventually(w);
    assert(!w.created && !w.started && !w.exited && !w.thread.impl);
    assert(waits == 3 && closes == 5 && sleeps == 3);
    assert(roomcircuit::ultra::release(w) == 0 && waits == 3 && closes == 5);

    // Permanent failures retain ownership; callers must not unload the worker.
    assert(roomcircuit::ultra::start(w, worker, nullptr, 0x4000) == 0);
    fail_wait_count = 100;
    for (int i = 0; i < 5; ++i) {
        assert(roomcircuit::ultra::release(w) == 56);
        assert(w.created && w.started && !w.exited && w.thread.impl);
    }
    const int beforeClose = closes;
    fail_wait_count = 0; fail_close_count = 100;
    assert(roomcircuit::ultra::release(w) == 78 && w.exited);
    const int afterWait = waits;
    for (int i = 0; i < 5; ++i) {
        assert(roomcircuit::ultra::release(w) == 78);
        assert(w.created && w.started && w.exited && w.thread.impl && waits == afterWait);
    }
    assert(closes == beforeClose + 6);
    fail_close_count = 0;
    assert(roomcircuit::ultra::release(w) == 0 && !w.created && !w.thread.impl);
}
