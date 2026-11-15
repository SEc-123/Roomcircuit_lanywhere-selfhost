"""Exercise the real Switch cancellation guard with reused DNS handles."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
code = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <netdb.h>
#include <time.h>
#include "node_probe.h"
static atomic_uint cancellations, entered;
static atomic_bool permit_cancel, permit_lookup, finished;
static NodeProbeGuard guard;
static atomic_bool stopped;
static void pause_ms(void) {
    struct timespec delay = {0, 1000000};
    nanosleep(&delay, NULL);
}
static void until(atomic_bool *flag) {
    while (!atomic_load(flag)) pause_ms();
}
unsigned resolverGetCancelHandle(void) { return 123; }
unsigned resolverCancel(unsigned handle) {
    assert(handle == 123);
    atomic_fetch_add(&cancellations, 1);
    until(&permit_cancel);
    return 0;
}
static int simulated_lookup(const char *host, const char *service,
                            const struct addrinfo *hints, struct addrinfo **out) {
    (void)host; (void)service; (void)hints;
    *out = NULL;
    atomic_fetch_add(&entered, 1);
    until(&permit_lookup);
    return EAI_NONAME;
}
#define getaddrinfo simulated_lookup
#include "SOURCE_FILE"
#undef getaddrinfo
static void *lookup(void *unused) {
    (void)unused;
    assert(open_node("example.invalid", 1, NodeProbeUdp, node_probe_now_ms() + 30000, &stopped, &guard) == -1);
    atomic_store(&finished, true);
    return NULL;
}
static void *cancel(void *unused) {
    (void)unused;
    node_probe_guard_poll(&guard, true);
    return NULL;
}
int main(void) {
    pthread_t worker, controller;
    assert(!pthread_create(&worker, NULL, lookup, NULL));
    while (!atomic_load(&entered)) pause_ms();
    for (unsigned i = 0; i < 20; i++) node_probe_guard_poll(&guard, false);
    assert(!atomic_load(&cancellations));
    assert(!pthread_create(&controller, NULL, cancel, NULL));
    while (!atomic_load(&cancellations)) pause_ms();
    atomic_store(&permit_lookup, true);
    for (unsigned i = 0; i < 20; i++) pause_ms();
    assert(!atomic_load(&finished)); // retiring cannot race a still-running cancel
    atomic_store(&permit_cancel, true);
    pthread_join(controller, NULL);
    pthread_join(worker, NULL);
    for (unsigned i = 0; i < 20; i++) node_probe_guard_poll(&guard, true);
    assert(atomic_load(&cancellations) == 1);
    atomic_store(&permit_lookup, false);
    atomic_store(&finished, false);
    assert(!pthread_create(&worker, NULL, lookup, NULL));
    while (atomic_load(&entered) < 2) pause_ms();
    node_probe_guard_poll(&guard, false);
    assert(atomic_load(&cancellations) == 1 && atomic_load(&guard.handle) == 123);
    atomic_store(&permit_lookup, true);
    pthread_join(worker, NULL);
    assert(!atomic_load(&guard.handle));
    puts("DNS guard: single cancellation, retirement barrier and reused-handle deadline passed");
}
'''
with tempfile.TemporaryDirectory(prefix='node-cancel-') as directory:
    directory = Path(directory)
    header = directory / 'switch/runtime/resolver.h'
    header.parent.mkdir(parents=True)
    header.write_text('#pragma once\nunsigned resolverGetCancelHandle(void);\nunsigned resolverCancel(unsigned);\n')
    fixture = directory / 'test.c'
    fixture.write_text(code.replace('SOURCE_FILE', str(root / 'app/source/node_probe.c')))
    binary = directory / 'test'
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=c11', '-D__SWITCH__', '-Wall', '-Wextra', '-Werror', '-pthread',
        '-I'+str(directory), '-I'+str(root/'app/source'), str(fixture), '-o', str(binary)
    ], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
